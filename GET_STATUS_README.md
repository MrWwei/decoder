# get_status 功能说明

## 功能概述

`get_status()` 方法用于获取解码器的当前打开状态，可以帮助应用程序监控解码器的运行状态。

## 状态枚举定义

```cpp
enum DecoderStatus {
    DECODER_STATUS_IDLE = 0,     // 未打开/空闲状态
    DECODER_STATUS_OPENING = 1,  // 正在打开中
    DECODER_STATUS_OPENED = 2,   // 正常打开/运行中
    DECODER_STATUS_FAILED = 3    // 打开失败
};
```

## 状态说明

| 状态值 | 状态名称 | 说明 |
|--------|----------|------|
| 0 | DECODER_STATUS_IDLE | 解码器未打开或已停止，处于空闲状态 |
| 1 | DECODER_STATUS_OPENING | 正在打开视频源（RTSP流或本地文件），连接建立中 |
| 2 | DECODER_STATUS_OPENED | 视频源已成功打开，解码器正常运行中 |
| 3 | DECODER_STATUS_FAILED | 打开视频源失败，无法建立连接或文件不存在 |

## 状态转换流程

```
[初始化] → IDLE (0)
    ↓
[调用 start_pull()] → OPENING (1)
    ↓
    ├→ [成功] → OPENED (2)
    │       ↓
    │   [调用 stop()] → IDLE (0)
    │       ↓
    │   [连接断开] → FAILED (3)
    │
    └→ [失败] → FAILED (3)
            ↓
        [调用 stop()] → IDLE (0)
```

## API 使用方法

```cpp
// 获取解码器状态
int status = decoder->get_status();

// 判断状态
switch (status) {
    case DECODER_STATUS_IDLE:
        // 解码器空闲，可以启动新的拉流
        break;
    case DECODER_STATUS_OPENING:
        // 正在连接中，等待连接完成
        break;
    case DECODER_STATUS_OPENED:
        // 正常运行，可以获取帧数据
        break;
    case DECODER_STATUS_FAILED:
        // 连接失败，需要重试或停止
        break;
}
```

## 实际应用场景

### 1. 连接前检查状态

```cpp
int status = decoder->get_status();
if (status == DECODER_STATUS_IDLE) {
    // 只有在空闲状态才能启动新的连接
    decoder->start_pull("rtsp://...");
} else {
    std::cout << "解码器正忙，当前状态: " << status << std::endl;
}
```

### 2. 等待连接建立

```cpp
decoder->start_pull("rtsp://192.168.1.100:554/stream");

// 等待连接建立（最多10秒）
int timeout = 0;
while (decoder->get_status() == DECODER_STATUS_OPENING && timeout < 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    timeout++;
}

if (decoder->get_status() == DECODER_STATUS_OPENED) {
    std::cout << "连接成功！" << std::endl;
} else {
    std::cout << "连接失败或超时" << std::endl;
}
```

### 3. 自动重连机制

```cpp
void monitor_and_reconnect(IDecoder* decoder, const std::string& url) {
    while (true) {
        int status = decoder->get_status();
        
        if (status == DECODER_STATUS_FAILED) {
            std::cout << "检测到连接失败，尝试重连..." << std::endl;
            decoder->stop();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            decoder->start_pull(url);
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
```

### 4. 获取帧前的状态检查

```cpp
if (decoder->get_status() == DECODER_STATUS_OPENED) {
    auto frame_info = decoder->get_frame();
    if (!frame_info.empty()) {
        // 处理帧数据
        process_frame(frame_info);
        
        // 释放内存
        if (frame_info[0] != 0) {
            free((void*)frame_info[0]);
        }
    }
} else {
    std::cout << "解码器未就绪，无法获取帧" << std::endl;
}
```

### 5. 状态监控和日志记录

```cpp
void log_decoder_status(IDecoder* decoder) {
    static int last_status = -1;
    int current_status = decoder->get_status();
    
    if (current_status != last_status) {
        const char* status_names[] = {
            "空闲", "正在打开", "已打开", "失败"
        };
        
        std::cout << "[" << get_timestamp() << "] "
                  << "解码器状态变更: " 
                  << status_names[last_status] << " → " 
                  << status_names[current_status] << std::endl;
        
        last_status = current_status;
    }
}
```

## 状态更新时机

### RTSP 流
- **IDLE → OPENING**: 调用 `start_pull()` 时
- **OPENING → OPENED**: ZLMediaKit 回调 `on_mk_play_event_func()` 成功（err_code == 0）时
- **OPENING → FAILED**: ZLMediaKit 回调 `on_mk_play_event_func()` 失败（err_code != 0）时
- **OPENED → FAILED**: ZLMediaKit 回调 `on_mk_shutdown_func()` 连接中断时
- **任何状态 → IDLE**: 调用 `stop()` 时

### 本地文件
- **IDLE → OPENING**: 调用 `start_pull()` 时
- **OPENING → OPENED**: 文件成功打开时
- **OPENING → FAILED**: 文件打开失败时
- **任何状态 → IDLE**: 调用 `stop()` 时

## 注意事项

1. **线程安全**: `get_status()` 使用 `std::atomic` 实现，可以安全地在多线程环境中调用
2. **状态延迟**: 状态变化可能有轻微延迟，特别是在 OPENING 状态时
3. **重复调用**: 频繁调用 `get_status()` 不会影响性能，是轻量级操作
4. **状态持久性**: 状态在 stop() 之前会一直保持，除非发生连接中断

## 完整示例代码

参考 `example_get_status.cpp` 文件获取完整的使用示例。

## 编译和测试

```bash
# 编译示例
g++ -o example_get_status example_get_status.cpp -I./include -L./build -lVideoDecoder -lpthread

# 运行测试
./example_get_status
```
