/**
 * @file local_video_reader.h
 * @brief 本地视频文件读取器
 * 
 * 提供本地视频文件的同步读取功能，支持：
 * - 多种视频格式（MP4, AVI, MKV, FLV 等）
 * - 循环播放
 * - 跳帧读取（隔帧）
 * - 直接输出BGR数据（无缓存）
 * - 获取视频元信息（帧率、码率、总帧数等）
 * 
 * 使用场景：
 * - 本地视频文件的实时解码播放
 * - 视频测试案例（代替RTSP流）
 * - 离线视频分析
 * 
 * 特点：
 * - 同步阅读模式：每次调用阻塞直到读取到帧
 * - 无缓存、无丢帧：与调用者速度同步
 * - 支持暂停/恢复操作
 */

#ifndef LOCAL_VIDEO_READER_H
#define LOCAL_VIDEO_READER_H

#include <atomic>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace cv {
class Mat;
}

namespace xtkj {

/**
 * @class LocalVideoReader
 * @brief 本地视频文件读取器
 * 
 * 使用 FFmpeg 库读取本地视频文件，提供同步的帧级别访问接口。
 * 
 * 主RTSP流的区别：
 * - 同步读取：get_frame() 调用时才解码
 * - 无缓存、无丢帧：与调用者速度严格同步
 * - 支持循环播放：文件结束后自动从头开始
 * - 支持跳帧：可隔帧读取减轻解码负载
 * 
 * 典型使用流程：
 * @code
 *   LocalVideoReader reader;
 *   if (reader.open("/path/to/video.mp4")) {
 *       reader.setLoopPlayback(true);   // 启用循环播放
 *       reader.setFrameSkip(true);      // 启用跳帧（隔帧读取）
 *       
 *       int width, height;
 *       size_t data_size;
 *       while (!stop) {
 *           uint8_t* bgr = reader.readFrameDirect(width, height, data_size);
 *           // 处理BGR数据...
 *           free(bgr);
 *       }
 *       reader.stopReading();
 *   }
 * @endcode
 */
class LocalVideoReader {
  private:
    // ========== FFmpeg 上下文对象 ==========
    AVFormatContext*   formatContext_;    ///< FFmpeg 格式上下文（封装/解封装）
    AVCodecContext*    codecContext_;     ///< FFmpeg 编解码器上下文
    AVFrame*           frame_;            ///< 原始YUV帧
    AVFrame*           frameRGB_;         ///< RGB帧（转换后）
    AVPacket*          packet_;           ///< 网络包/数据包
    struct SwsContext* swsContext_;       ///< 颜色空间转换上下文（YUV→RGB/BGR）
    uint8_t*           buffer_;           ///< RGB数据缓冲区
    
    // ========== 视频信息 ==========
    int                videoStreamIndex_; ///< 视频流索引（文件可能包含多个流）
    int                width_;            ///< 视频宽度（像素）
    int                height_;           ///< 视频高度（像素）
    double             fps_;              ///< 帧率（Frames Per Second）
    int64_t            bitrate_;          ///< 码率（bits per second）
    int64_t            total_frames_;     ///< 总帧数
    
    // ========== 状态标志 ==========
    bool               opened_;           ///< 是否已打开文件
    std::atomic<bool>  stop_;             ///< 停止标志
    bool               loop_playback_;    ///< 循环播放模式
    bool               is_interval_;      ///< 是否启用跳帧（隔帧读取）
    bool               skip_next_frame_;  ///< 跳帧标志：true=下一帧跳过
    
    // ========== 时间戳管理 ==========
    int64_t            last_pts_;         ///< 最后一帧的PTS（Presentation Time Stamp）
    double             time_base_;        ///< 时间基准（秒/刻度）
    
    /**
     * @brief 清理FFmpeg资源
     * @note 释放所有分配的FFmpeg对象和缓冲区
     */
    void cleanup();

  public:
    /** @brief 构造函数 */
    LocalVideoReader();
    
    /**
     * @brief 析构函数
     * @note 自动调用 cleanup() 释放资源
     */
    ~LocalVideoReader();

    /**
     * @brief 打开本地视频文件
     * @param videoPath 视频文件绝对路径
     * @return true=打开成功，false=打开失败
     * @note 会自动检测视频流并初始化解码器
     */
    bool open(const std::string& videoPath);
    
    /**
     * @brief 读取一帧到OpenCV Mat（传统接口）
     * @param outMat 输出OpenCV Mat对象（BGR格式）
     * @return true=读取成功，false=读取失败或文件结束
     * @note 会自动处理循环播放和跳帧
     */
    bool readFrame(cv::Mat& outMat);
    
    /**
     * @brief 直接读取BGR数据指针（零拷贝，性能优化版本）
     * @param width 输出参数：帧宽度
     * @param height 输出参数：帧高度
     * @param data_size 输出参数：BGR数据大小
     * @return BGR数据指针，失败返回nullptr
     * @note 调用者必须使用 free() 释放返回的指针
     * @note 相比 readFrame()，此函数避免了Mat封装的开销
     */
    uint8_t* readFrameDirect(int& width, int& height, size_t& data_size);
    
    /**
     * @brief 停止读取
     * @note 设置停止标志，阻断后续读取操作
     */
    void stopReading();
    
    /**
     * @brief 设置循环播放模式
     * @param loop true=循环播放，false=播放一次后停止
     * @note 必须在 open() 之后调用
     */
    void setLoopPlayback(bool loop);
    
    /**
     * @brief 设置跳帧模式（隔帧读取）
     * @param enable true=启用跳帧（解码1帧跳过1帧），false=读取所有帧
     * @note 启用后可减少约50%的解码负载
     */
    void setFrameSkip(bool enable);

    // ========== 视频信息获取接口 ==========
    
    /** @brief 获取视频宽度（像素）*/
    int     getWidth() const;
    
    /** @brief 获取视频高度（像素）*/
    int     getHeight() const;
    
    /** @brief 获取最后一帧的PTS（Presentation Time Stamp）*/
    int64_t getLastPTS() const;
    
    /** @brief 获取视频帧率（FPS）*/
    double  getFPS() const;
    
    /** @brief 获取视频码率（bps）*/
    int64_t getBitrate() const;
    
    /** @brief 获取视频总帧数 */
    int64_t getTotalFrames() const;
};

}  // namespace xtkj

#endif  // LOCAL_VIDEO_READER_H
