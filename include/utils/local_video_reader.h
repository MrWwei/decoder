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

class LocalVideoReader {
  private:
    AVFormatContext*   formatContext_;
    AVCodecContext*    codecContext_;
    AVFrame*           frame_;
    AVFrame*           frameRGB_;
    AVPacket*          packet_;
    struct SwsContext* swsContext_;
    uint8_t*           buffer_;
    int                videoStreamIndex_;
    int                width_;
    int                height_;
    bool               opened_;
    std::atomic<bool>  stop_;
    bool               loop_playback_;
    int64_t            last_pts_;
    double             time_base_;
    double             fps_;
    int64_t            bitrate_;
    int64_t            total_frames_;
    bool               skip_next_frame_;
    bool               is_interval_;

    void cleanup();

  public:
    LocalVideoReader();
    ~LocalVideoReader();

    bool open(const std::string& videoPath);
    bool readFrame(cv::Mat& outMat);
    
    // 直接返回BGR数据指针，避免多次拷贝（调用者负责free）
    uint8_t* readFrameDirect(int& width, int& height, size_t& data_size);
    
    void stopReading();
    void setLoopPlayback(bool loop);
    void setFrameSkip(bool enable);

    int     getWidth() const;
    int     getHeight() const;
    int64_t getLastPTS() const;
    double  getFPS() const;
    int64_t getBitrate() const;
    int64_t getTotalFrames() const;
};

}  // namespace xtkj

#endif  // LOCAL_VIDEO_READER_H
