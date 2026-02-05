/**
 * @file ffmpeg_decoder.h
 * @brief FFmpeg 视频软解码器封装
 * 
 * 提供 H.264/H.265 视频流的软件解码功能，支持：
 * - H.264 (AVC) 解码
 * - H.265 (HEVC) 解码
 * - 直接输出BGR格式（避免YUV中间转换）
 * - 自动管理FFmpeg上下文生命周期
 * 
 * 使用示例：
 * @code
 *   VideoDecoder decoder(0);  // 0=H.264, 1=H.265
 *   int width, height;
 *   size_t data_size;
 *   uint8_t* bgr = decoder.decodeToBGR(h264_data, len, width, height, data_size);
 *   // 使用BGR数据...
 *   free(bgr);  // 调用者负责释放
 * @endcode
 */

#ifndef __FFMPEG_DECODER_H__
#define __FFMPEG_DECODER_H__
#include <cstdint>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <libavutil/opt.h>
}

/**
 * @enum DecPixelFormat_
 * @brief 解码后的像素格式枚举
 * 
 * 定义解码器输出的像素数据格式，常用于视频处理和显示。
 */
typedef enum DecPixelFormat_ {
    DEC_FMT_NONE = -1,        ///< 未定义格式
    
    /**
     * @brief YUV 4:2:0 平面格式，12bpp
     * 内存布局：[Y平面][U平面][V平面]
     * 采样：每2x2个Y像素共享1个Cr和1个Cb
     */
    DEC_FMT_YUV420P,
    
    /**
     * @brief NV12 格式，12bpp
     * 内存布局：[Y平面][UV交织平面]
     * UV交织：UVUVUV...（U在前）
     */
    DEC_FMT_NV12,
    
    /**
     * @brief NV21 格式，12bpp
     * 内存布局：[Y平面][VU交织平面]
     * VU交织：VUVUVU...（V在前）
     */
    DEC_FMT_NV21,

} DecPixelFormat;

/**
 * @class VideoDecoder
 * @brief FFmpeg 视频软解码器
 * 
 * 封装 FFmpeg 库的解码功能，提供简洁的接口来解码H.264/H.265视频流。
 * 
 * 特性：
 * - 支持 H.264 和 H.265 编码
 * - 自动管理 FFmpeg 上下文（初始化/释放）
 * - 直接输出 BGR 格式（适用于 OpenCV）
 * - 支持 YUV 格式输出（传统接口）
 * 
 * 性能优化：
 * - decodeToBGR() 直接输出BGR，避免了YUV中间拷贝和转换
 * - 使用 FFmpeg 的 SwsContext 实现高效颜色空间转换
 * 
 * 注意事项：
 * - 返回的数据指针需要调用者使用 free() 释放
 * - 线程安全：一个实例不应在多线程中同时使用
 */
class VideoDecoder {
  public:
    /**
     * @brief 构造函数
     * @param codec_id 编码类型：0=H.264, 1=H.265 (HEVC)
     * @throws 如果找不到解码器或初始化失败，可能崩溃
     */
    VideoDecoder(int32_t codec_id);
    
    /**
     * @brief 析构函数
     * @note 自动释放 FFmpeg 上下文和资源
     */
    ~VideoDecoder();

    /**
     * @brief 解码H.264/H.265数据为YUV格式（传统接口）
     * @param src 输入的H.264/H.265码流数据
     * @param len 码流数据长度（字节）
     * @param pix_w 输出参数：图像宽度（像素）
     * @param pix_h 输出参数：图像高度（像素）
     * @param format 输出参数：像素格式 (DecPixelFormat)
     * @param data_size 输出参数：YUV数据总大小（字节）
     * @return YUV数据指针，失败返回nullptr
     * @note 调用者必须使用 free() 释放返回的指针
     * @warning 每次调用都会分配新内存，注意内存泄漏
     */
    uint8_t* decode(const uint8_t* src,
                    uint32_t       len,
                    int32_t&       pix_w,
                    int32_t&       pix_h,
                    int32_t&       format,
                    size_t&        data_size);
    
    /**
     * @brief 直接解码为BGR格式（性能优化版本）
     * @param src 输入的H.264/H.265码流数据
     * @param len 码流数据长度（字节）
     * @param pix_w 输出参数：图像宽度（像素）
     * @param pix_h 输出参数：图像高度（像素）
     * @param data_size 输出参数：BGR数据总大小（字节）
     * @return BGR数据指针，失败返回nullptr
     * @note 调用者必须使用 free() 释放返回的指针
     * @note BGR格式：每像素3字节 (Blue, Green, Red)，适用于OpenCV
     * @note 相比 decode() + YUV转BGR，此函数减少了一次内存拷贝
     */
    uint8_t* decodeToBGR(const uint8_t* src,
                         uint32_t       len,
                         int32_t&       pix_w,
                         int32_t&       pix_h,
                         size_t&        data_size);

  private:
    /**
     * @brief 将 FFmpeg 的 AVPixelFormat 转换为本地的 DecPixelFormat
     * @param av_fmt FFmpeg 的像素格式枚举值
     * @return 本地的像素格式枚举值
     */
    int32_t AVPixelFormat2Format(int32_t av_fmt);

  private:
    /** @brief FFmpeg 解码器上下文，管理解码器的状态和配置 */
    AVCodecContext* dec_context = nullptr;
};
#endif