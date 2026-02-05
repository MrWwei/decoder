/**
 * @file pullFramer.h
 * @brief 视频帧拉取与处理模块
 * 
 * 本文件实现了视频流的帧数据封装与处理功能，主要包含两个核心类：
 * 1. FrameData: 封装 ZLMediaKit 的原始帧数据，提供统一的访问接口
 * 2. PullFramer: 处理视频帧序列，合并配置帧(SPS/PPS/VPS)并传递给解码器
 * 
 * 主要功能：
 * - 视频帧数据的封装与管理（智能指针）
 * - H.264/H.265 配置帧的累积与合并
 * - I/P/B帧类型识别与处理
 * - 首帧前的B帧过滤（避免解码失败）
 * - 回调机制传递帧数据给解码器
 * 
 * 典型使用流程：
 * @code
 *   auto puller = PullFramer::CreateShared();
 *   puller->setOnGetFrame([](const FrameData::Ptr& frame, void* decoder) {
 *       // 处理帧数据：解码、显示等
 *   }, decoder_ptr);
 *   // ZLMediaKit 回调触发：puller->onFrame(mk_frame);
 * @endcode
 */

#ifndef PULL_FRAMER_H  // 防止头文件被重复包含
#define PULL_FRAMER_H

#include <iostream>
#include <memory>
#include <functional>
#include "mk_frame.h"
#include "opencv2/opencv.hpp"

/**
 * @class FrameData
 * @brief 视频帧数据封装类
 * 
 * 封装 ZLMediaKit 的 mk_frame 原始帧数据，提供 C++ 风格的安全访问接口。
 * 使用智能指针自动管理内存，防止内存泄漏和野指针问题。
 * 
 * 设计模式：
 * - 工厂模式：通过 CreateShared() 创建实例
 * - RAII：使用 shared_ptr 自动管理生命周期
 * - 禁止拷贝：防止内存双重释放
 * 
 * 支持两种构造方式：
 * 1. 从 mk_frame 构造（共享内存，适用于原始帧）
 * 2. 从自定义数据构造（深拷贝，适用于合并后的帧如 SPS+PPS+IDR）
 */
class FrameData {
public:
    using Ptr = std::shared_ptr<FrameData>;  ///< 智能指针别名，自动内存管理
    
    /**
     * @brief 从 ZLMediaKit 原始帧创建 FrameData（共享内存模式）
     * @param frame ZLMediaKit 的 mk_frame 对象
     * @return 智能指针，自动管理生命周期
     * @note 内存由 mk_frame 管理，FrameData 只保存指针引用
     */
    static Ptr CreateShared(const mk_frame frame) {
        return Ptr(new FrameData(frame));
    }
    
    /**
     * @brief 从自定义数据创建 FrameData（深拷贝模式）
     * @param data_ 帧数据指针（如合并后的 SPS+PPS+IDR）
     * @param size_ 数据大小（字节）
     * @param frame 原始 mk_frame，用于提取时间戳和标志位
     * @return 智能指针
     * @note 会分配新内存并拷贝数据，适用于需要独立管理内存的场景
     */
    static Ptr CreateShared(uint8_t* data_, size_t size_, const mk_frame frame) {
        return Ptr(new FrameData(data_, size_, frame));
    }
    
    /**
     * @brief 析构函数，释放资源
     * @warning 当前实现存在内存泄漏：未释放深拷贝模式下分配的内存
     */
    ~FrameData();

    // ========== 访问器函数（Getters）==========
    
    /** @brief 获取解码时间戳（Decoding Time Stamp）*/
    uint64_t dts() { return dts_; };
    
    /** @brief 获取显示时间戳（Presentation Time Stamp）*/
    uint64_t pts() const { return pts_; }
    
    /** @brief 获取H.264/H.265起始码长度（通常为3或4字节：0x000001 或 0x00000001）*/
    size_t prefixSize() const { return prefixSize_; }
    
    /** @brief 判断是否为关键帧（I帧/IDR帧）- 可独立解码的完整图像 */
    bool keyFrame() const { return keyFrame_; }
    
    /** @brief 判断是否为可丢弃帧（B帧）- 双向预测帧，丢失不影响其他帧解码 */
    bool dropAble() const { return dropAble_; }
    
    /** @brief 判断是否为配置帧（SPS/PPS/VPS）- 解码器初始化参数，非图像数据 */
    bool configFrame() const { return configFrame_; }
    
    /** @brief 判断是否可解码 */
    bool decodeAble() const { return decodeAble_; }
    
    /** @brief 获取帧数据指针（原始H.264/H.265码流）*/
    uint8_t* data() const { return data_; }
    
    /** @brief 获取帧数据大小（字节）*/
    size_t size() const { return size_; }
    
    /** @brief 重载输出运算符，用于调试打印帧信息 */
    friend std::ostream& operator<<(std::ostream& os, const FrameData::Ptr& Frame);
    
private:
    // ========== 私有构造函数（防止直接实例化）==========
    
