# 统一视频解码器 - RTSP & 本地文件支持

## 概述

`xtkj_decoder` 提供统一接口，同时支持：
- **RTSP/RTMP 网络视频流** - 通过 mk_mediakit 拉流
- **本地视频文件** - 通过 FFmpeg 直接读取（MP4/AVI/MKV等格式）

接口会自动识别输入是网络流还是本地文件路径。

---

## 主要特性

✅ **统一接口** - 无需关心视频源类型，使用相同的API  
✅ **自动识别** - 根据路径自动判断RTSP流或本地文件  
✅ **双模式处理**:
  - **RTSP流**: 异步模式，后台线程解码+帧队列缓存，支持丢帧和重连
  - **本地文件**: 同步阻塞模式，调用 `get_frame()` 时才解码，无缓存无丢帧
✅ **软硬解码** - RTSP流支持MPP硬件解码（可选）  
✅ **帧间隔控制** - RTSP流支持跳帧处理，降低CPU负载  
✅ **线程安全** - 内部使用线程池和原子操作  
✅ **内存管理** - RAII和智能指针，避免内存泄漏  
✅ **自动重连** - RTSP流断线智能检测并自动重连（v2.5新增） 🆕  

---

## 🆕 v2.5 新功能：RTSP流自动重连

### 智能流中断检测

系统提供双重检测机制，时刻监控RTSP流状态：
- **超时检测**：连续获取帧失败时触发
- **后台监控**：独立线程持续检查流健康状态

### auto_reopen参数

通过`auto_reopen`参数控制是否自动重连（默认启用）：

```cpp
// 启用自动重连（默认）
decoder->set_auto_reopen(true);

// 禁用自动重连（需要手动处理）
decoder->set_auto_reopen(false);

// 查询当前设置
bool enabled = decoder->get_auto_reopen();
```

### 使用示例

```cpp
IDecoder* decoder = createDecoder();
decoder->init(10000);

// RTSP流会自动重连，无需特殊处理
decoder->start_pull("rtsp://192.168.1.100:554/stream");

while (running) {
    auto frame = decoder->get_frame();
    if (!frame.empty()) {
        // 处理帧数据
        uint8_t* bgr_data = (uint8_t*)frame[0];
        // ... 使用数据 ...
        free(bgr_data);
    }
    // 流中断时系统会自动重连，应用层无感知
}
```

### 详细文档

- 📖 [完整功能说明](RTSP_AUTO_RECONNECT_README.md)
- 💻 [使用示例代码](example_auto_reconnect.cpp)
- 📋 [快速参考](QUICK_REFERENCE.md)
- 📝 [变更日志](CHANGELOG.md)

---

## 接口说明

### 1. 创建解码器

```cpp
#include "xtkj_decoder.h"
using namespace xtkj;

IDecoder* decoder = createDecoder();
```

### 2. 初始化

```cpp
/**
 * @param decode_thread_num  解码器实例索引（多实例时使用不同编号）
 * @param frame_interval     帧间隔 (1=每帧, 2=隔一帧)
 * @param timeout_ms         获取帧超时时间（毫秒）
 */
decoder->init(0, 1, 5000);
```

### 3. 启动视频源

```cpp
/**
 * @param video_path  视频路径
 *                    - RTSP流: "rtsp://192.168.1.100:8554/live"
 *                    - 本地文件: "/path/to/video.mp4"
 * @param is_mpp      是否使用MPP硬解（仅RTSP，0=软解，1=硬解）
 * @param interval    是否启用帧间隔（0=禁用，1=启用）
 */
int ret = decoder->start_pull(video_path, 0, 1);
```

### 4. 获取帧数据

```cpp
while (true) {
    // 返回: [BGR数据地址, 宽度, 高度, 时间戳(毫秒)]
    // 
    // 对于RTSP流：异步模式，从队列获取已解码的帧，可能超时
    //            时间戳为视频流从开始的累积PTS时间（毫秒）
    // 对于本地文件：同步阻塞模式，此调用会立即解码下一帧并返回
    //            时间戳为视频从开始的播放时间（毫秒），循环播放时重置为0
    auto mat_info = decoder->get_frame();
    
    if (mat_info.empty()) {
        // 超时或出错（主要针对RTSP流）
        // 本地文件通常不会返回空，除非到达文件末尾
        continue;
    }
    
    unsigned char* bgr_data = (unsigned char*)mat_info[0];
    int width = mat_info[1];
    int height = mat_info[2];
    long long timestamp_ms = mat_info[3];  // 视频帧PTS时间戳（毫秒）
    
    // 使用OpenCV处理
    cv::Mat frame(height, width, CV_8UC3, bgr_data);
    
    // 处理帧...
    std::cout << "Frame timestamp: " << timestamp_ms << " ms" << std::endl;
    
    // 释放内存（重要！）
    free(bgr_data);
}
```

### 5. 停止和释放

