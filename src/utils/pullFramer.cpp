/**
 * @file pullFramer.cpp
 * @brief 视频帧拉取与处理模块实现
 *
 * 实现H.264/H.265视频帧的接收、分析和处理逻辑。
 * 主要功能包括：
 * 1. 封装ZLMediaKit的mk_frame为FrameData对象
 * 2. 累积和合并配置帧（SPS/PPS/VPS）
 * 3. 过滤首个IDR帧之前的B帧
 * 4. 将处理后的帧传递给解码器
 */

#include "pullFramer.h"
#include "opencv2/opencv.hpp"
#include "stdio.h"
#include "string.h"
#include <iomanip>
#include <iostream>
using namespace std;

/**
 * @brief 将二进制数据转换为十六进制字符串（用于调试）
 * @param buf 数据缓冲区指针
 * @param size 数据大小（字节）
 * @return 十六进制字符串，格式为 "01 23 45 67..."
 * @note 主要用于打印帧数据的起始字节（如H.264起始码 00 00 00 01）
 */

static std::string dump(const void* buf, size_t size)
{
    int         len    = 0;
    char*       data   = (char*)buf;
    std::string result = "NULL";
    if (data == NULL || size <= 0)
        return result;
    size_t total = size * 3 + 1;
    char*  buff  = new char[size * 3 + 1];
    memset(buff, 0, size * 3 + 1);
    for (size_t i = 0; i < size; i++) {
        len += snprintf(buff + len, total - len, "%.2x ", (data[i] & 0xff));
    }
    result = std::string(buff);
    delete[] buff;
    buff = NULL;
    return result;
}

/**
 * @brief 从 mk_frame 构造 FrameData（深拷贝模式）
 *
 * 将ZLMediaKit的mk_frame转换为FrameData对象，分配新内存并拷贝数据。
 *
 * 处理步骤：
 * 1. 验证mk_frame有效性
 * 2. 提取帧数据和大小
 * 3. 分配内存并拷贝数据（+1字节用于\0结束符）
 * 4. 提取时间戳和帧属性标志
 *
 * @note 此构造函数会分配新内存，需要在析构函数中释放
 */
FrameData::FrameData(const mk_frame frame)
    : data_(nullptr), size_(0), dts_(0), pts_(0), prefixSize_(0),
      keyFrame_(false), configFrame_(false), dropAble_(true), decodeAble_(false)
{
    // 验证mk_frame有效性
    if (frame == NULL) {
        std::cerr << "frame is NULL" << std::endl;
        return;
    }

    // 从 mk_frame 提取数据指针和大小
    auto data = mk_frame_get_data(frame);
    auto size = mk_frame_get_data_size(frame);
    if (data == NULL || size == 0) {
        std::cerr << "data is NULL or size is 0" << std::endl;
        return;
    }

    // 分配内存并拷贝数据（+1用于\0结束符）
    data_ = new uint8_t[size + 1];
    if (data_ == nullptr) {
        std::cout << "new failed" << std::endl;
        return;
    }
    size_ = size;
    memcpy(data_, data, size_);
    data_[size_] = 0;  // 添加\0结束符

    // 提取时间戳信息
    dts_ = mk_frame_get_dts(frame);  // 解码时间戳
    pts_ = mk_frame_get_pts(frame);  // 显示时间戳
    prefixSize_ =
        mk_frame_get_data_prefix_size(frame);  // H.264/H.265起始码长度

    // 解析帧属性标志位
    auto flag    = mk_frame_get_flags(frame);
    keyFrame_    = (flag & MK_FRAME_FLAG_IS_KEY);     // 是否为I帧
    configFrame_ = (flag & MK_FRAME_FLAG_IS_CONFIG);  // 是否为SPS/PPS/VPS
    dropAble_ = (flag & MK_FRAME_FLAG_DROP_ABLE);  // 是否为B帧（可丢弃）
    decodeAble_ = !(flag & MK_FRAME_FLAG_NOT_DECODE_ABLE);  // 是否可解码
}

