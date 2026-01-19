# 内存安全分析报告

## 🔴 已发现的严重问题及修复

### 1. ✅ **已修复：线程重复启动导致的资源泄漏**
**问题位置**: `Decoder::start_pull()` 和 `Decoder::stop()`

**原问题**:
- 多次调用 `start_pull()` 而不调用 `stop()`，会创建新的 `worker_` 线程但不等待旧线程结束
- `worker_` 是 `shared_ptr<thread>`，重新赋值会导致旧线程对象析构但线程仍在运行

**修复方案**:
```cpp
// start_pull() 中添加检查
if (worker_ && worker_->joinable()) {
    log_info("Decoder already running, stopping first...");
    stop();
}

// stop() 中正确清理
if (worker_ != nullptr && worker_->joinable()) {
    worker_->join();
    worker_.reset();  // 重置 shared_ptr
}
```

---

### 2. ✅ **已修复：get_frame() 中的递归重连风险**
**问题位置**: `Decoder::get_frame()` 第881-883行

**原问题**:
```cpp
if (null_times > failed_times_) {
    stop();        // 可能等待当前 get_frame() 完成
    start_pull();  // 在 get_frame() 内部重启，递归风险
    return {};
}
```
- 在 `get_frame()` 中调用 `stop()` + `start_pull()` 可能导致：
  - 死锁（等待自己完成）
  - 递归调用栈增长
  - 资源竞争

**修复方案**:
```cpp
// 只标记失败状态，让上层决定是否重连
if (null_times > failed_times_) {
    decoder_status_.store(DECODER_STATUS_FAILED);
    // 记录日志但不自动重连
    return {};
}
```

---

### 3. ✅ **已修复：stop() 后回调函数继续分配内存**
**问题位置**: `onGetFrame()` 和 `mpp_decoder_frame_callback()`

**原问题**:
- 调用 `stop()` 后，网络层回调可能仍在执行
- 继续 `new image_frame_t()` 和 `malloc()` 分配内存
- 这些内存推入 `frame_stack_`，但可能永远不会被 `get_frame()` 取走

**修复方案**:
```cpp
// 在回调开始处检查
if (ctx->stop || !ctx->decoder_instance) {
    return;  // 直接返回，不分配任何内存
}
```

---

## ⚠️ 仍需注意的潜在问题

### 4. **get_frame() 返回的内存必须由调用者释放**
**问题位置**: 
- 第835行: `unsigned char* bgr_data = (unsigned char*)malloc(data_size);`
- 第907行: `void* mdata = YV12ToBGR24_OpenCV(...);` (内部也是 malloc)

**风险**:
```cpp
vector<long long> mat_info = decoder->get_frame();
// mat_info[0] 是 malloc 分配的内存地址
// 调用者必须记得 free(reinterpret_cast<void*>(mat_info[0]));
// 如果忘记，会持续泄漏
```

**建议**:
1. ✅ **已在文档中明确说明** (见 main.cpp 第74行和167行)
2. 考虑使用 RAII 包装器（例如返回 `std::unique_ptr<unsigned char[]>`）
3. 或者提供 `release_frame(void*)` 接口统一释放

---

### 5. **LocalVideoReader 循环播放的内存压力**
**问题位置**: `LocalVideoReader::readFrame()` 第255-263行

**潜在风险**:
- 循环播放时不断调用 `av_seek_frame()` 和解码
- 虽然 FFmpeg 内部有内存管理，但频繁 seek 可能导致缓存增长
- `cv::Mat` 的临时对象在高频调用时可能产生内存压力

**当前缓解措施**:
- ✅ 已添加 `set_loop_playback(false)` 选项
- ✅ 支持播放一次后自动停止

**建议**:
- 监控长时间运行时的内存使用
- 考虑添加循环次数限制
- 定期检查 FFmpeg 内部缓存状态

---

### 6. **frame_stack_ 的大小限制**
**问题位置**: `DecoderConfig::MAX_STACK_SIZE = 1`

**当前设计**:
```cpp
while (decoder_obj->frame_stack_.size() >= DecoderConfig::MAX_STACK_SIZE) {
    // 清理最旧的帧
    image_frame_t* frame_item = decoder_obj->frame_stack_.top();
    decoder_obj->frame_stack_.pop();
    // ... free memory
}
```

**优点**:
- ✅ 限制了最大内存使用（只保留1帧）
- ✅ 防止帧积累导致内存爆炸

**建议**: 当前设计良好，但需要确保 `get_frame()` 调用频率 ≥ 帧到达频率

---

