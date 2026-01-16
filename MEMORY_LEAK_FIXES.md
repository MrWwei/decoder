# 内存泄漏修复报告

## 发现的问题

### 🔴 严重问题

#### 1. 递归调用导致的栈溢出和内存泄漏
**位置**: `LocalVideoReader::readFrame()`  
**问题**: 当视频到达末尾时，函数递归调用自己，导致无限递归和栈溢出
```cpp
// 错误代码
av_seek_frame(formatContext_, videoStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
avcodec_flush_buffers(codecContext_);
return readFrame(outMat);  // ❌ 递归调用！
```

**修复**: 改为循环结构
```cpp
// 修复后
while (!stop_.load()) {
    while (av_read_frame(formatContext_, packet_) >= 0) {
        // ... 处理帧
    }
    // 到达文件末尾，循环播放
    if (!stop_.load()) {
        av_seek_frame(formatContext_, videoStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecContext_);
        // 继续外层循环从头读取
    }
}
```

#### 2. 析构函数未释放资源
**位置**: `Decoder::~Decoder()`  
**问题**: `app_ctx_.frame` 在析构时未被释放

```cpp
// 错误代码
~Decoder() {
    stop();
    if (worker_->joinable())
        worker_->join();
    // ❌ 未释放 app_ctx_.frame
}
```

**修复**: 添加资源清理
```cpp
// 修复后
~Decoder() {
    stop();
    if (worker_ && worker_->joinable()) {
        worker_->join();
    }
    
    // ✅ 清理 frame
    if (app_ctx_.frame) {
        delete app_ctx_.frame;
        app_ctx_.frame = nullptr;
    }
    
    // ✅ 停止本地视频读取器
    if (local_video_reader_) {
        local_video_reader_->stopReading();
        local_video_reader_.reset();
    }
}
```

#### 3. stop() 函数未停止本地视频读取
**位置**: `Decoder::stop()`  
**问题**: 未停止 LocalVideoReader，可能导致线程继续运行

**修复**: 添加停止调用
```cpp
int Decoder::stop() {
    stop_.store(true);
    app_ctx_.stop = true;
    
    // ✅ 停止本地视频读取器
    if (local_video_reader_) {
        local_video_reader_->stopReading();
    }
    
    // ... 其他清理代码
}
```

---

## 内存管理检查清单

### ✅ 已修复的问题

1. **递归调用** - 改为循环结构
2. **析构函数** - 添加 frame 和 local_video_reader 清理
3. **stop 函数** - 添加本地视频读取器停止
4. **停止检查** - 在循环中添加 stop 标志检查

### ✅ 已验证正确的内存管理

1. **get_frame() 返回的内存** - 调用者负责 free()，文档已说明
2. **帧栈清理** - clear_frame_stack() 正确释放所有帧
3. **YUV转BGR** - malloc 的内存由调用者释放
4. **FFmpeg 资源** - LocalVideoReader::cleanup() 正确释放

---

## 内存使用建议

### 对于使用者

1. **必须释放 get_frame() 返回的数据**
```cpp
auto mat_info = decoder->get_frame();
if (!mat_info.empty()) {
    unsigned char* data = (unsigned char*)mat_info[0];
    // 使用数据...
    free(data);  // ⚠️ 必须调用！
}
```

2. **及时调用 stop()**
```cpp
// 处理完毕后立即停止
decoder->stop();
releaseDecoder(decoder);
```

3. **本地文件模式的建议**
- 本地文件采用同步模式，不缓存帧
- 调用 get_frame() 时才解码，内存占用稳定
- 处理速度可以慢于视频帧率

### 对于开发者

1. **使用智能指针** - 已使用 shared_ptr 管理资源
2. **RAII 原则** - LocalVideoReader 使用 RAII 管理 FFmpeg 资源
3. **避免递归** - 用循环代替递归调用
4. **原子操作** - 使用 atomic 保证线程安全
5. **及时清理** - 析构函数和 stop() 都要清理资源

---

## 内存占用预期

### RTSP 流模式
- **帧队列**: 1帧（MAX_STACK_SIZE = 1）
- **解码缓冲**: ~width × height × 1.5 字节（YUV）
- **转换缓冲**: ~width × height × 3 字节（BGR）
- **总计**: 约 width × height × 4.5 字节

### 本地文件模式
- **无帧队列**: 0 字节
- **解码缓冲**: ~width × height × 1.5 字节（临时）
- **转换缓冲**: ~width × height × 3 字节
- **总计**: 约 width × height × 4.5 字节（单帧）

### 示例（1080p）
- 1920×1080 分辨率
- RTSP: ~8.7 MB（1帧队列）
- 本地: ~8.7 MB（单帧处理）

---

## 运行时监控

### 检查内存泄漏的方法

```bash
# 使用 valgrind 检查
valgrind --leak-check=full --show-leak-kinds=all ./your_program

# 使用 top 监控内存
top -p $(pidof your_program)

# 查看进程内存映射
cat /proc/$(pidof your_program)/status | grep -i vmsize
```

### 正常的内存增长

- **启动阶段**: 加载库和初始化资源
- **首帧解码**: 分配解码缓冲区
- **稳定运行**: 内存应保持稳定，不持续增长

### 异常的内存增长

如果内存持续增长，检查：
1. 是否忘记 free(get_frame() 返回的数据)
2. 是否创建了多个实例但未释放
3. 是否有异常导致析构函数未被调用

---

## 测试建议

```cpp
// 测试代码示例
void test_memory_leak() {
    for (int i = 0; i < 1000; i++) {
        IDecoder* decoder = createDecoder();
        decoder->init(0, 1, 5000);
        decoder->start_pull("test.mp4", 0, 1);
        
        // 处理100帧
        for (int j = 0; j < 100; j++) {
            auto mat_info = decoder->get_frame();
            if (!mat_info.empty()) {
                unsigned char* data = (unsigned char*)mat_info[0];
                // 模拟处理...
                free(data);  // 必须释放
            }
        }
        
        decoder->stop();
        releaseDecoder(decoder);
        
        // 每100次迭代检查一次内存
        if (i % 100 == 0) {
            std::cout << "Iteration " << i << " - Check memory usage" << std::endl;
        }
    }
}
```

---

## 版本历史

### v2.1 (2026-01-15)
- 🔧 修复递归调用导致的栈溢出
- 🔧 修复析构函数未释放 frame 资源
- 🔧 添加本地视频读取器停止逻辑
- 🔧 改进停止标志检查
- 📝 添加内存管理文档

### v2.0 (2026-01-15)
- ✨ 添加本地视频文件支持
- ✨ 双模式处理（RTSP异步/本地同步）

---

## 总结

所有已知的内存泄漏问题已修复。关键改进：

1. ✅ 消除递归调用
2. ✅ 完善析构函数
3. ✅ 改进停止流程
4. ✅ 添加停止检查

建议定期使用内存检测工具（valgrind, AddressSanitizer）验证代码。