/**
 * @brief 从自定义数据构造 FrameData（用于合并后的帧）
 *
 * 主要用于创建合并后的帧数据，如 SPS+PPS+IDR。
 * 时间戳和属性标志从原始mk_frame中继承。
 *
 * 使用场景：
 * @code
 *   // 合并SPS+PPS+IDR
 *   unsigned char* merged = malloc(sps_size + pps_size + idr_size);
 *   memcpy(merged, sps_data, sps_size);
 *   memcpy(merged + sps_size, pps_data, pps_size);
 *   memcpy(merged + sps_size + pps_size, idr_data, idr_size);
 *
 *   // 创建FrameData（会拷贝数据）
 *   auto frame = FrameData::CreateShared(merged, total_size,
 * original_mk_frame); free(merged);  // 可以立即释放
 * @endcode
 *
 * @param data 帧数据指针（合并后的数据）
 * @param size 数据大小
 * @param frame 原始mk_frame，用于获取时间戳和标志位
 */
FrameData::FrameData(uint8_t* data, size_t size, const mk_frame frame)
    : data_(nullptr), size_(0), dts_(0), pts_(0), prefixSize_(0),
      keyFrame_(false), configFrame_(false), dropAble_(true), decodeAble_(false)
{
    if (frame == NULL) {
        std::cerr << "frame is NULL" << std::endl;
        return;
    }

    if (data == NULL || size == 0) {
        std::cerr << "data is NULL or size is 0" << std::endl;
        return;
    }

    // 分配内存并拷贝合并后的数据
    data_ = new uint8_t[size + 1];
    if (data_ == nullptr) {
        std::cout << "new failed" << std::endl;
        return;
    }
    size_ = size;
    memcpy(data_, data, size_);
    data_[size_] = 0;  // 添加结束符

    // 从原始 mk_frame 中继承时间戳和属性
    dts_         = mk_frame_get_dts(frame);
    pts_         = mk_frame_get_pts(frame);
    prefixSize_  = mk_frame_get_data_prefix_size(frame);
    auto flag    = mk_frame_get_flags(frame);
    keyFrame_    = (flag & MK_FRAME_FLAG_IS_KEY);
    configFrame_ = (flag & MK_FRAME_FLAG_IS_CONFIG);
    dropAble_    = (flag & MK_FRAME_FLAG_DROP_ABLE);
    decodeAble_  = !(flag & MK_FRAME_FLAG_NOT_DECODE_ABLE);
}

FrameData::~FrameData()
{
    if (data_ != nullptr) {
        delete[] data_;
        data_ = nullptr;
    }
    size_ = 0;
}

std::ostream& operator<<(std::ostream& os, const FrameData::Ptr& Frame)
{
    size_t len = 10;
    if (Frame.get() == nullptr || Frame->data() == nullptr ||
        Frame->size() == 0) {
        os << "NULL";
        return os;
    }
    if (Frame->size() < 10) {
        len = Frame->size();
    }
    os << "[" << Frame->pts() << ", drop:" << Frame->dropAble()
       << ", key:" << Frame->keyFrame() << ", " << std::setw(6) << std::right
       << Frame->size() << "] : " << dump(Frame->data(), len);

    return os;
}

PullFramer::PullFramer()
{
    configFrames     = NULL;
    configFramesSize = 0;
    cb_              = NULL;
    decoder_         = NULL;
    displayer_       = NULL;
}

PullFramer::~PullFramer()
{
    clearConfigFrames();
}

void PullFramer::setOnGetFrame(const onGetFrame& onGetFrame, void* decoder)
{
    cb_      = onGetFrame;  // 设置函数对象
    decoder_ = decoder;     // 设置解码器
}

/**
 * @brief 清空配置帧缓冲区
 * @note 释放 configFrames 内存并重置大小为0
 */
void PullFramer::clearConfigFrames()
{
    if (configFrames != NULL) {
        free(configFrames);
        configFrames = nullptr;
    }
    configFramesSize = 0;
}

/**
 * @brief 处理单个视频帧（核心函数）
 *
 * 这是整个视频帧处理的核心逻辑，负责：
 * 1. 识别帧类型（配置帧/I帧/P帧/B帧）
 * 2. 累积配置帧（SPS/PPS/VPS）
 * 3. 过滤首个IDR帧前的B帧
 * 4. 合并配置帧和IDR帧
 * 5. 通过回调传递给解码器
 *
 * 处理流程：
 * @code
 *   帧序列: SPS → PPS → B → B → IDR → P → B → P ...
 *
 *   处理过程:
 *   1. SPS      → 累积到 configFrames
 *   2. PPS      → 追加到 configFrames
 *   3. B        → dropAble=true，追加到 configFrames（但不发送）
 *   4. B        → dropAble=true，追加到 configFrames
 *   5. IDR      → 合并 [SPS+PPS+IDR] → 发送给解码器
 *                 清空 configFrames
 *   6. P        → 直接发送
 *   7. B        → 直接发送
 *   8. P        → 直接发送
 * @endcode
 *
 * @param frame_ ZLMediaKit 的 mk_frame 对象
 * @return true=处理成功，false=帧无效或处理失败
 */