```cpp
decoder->stop();
releaseDecoder(decoder);
```

---

## 使用示例

### 示例1：处理RTSP流

```cpp
IDecoder* decoder = createDecoder();
decoder->init(0, 1, 5000);

// 自动识别RTSP流
decoder->start_pull("rtsp://192.168.1.100:8554/live", 0, 1);

int count = 0;
while (count < 100) {
    auto mat_info = decoder->get_frame();
    if (!mat_info.empty()) {
        unsigned char* data = (unsigned char*)mat_info[0];
        int w = mat_info[1], h = mat_info[2];
        
        cv::Mat frame(h, w, CV_8UC3, data);
        // 处理帧...
        
        free(data);
        count++;
    }
}

decoder->stop();
releaseDecoder(decoder);
```

### 示例2：处理本地视频文件

```cpp
IDecoder* decoder = createDecoder();
decoder->init(0, 1, 5000);

// 自动识别本地文件（同步阻塞模式）
decoder->start_pull("/home/user/video.mp4", 0, 1);

// 本地文件模式：
// - get_frame() 会阻塞直到解码完成
// - 无帧缓存，无丢帧
// - 按视频原始顺序逐帧处理
while (true) {
    auto mat_info = decoder->get_frame();
    if (mat_info.empty()) {
        // 本地文件通常不会返回空，除非文件错误
        break;
    }
    
    unsigned char* data = (unsigned char*)mat_info[0];
    int w = mat_info[1], h = mat_info[2];
    
    cv::Mat frame(h, w, CV_8UC3, data);
    // 处理...（可以慢慢处理，不用担心丢帧）
    
    free(data);
}

decoder->stop();
releaseDecoder(decoder);
```

### 示例3：完整的错误处理

```cpp
IDecoder* decoder = createDecoder();

if (decoder->init(0, 1, 5000) != 0) {
    std::cerr << "Init failed" << std::endl;
    releaseDecoder(decoder);
    return -1;
}

std::string video_path = "rtsp://example.com/stream";
// std::string video_path = "./video.mp4";  // 或本地文件

if (decoder->start_pull(video_path, 0, 1) != 0) {
    std::cerr << "Start failed: " << video_path << std::endl;
    releaseDecoder(decoder);
    return -1;
}

int null_count = 0;
while (true) {
    auto mat_info = decoder->get_frame();
    
    if (mat_info.empty()) {
        null_count++;
        if (null_count > 10) {
            std::cerr << "Too many null frames" << std::endl;
            break;
        }
        continue;
    }
    
    null_count = 0;
    
    // 处理帧...
    unsigned char* data = (unsigned char*)mat_info[0];
    free(data);
}

decoder->stop();
releaseDecoder(decoder);
```

---

## 编译

### 依赖项

- **OpenCV** (opencv_world)
- **FFmpeg** (avformat, avcodec, avutil, swscale)
- **mk_mediakit** (用于RTSP流)
- **C++11** 或更高版本

### CMake 配置示例

```cmake
cmake_minimum_required(VERSION 3.10)
project(VideoDecoder)

set(CMAKE_CXX_STANDARD 11)

# 查找依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
)

# OpenCV
set(ThirdParty $ENV{THIRD_PARTY})
add_library(opencv_lib SHARED IMPORTED)
set_target_properties(opencv_lib PROPERTIES 
    IMPORTED_LOCATION ${ThirdParty}/opencv-4.5.4_video/build/install/lib/libopencv_world.so
)
include_directories(${ThirdParty}/opencv-4.5.4_video/build/install/include/opencv4)

# mk_mediakit
add_library(mk_api SHARED IMPORTED)
set_target_properties(mk_api PROPERTIES 
    IMPORTED_LOCATION ${ThirdParty}/ZLMediaKit/lib/libmk_api.so
)

# 编译
add_executable(example_unified example_unified.cpp src/xtkj_decoder.cpp)
target_link_libraries(example_unified
    opencv_lib
    mk_api
    PkgConfig::LIBAV
    pthread
)
```

### 编译命令

```bash
mkdir -p build && cd build
cmake ..
make -j4
```

---

## 运行

### RTSP流

```bash
./example_unified rtsp://192.168.1.100:8554/live
```

### 本地文件

```bash
./example_unified /path/to/video.mp4
./example_unified ./test.avi
```

---

## 工作模式说明

### RTSP 流模式（异步）

```
[RTSP Server] → [mk_mediakit] → [解码线程] → [帧队列(缓存1帧)] → get_frame()
                                    ↓
                            后台持续解码
                            可能丢帧
```

**特点**：
- 后台线程持续拉流解码
- 使用帧队列缓存（默认1帧）
- `get_frame()` 从队列取帧，有超时机制
- 支持帧间隔（跳帧）减轻负载
- 断线自动重连

### 本地文件模式（同步）

