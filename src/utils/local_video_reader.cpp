#include "local_video_reader.h"
#include "decoder_utils.h"
#include <iostream>
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace {

// Helper logging functions (in anonymous namespace to avoid conflicts)
void log_error(const std::string& msg)
{
    std::cerr << "[ERROR] " << msg << std::endl;
}

void log_info(const std::string& msg)
{
    std::cout << "[INFO] " << msg << std::endl;
}

}  // anonymous namespace

namespace xtkj {

LocalVideoReader::LocalVideoReader()
    : formatContext_(nullptr), codecContext_(nullptr), frame_(nullptr),
      frameRGB_(nullptr), packet_(nullptr), swsContext_(nullptr),
      buffer_(nullptr), videoStreamIndex_(-1), width_(0), height_(0),
      opened_(false), stop_(false), loop_playback_(true), last_pts_(0),
      time_base_(0.0), fps_(0.0), bitrate_(0), total_frames_(-1),
      skip_next_frame_(false), is_interval_(true)
{
}

LocalVideoReader::~LocalVideoReader()
{
    cleanup();
}

bool LocalVideoReader::open(const std::string& videoPath)
{
    if (avformat_open_input(&formatContext_, videoPath.c_str(), nullptr,
                            nullptr) != 0) {
        log_error(std::string("Cannot open video file: ") + videoPath);
        return false;
    }

    if (avformat_find_stream_info(formatContext_, nullptr) < 0) {
        log_error("Cannot find stream information");
        cleanup();  // 释放已分配的formatContext_
        return false;
    }

    videoStreamIndex_ = -1;
    for (unsigned int i = 0; i < formatContext_->nb_streams; i++) {
        if (formatContext_->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex_ = i;
            break;
        }
    }

    if (videoStreamIndex_ == -1) {
        log_error("No video stream found");
        cleanup();  // 释放已分配的formatContext_
        return false;
    }

    AVCodecParameters* codecParameters =
        formatContext_->streams[videoStreamIndex_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
    if (codec == nullptr) {
        log_error("Decoder not found");
        cleanup();  // 释放已分配的formatContext_
        return false;
    }

    codecContext_ = avcodec_alloc_context3(codec);
    if (codecContext_ == nullptr) {
        log_error("Cannot allocate decoder context");
        cleanup();  // 释放已分配的formatContext_
        return false;
    }

    if (avcodec_parameters_to_context(codecContext_, codecParameters) < 0) {
        log_error("Cannot copy decoder parameters");
        cleanup();  // 释放formatContext_和codecContext_
        return false;
    }

    if (avcodec_open2(codecContext_, codec, nullptr) < 0) {
        log_error("Cannot open decoder");
        cleanup();  // 释放formatContext_和codecContext_
        return false;
    }

    frame_    = av_frame_alloc();
    frameRGB_ = av_frame_alloc();
    packet_   = av_packet_alloc();

    if (!frame_ || !frameRGB_ || !packet_) {
        log_error("Cannot allocate frames or packet");
        cleanup();  // 释放所有已分配资源
        return false;
    }

    width_  = codecContext_->width;
    height_ = codecContext_->height;

    // Calculate time base for PTS to milliseconds conversion
    AVRational time_base =
        formatContext_->streams[videoStreamIndex_]->time_base;
    time_base_ = av_q2d(time_base) * 1000.0;  // Convert to milliseconds
    last_pts_  = 0;

    // Get FPS
    AVRational frame_rate =
        formatContext_->streams[videoStreamIndex_]->avg_frame_rate;

    if (frame_rate.den != 0) {
        fps_ = av_q2d(frame_rate);
    }
    else {
        fps_ = 0.0;
    }

    // Get bitrate
    bitrate_ = formatContext_->streams[videoStreamIndex_]->codecpar->bit_rate;
    if (bitrate_ == 0 && formatContext_->bit_rate > 0) {
        bitrate_ = formatContext_->bit_rate;
    }

    // Get total frames
    int64_t duration = formatContext_->streams[videoStreamIndex_]->duration;
    if (duration != AV_NOPTS_VALUE && fps_ > 0) {
        total_frames_ =
            static_cast<int64_t>(duration * fps_ * time_base_ / 1000.0);
    }
    else if (formatContext_->streams[videoStreamIndex_]->nb_frames > 0) {
        total_frames_ = formatContext_->streams[videoStreamIndex_]->nb_frames;
    }
    else {
        total_frames_ = -1;
    }

    int numBytes =
        av_image_get_buffer_size(AV_PIX_FMT_RGB24, width_, height_, 1);
    buffer_ = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    if (buffer_ == nullptr) {
        log_error("Cannot allocate buffer");
        cleanup();  // 释放所有已分配资源
        return false;
    }

    av_image_fill_arrays(frameRGB_->data, frameRGB_->linesize, buffer_,
                         AV_PIX_FMT_RGB24, width_, height_, 1);

    swsContext_ = sws_getContext(width_, height_, codecContext_->pix_fmt,
                                 width_, height_, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (swsContext_ == nullptr) {
        log_error("Cannot create conversion context");
        cleanup();  // 释放所有已分配资源
        return false;
    }

    log_info(std::string("Local video opened: ") + std::to_string(width_) +
             "x" + std::to_string(height_));
    opened_ = true;
    return true;
}

bool LocalVideoReader::readFrame(cv::Mat& outMat)
{
    if (!opened_ || stop_.load()) {
        return false;
    }

    while (!stop_.load()) {
        while (av_read_frame(formatContext_, packet_) >= 0) {
            if (stop_.load()) {
                av_packet_unref(packet_);
                return false;
            }

            if (packet_->stream_index == videoStreamIndex_) {
                // 跳帧处理：当 is_interval_=true
                // 时，交替跳过帧（解码一帧，跳过一帧）
                // 使用布尔标志避免计数器在长时间运行中溢出
                if (is_interval_) {
                    if (skip_next_frame_) {
                        skip_next_frame_ = false;  // 翻转标志，下一帧解码
                        av_packet_unref(packet_);
                        // printf("跳过一帧\n");
                        continue;  // 跳过当前帧，不解码
                    }
                    skip_next_frame_ = true;  // 翻转标志，下一帧跳过
                }

                int ret = avcodec_send_packet(codecContext_, packet_);
                if (ret < 0) {
                    av_packet_unref(packet_);
                    continue;
                }

                ret = avcodec_receive_frame(codecContext_, frame_);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_unref(packet_);
                    continue;
                }
                else if (ret < 0) {
                    av_packet_unref(packet_);
                    return false;
                }

                sws_scale(swsContext_, frame_->data, frame_->linesize, 0,
                          height_, frameRGB_->data, frameRGB_->linesize);

                cv::Mat mat(height_, width_, CV_8UC3, frameRGB_->data[0],
                            frameRGB_->linesize[0]);
                cv::cvtColor(mat, outMat, cv::COLOR_RGB2BGR);

                // Calculate PTS in milliseconds
                if (frame_->pts != AV_NOPTS_VALUE) {
                    last_pts_ = static_cast<int64_t>(frame_->pts * time_base_);
                }
                else if (packet_->pts != AV_NOPTS_VALUE) {
                    last_pts_ = static_cast<int64_t>(packet_->pts * time_base_);
                }
                else {
                    // If no PTS available, estimate based on frame count
                    // and fps
                    last_pts_ += static_cast<int64_t>(
                        1000.0 /
                        av_q2d(formatContext_->streams[videoStreamIndex_]
                                   ->r_frame_rate));
                }

                av_packet_unref(packet_);
                return true;
            }
            av_packet_unref(packet_);
        }

        // End of file
        if (!stop_.load() && loop_playback_) {
            // Loop playback: seek to beginning
            av_seek_frame(formatContext_, videoStreamIndex_, 0,
                          AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codecContext_);
            last_pts_        = 0;      // Reset PTS for new loop
            skip_next_frame_ = false;  // Reset frame skip flag for new loop
            // Continue the outer loop to read from the beginning
        }
        else {
            // Stop playback or no loop
            break;
        }
    }

    return false;
}

void LocalVideoReader::stopReading()
{
    stop_.store(true);
}

void LocalVideoReader::setLoopPlayback(bool loop)
{
    loop_playback_ = loop;
}

void LocalVideoReader::setFrameSkip(bool enable)
{
    is_interval_ = enable;
}

int LocalVideoReader::getWidth() const
{
    return width_;
}

int LocalVideoReader::getHeight() const
{
    return height_;
}

int64_t LocalVideoReader::getLastPTS() const
{
    return last_pts_;
}

double LocalVideoReader::getFPS() const
{
    return fps_;
}

int64_t LocalVideoReader::getBitrate() const
{
    return bitrate_;
}

int64_t LocalVideoReader::getTotalFrames() const
{
    return total_frames_;
}

void LocalVideoReader::cleanup()
{
    if (buffer_) {
        av_free(buffer_);
        buffer_ = nullptr;
    }
    if (frameRGB_) {
        av_frame_free(&frameRGB_);
        frameRGB_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    if (codecContext_) {
        avcodec_free_context(&codecContext_);
        codecContext_ = nullptr;
    }
    if (formatContext_) {
        avformat_close_input(&formatContext_);
        formatContext_ = nullptr;
    }
    if (swsContext_) {
        sws_freeContext(swsContext_);
        swsContext_ = nullptr;
    }
    opened_ = false;
}

}  // namespace xtkj