bool PullFramer::onFrame(const mk_frame frame_)
{
    // === 步骤1：验证帧有效性 ===
    if (frame_ == NULL) {
        return false;
    }

    // 封装成 FrameData 对象
    auto frame = FrameData::CreateShared(frame_);
    if (frame.get() == nullptr || frame->data() == nullptr ||
        frame->size() == 0) {
        return false;
    }

    auto data = frame->data();
    auto size = frame->size();

    // === 步骤2：判断帧类型并处理 ===

    // 【情况1】配置帧（SPS/PPS/VPS）：累积到缓冲区
    if (frame->configFrame()) {
        // 扩展缓冲区并追加配置帧数据
        size_t         newSize = configFramesSize + size;
        unsigned char* newConfigFrames =
            (unsigned char*)realloc(configFrames, newSize);
        if (newConfigFrames == NULL) {
            std::cout << "realloc failed" << std::endl;
            clearConfigFrames();
            return false;
        }
        configFrames = newConfigFrames;
        memcpy(configFrames + configFramesSize, data, size);  // 追加数据
        configFramesSize = newSize;
    }
    // 【情况2/3】数据帧（I/P/B帧）
    else {
        // === 检查是否有累积的配置帧 ===
        if (configFrames != NULL && configFramesSize != 0) {
            // 【情况2】可丢弃帧（B帧）且首个IDR未到：累积但不发送
            // 原因：B帧依赖前后帧，在首个IDR到达前无法解码，丢弃避免解码失败
            if (frame->dropAble()) {
                size_t         newSize = configFramesSize + size;
                unsigned char* newConfigFrames =
                    (unsigned char*)realloc(configFrames, newSize);
                if (newConfigFrames == NULL) {
                    std::cout << "realloc failed" << std::endl;
                    clearConfigFrames();
                    return false;
                }
                configFrames = newConfigFrames;
                memcpy(configFrames + configFramesSize, data, size);
                configFramesSize = newSize;
                return true;  // 注意：累积但不发送给解码器
            }

            // 【情况3】关键帧（IDR）或首帧：合并配置帧后发送
            // 合并格式：[SPS] + [PPS] + [当前帧]
            size_t         totalSize  = configFramesSize + size;
            unsigned char* mergedData = (unsigned char*)malloc(totalSize);
            if (mergedData == NULL) {
                std::cout << "malloc failed" << std::endl;
                clearConfigFrames();
                return false;
            }

            // 拷贝：配置帧 + 当前帧
            memcpy(mergedData, configFrames, configFramesSize);
            memcpy(mergedData + configFramesSize, data, size);

            // 清空配置帧缓冲区（已合并完成）
            clearConfigFrames();

            // 发送合并后的帧给解码器
            if (cb_) {
                cb_(FrameData::CreateShared(mergedData, totalSize, frame_),
                    decoder_);
            }
            else {
                // 调试输出：打印帧信息
                printf("on Frame %zu [%.2x %.2x %.2x %.2x %.2x %.2x]\n",
                       totalSize, mergedData[0], mergedData[1], mergedData[2],
                       mergedData[3], (mergedData[4] & 0xff),
                       (mergedData[5] & 0xff));
            }

            free(mergedData);
            mergedData = NULL;
            return true;
        }
        // 【情况4】后续帧（P/B帧，IDR之后）：直接发送
        else {
            if (cb_) {
                cb_(FrameData::CreateShared(frame_), decoder_);
            }
            else {
                // 调试输出
                printf("on Frame %zu [%.2x %.2x %.2x %.2x %.2x %.2x]\n", size,
                       data[0], data[1], data[2], data[3], (data[4] & 0xff),
                       (data[5] & 0xff));
            }
            return true;
        }
    }
    return true;
}