### 7. **YV12ToBGR24 转换函数的内存分配**
**问题位置**: 
- 第720行: `YV12ToBGR24_OpenCV_FFMPEG()`
- 第739行: `YV12ToBGR24_OpenCV()`

**当前实现**:
```cpp
unsigned char* pBGR24 = (unsigned char*)malloc(width * height * 3);
// ... 转换
return pBGR24;  // 调用者必须 free
```

**风险**: 与问题4相同，依赖调用者记得释放

**当前缓解**: 
- ✅ 在 `get_frame()` 中使用后立即释放原始 YUV
- ✅ main.cpp 中有正确的 free 调用

---

## 📊 内存使用估算

### 每帧内存消耗
假设视频分辨率 1920x1080:

1. **RTSP 流**:
   - YUV 数据: ~3MB (1920 × 1080 × 1.5)
   - BGR 转换后: ~6MB (1920 × 1080 × 3)
   - frame_stack_: 最多保留1帧 (~3MB)
   - **峰值**: ~9MB/帧

2. **本地视频**:
   - 直接读取 BGR: ~6MB
   - 无缓存队列
   - **峰值**: ~6MB/帧

### 多实例场景
- 16个 Decoder 实例同时运行
- RTSP 流: 16 × 9MB = **~144MB**
- 本地视频: 16 × 6MB = **~96MB**

---

## ✅ 最佳实践建议

### 使用代码模板

#### 1. 正确使用 Decoder
```cpp
// 创建
IDecoder* decoder = xtkj::createDecoder();
decoder->init(0);

// 启动（自动检查是否需要先停止）
decoder->start_pull("rtsp://...");

// 获取帧
while (true) {
    auto mat_info = decoder->get_frame();
    if (mat_info.empty()) {
        // 检查状态
        if (decoder->get_status() == DECODER_STATUS_FAILED) {
            // 连接失败，重连
            decoder->stop();
            decoder->start_pull("rtsp://...");
        }
        continue;
    }
    
    // 使用帧数据
    void* bgr_data = reinterpret_cast<void*>(mat_info[0]);
    int width = mat_info[1];
    int height = mat_info[2];
    
    // ... 处理图像
    
    // ⚠️ 重要：必须释放内存
    free(bgr_data);
}

// 清理
decoder->stop();
xtkj::releaseDecoder(decoder);
```

#### 2. 本地视频播放一次
```cpp
decoder->set_loop_playback(false);  // 播放一次后停止
decoder->start_pull("video.mp4");

while (decoder->get_status() == DECODER_STATUS_OPENED) {
    auto mat_info = decoder->get_frame();
    if (mat_info.empty()) {
        break;  // 视频结束
    }
    
    // 处理并释放
    void* bgr_data = reinterpret_cast<void*>(mat_info[0]);
    // ... 使用
    free(bgr_data);
}

decoder->stop();
```

#### 3. 监控内存使用
```cpp
// 定期检查解码器状态
int null_times = decoder->get_null_times();
if (null_times > 10) {
    log_warning("Connection unstable, null frames: " + std::to_string(null_times));
}

// 检查状态
int status = decoder->get_status();
// DECODER_STATUS_IDLE (0)
// DECODER_STATUS_OPENING (1)
// DECODER_STATUS_OPENED (2)
// DECODER_STATUS_FAILED (3)
```

---

## 🔍 检测内存泄漏的方法

### Linux (Valgrind)
```bash
valgrind --leak-check=full --show-leak-kinds=all ./VideoDecoder
```

### Linux (AddressSanitizer)
```bash
g++ -fsanitize=address -g main.cpp -o VideoDecoder
./VideoDecoder
```

### 运行时监控
```bash
# 监控进程内存
watch -n 1 'ps aux | grep VideoDecoder'

# 详细内存映射
cat /proc/$(pidof VideoDecoder)/status | grep -i vmrss
```

---

## 📝 修改日志

### 2026-01-16
1. ✅ 修复 `start_pull()` 重复调用导致的线程泄漏
2. ✅ 修复 `stop()` 中 worker_ 线程未正确清理
3. ✅ 移除 `get_frame()` 中的递归重连逻辑
4. ✅ 添加回调函数中的 stop 标志检查
5. ✅ 改进 `stack_cond_.notify_all()` 调用时机
6. ✅ 将帧队列和锁从全局改为实例成员变量

### 待办事项
- [ ] 考虑为 `get_frame()` 返回值添加 RAII 包装
- [ ] 添加内存使用统计接口
- [ ] 增加单元测试覆盖内存泄漏场景