    /** @brief 从 mk_frame 构造（共享内存）*/
    FrameData(const mk_frame frame);
    
    /** @brief 从自定义数据构造（深拷贝）*/
    FrameData(uint8_t *data_, size_t size_, const mk_frame frame);
    
    /** @brief 禁止拷贝构造（防止内存双重释放）*/
    FrameData(const FrameData& other) = delete;
    
private:
    // ========== 成员变量 ==========
    
    // === 时间戳信息 ===
    uint64_t dts_;          ///< 解码时间戳（Decoding Time Stamp）
    uint64_t pts_;          ///< 显示时间戳（Presentation Time Stamp）
    
    // === 帧属性标志（从 mk_frame_get_flags 解析）===
    size_t prefixSize_;     ///< H.264/H.265 起始码长度（3或4字节）
    bool keyFrame_;         ///< 是否为关键帧（I帧）- 完整图像，可独立解码
    bool dropAble_;         ///< 是否可丢弃（B帧）- 双向预测帧，依赖前后帧
    bool configFrame_;      ///< 是否为配置帧（SPS/PPS/VPS）- 解码器参数
    bool decodeAble_;       ///< 是否可解码
    
    // === 数据指针 ===
    uint8_t* data_;         ///< 帧数据指针（H.264/H.265 码流）
    size_t size_;           ///< 数据大小（字节）
};

/**
 * @class PullFramer
 * @brief 视频帧拉取器与处理器
 * 
 * 核心功能：
 * 1. 处理 ZLMediaKit 拉取的原始视频帧
 * 2. 累积并合并配置帧（SPS/PPS/VPS）
 * 3. 过滤首个IDR帧前的B帧（避免解码失败）
 * 4. 将处理后的帧通过回调传递给解码器
 * 
 * 处理流程：
 * @code
 *   接收帧 → 判断类型 → 处理逻辑
 *   
 *   配置帧(SPS/PPS/VPS)  → 累积到缓冲区
 *   B帧(首个IDR前)       → 丢弃（累积但不发送）
 *   I帧(IDR)            → 合并 [SPS+PPS+IDR] 发送
 *   P/B帧(IDR后)        → 直接发送
 * @endcode
 * 
 * 典型使用示例：
 * @code
 *   auto puller = PullFramer::CreateShared();
 *   puller->setOnGetFrame([](const FrameData::Ptr& frame, void* decoder) {
 *       // 这里接收到的帧已经包含必要的配置信息
 *       VideoDecoder* dec = static_cast<VideoDecoder*>(decoder);
 *       dec->decode(frame->data(), frame->size(), ...);
 *   }, decoder_ptr);
 * @endcode
 */
class PullFramer {
public:
    using Ptr = std::shared_ptr<PullFramer>;  ///< 智能指针别名
    
    /** @brief 帧处理回调函数类型 */
    using onGetFrame = std::function<void(const FrameData::Ptr&, void* decoder)>;
    
    /**
     * @brief 创建 PullFramer 实例
     * @return 智能指针
     */
    static PullFramer::Ptr CreateShared() {
        return std::make_shared<PullFramer>();
    }
    
    /**
     * @brief 设置帧处理回调函数
     * @param onGetFrame 回调函数，接收处理后的帧数据
     * @param decoder 解码器指针，传递给回调函数
     */
    void setOnGetFrame(const onGetFrame& onGetFrame, void* decoder);
    
    /** @brief 构造函数 */
    PullFramer();
    
    /** @brief 析构函数，释放配置帧缓冲区 */
    ~PullFramer();

    /**
     * @brief 处理单个视频帧（ZLMediaKit回调入口）
     * @param frame ZLMediaKit 的 mk_frame 对象
     * @return true=成功处理，false=帧无效或处理失败
     * 
     * 处理逻辑：
     * - 配置帧：累积到 configFrames 缓冲区
     * - B帧（首个IDR前）：累积但不发送（避免解码失败）
     * - IDR帧：合并配置帧后发送给解码器
     * - 后续P/B帧：直接发送
     */
    bool onFrame(const mk_frame frame);

private:
    // ========== 成员变量 ==========
    
    onGetFrame cb_;                      ///< 帧处理回调函数
    void* decoder_;                      ///< 解码器指针，传递给回调函数
    void* displayer_;                    ///< 显示器指针（预留，暂未使用）
    
    // === 配置帧缓冲区（用于累积 SPS/PPS/VPS）===
    unsigned char* configFrames;         ///< 配置帧缓冲区指针（使用realloc动态扩展）
    size_t configFramesSize;             ///< 配置帧缓冲区大小（字节）
    
    /**
     * @brief 清空配置帧缓冲区
     * @note 释放 configFrames 内存并重置大小为0
     */
    void clearConfigFrames();
};
#endif