```
[本地视频文件] → get_frame() 调用 → [立即解码] → 返回BGR数据
                        ↓
                   阻塞等待解码完成
                   不丢帧，不缓存
```

**特点**：
- 按需解码：仅在调用 `get_frame()` 时解码
- 同步阻塞：调用会等待解码完成
- 无帧队列：不缓存任何帧
- 无丢帧：严格按顺序处理每一帧
- 循环播放：到达文件末尾自动跳回开头

**对比**：

| 特性 | RTSP流 | 本地文件 |
|------|--------|----------|
| 解码模式 | 异步（后台线程） | 同步（按需解码） |
| 帧缓存 | 有（1帧队列） | 无缓存 |
| 丢帧 | 可能丢帧 | 不丢帧 |
| get_frame() | 非阻塞（有超时） | 阻塞（直到解码完成） |
| 处理速度 | 实时要求 | 可以慢速处理 |
| 适用场景 | 实时监控、直播 | 离线分析、逐帧处理 |

---

## 注意事项

### 内存管理

⚠️ **重要**: `get_frame()` 返回的BGR数据地址需要调用 `free()` 释放！

```cpp
auto mat_info = decoder->get_frame();
if (!mat_info.empty()) {
    unsigned char* data = (unsigned char*)mat_info[0];
    // 使用数据...
    free(data);  // 必须释放！
}
```

### 返回值格式

`get_frame()` 返回 `vector<long long>`，包含4个元素：

| 索引 | 含义 | 类型 | 说明 |
|------|------|------|------|
| 0 | BGR数据地址 | void* | 需要转换为 `unsigned char*`，使用后必须调用 `free()` |
| 1 | 图像宽度 | int | 像素单位 |
| 2 | 图像高度 | int | 像素单位 |
| 3 | 帧时间戳 | long long | **毫秒单位**<br>RTSP流：从流开始的累积PTS时间<br>本地文件：从视频开始的播放时间（0开始，循环时重置） |

### 视频源识别规则

接口通过路径前缀自动识别：

- **RTSP流**: `rtsp://`, `rtmp://`, `http://`, `https://`
- **本地文件**: 其他所有路径（相对或绝对）

### 性能建议

**针对 RTSP 流**：
1. **帧间隔**: 如果不需要处理每一帧，设置 `frame_interval=2` 或更高
2. **超时时间**: 建议 5000-10000ms
3. **实时处理**: 处理速度要跟上视频帧率，否则会丢帧

**针对本地文件**：
1. **无需担心丢帧**: 可以慢速处理每一帧，系统会等待
2. **处理耗时**: `get_frame()` 的调用时间 = 解码时间 + 等待时间
3. **超时设置无效**: 本地文件模式不使用超时机制

**多实例**：
- 可以创建多个解码器实例，使用不同的 `decode_thread_num`
- 每个实例独立工作，互不影响

---

## 故障排查

### 问题1: 获取帧一直返回空

**可能原因**:
- RTSP流地址错误或无法访问
- 本地文件路径错误或文件损坏
- 超时时间设置过短

**解决方法**:
- 检查日志输出 `[ERROR]` 信息
- 验证视频源是否可访问
- 增加超时时间

### 问题2: 内存泄漏

**原因**: 未释放 `get_frame()` 返回的数据

**解决**:
```cpp
auto mat_info = decoder->get_frame();
if (!mat_info.empty()) {
    unsigned char* data = (unsigned char*)mat_info[0];
    // ... 使用数据
    free(data);  // 必须释放
}
```

### 问题3: 编译错误 - FFmpeg符号未定义

**解决**: 确保链接了FFmpeg库
```cmake
target_link_libraries(your_target
    avformat avcodec avutil swscale
)
```

---

## API 参考

### IDecoder 接口

```cpp
class IDecoder {
public:
    virtual int init(int decode_thread_num, 
                    int frame_interval = 1,
                    int timeout_ms = 200) = 0;
                    
    virtual vector<long long> get_frame() = 0;
    
    virtual int start_pull(string video_path, 
                          int is_mpp = 1, 
                          int interval = 1) = 0;
                          
    virtual int stop() = 0;
    
    virtual int get_null_times() = 0;
    
    virtual string get_rtsp() = 0;
    
    virtual ~IDecoder() noexcept = default;
};
```

### 工厂函数

```cpp
// 创建解码器实例
extern "C" IDecoder* createDecoder();

// 释放解码器实例
extern "C" void releaseDecoder(IDecoder* pDecoder);
```

---

## 更新日志

### v2.0 (2026-01-15)
- ✨ 新增本地视频文件支持
- ✨ 自动识别RTSP流和本地文件
- 🔧 改进内存管理和线程安全
- 🔧 统一日志输出格式
- 📝 完善文档和示例

### v1.0
- 基础RTSP流支持
- MPP硬件解码支持

---

## 许可证

详见项目LICENSE文件

## 作者

wtwei

## 贡献

欢迎提交Issue和Pull Request
