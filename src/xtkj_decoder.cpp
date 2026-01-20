#include "xtkj_decoder.h"
#include "data_type.h"
#include "ffmpeg_decoder.h"
#include "opencv2/opencv.hpp"
#include "pullFramer.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <queue>
#include <stack>
#include <thread>
#include <unordered_set>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Include mk_mediakit after standard headers to avoid macro conflicts
#include "mk_mediakit.h"

// Undefine conflicting macros if they exist
#ifdef log_printf
#    undef log_printf
#endif
#ifdef log_error
#    undef log_error
#endif
#ifdef log_info
#    undef log_info
#endif

#ifdef __cplusplus
extern "C" {
#endif

using namespace xtkj;

// Configuration structure
struct DecoderConfig
{
    static constexpr int MAX_STACK_SIZE     = 1;
    static constexpr int DEFAULT_TIMEOUT_MS = 10000;
    static constexpr int MAX_RETRY_TIMES    = 20;
};

// Forward declaration
class Decoder;

static unsigned char* load_data(FILE* fp, size_t ofst, size_t sz);
static unsigned char* read_file_data(const char* filename, int* model_size);
void*       YV12ToBGR24_OpenCV(unsigned char* pYUV, int width, int height);
static void clear_frame_stack(Decoder* decoder);
static void log_error(const std::string& msg);
static void log_info(const std::string& msg);
static bool is_rtsp_url(const std::string& path);

// Local video reader class for reading video files directly
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
    bool    loop_playback_;  // Whether to loop playback when reaching end
    int64_t last_pts_;       // Store last frame PTS in milliseconds
    double  time_base_;      // Time base for PTS conversion
    double  fps_;            // Frame rate
    int64_t bitrate_;        // Bit rate
    int64_t total_frames_;   // Total number of frames
    bool skip_next_frame_;   // Boolean flag for alternating frame skip (avoids
                             // overflow)
    bool is_interval_;       // Whether to enable frame skipping

  public:
    LocalVideoReader()
        : formatContext_(nullptr), codecContext_(nullptr), frame_(nullptr),
          frameRGB_(nullptr), packet_(nullptr), swsContext_(nullptr),
          buffer_(nullptr), videoStreamIndex_(-1), width_(0), height_(0),
          opened_(false), stop_(false), loop_playback_(true), last_pts_(0),
          time_base_(0.0), fps_(0.0), bitrate_(0), total_frames_(-1),
          skip_next_frame_(false), is_interval_(true)
    {
    }

    ~LocalVideoReader()
    {
        cleanup();
    }

    bool open(const std::string& videoPath)
    {
        if (avformat_open_input(&formatContext_, videoPath.c_str(), nullptr,
                                nullptr) != 0) {
            log_error(std::string("Cannot open video file: ") + videoPath);
            return false;
        }

        if (avformat_find_stream_info(formatContext_, nullptr) < 0) {
            log_error("Cannot find stream information");
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
            return false;
        }

        AVCodecParameters* codecParameters =
            formatContext_->streams[videoStreamIndex_]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
        if (codec == nullptr) {
            log_error("Decoder not found");
            return false;
        }

        codecContext_ = avcodec_alloc_context3(codec);
        if (codecContext_ == nullptr) {
            log_error("Cannot allocate decoder context");
            return false;
        }

        if (avcodec_parameters_to_context(codecContext_, codecParameters) < 0) {
            log_error("Cannot copy decoder parameters");
            return false;
        }

        if (avcodec_open2(codecContext_, codec, nullptr) < 0) {
            log_error("Cannot open decoder");
            return false;
        }

        frame_    = av_frame_alloc();
        frameRGB_ = av_frame_alloc();
        packet_   = av_packet_alloc();

        if (!frame_ || !frameRGB_ || !packet_) {
            log_error("Cannot allocate frames or packet");
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
        bitrate_ =
            formatContext_->streams[videoStreamIndex_]->codecpar->bit_rate;
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
            total_frames_ =
                formatContext_->streams[videoStreamIndex_]->nb_frames;
        }
        else {
            total_frames_ = -1;
        }

        int numBytes =
            av_image_get_buffer_size(AV_PIX_FMT_RGB24, width_, height_, 1);
        buffer_ = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
        if (buffer_ == nullptr) {
            log_error("Cannot allocate buffer");
            return false;
        }

        av_image_fill_arrays(frameRGB_->data, frameRGB_->linesize, buffer_,
                             AV_PIX_FMT_RGB24, width_, height_, 1);

        swsContext_ = sws_getContext(width_, height_, codecContext_->pix_fmt,
                                     width_, height_, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (swsContext_ == nullptr) {
            log_error("Cannot create conversion context");
            return false;
        }

        log_info(std::string("Local video opened: ") + std::to_string(width_) +
                 "x" + std::to_string(height_));
        opened_ = true;
        return true;
    }

    bool readFrame(cv::Mat& outMat)
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
                        last_pts_ =
                            static_cast<int64_t>(frame_->pts * time_base_);
                    }
                    else if (packet_->pts != AV_NOPTS_VALUE) {
                        last_pts_ =
                            static_cast<int64_t>(packet_->pts * time_base_);
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

    void stopReading()
    {
        stop_.store(true);
    }

    void setLoopPlayback(bool loop)
    {
        loop_playback_ = loop;
    }

    void setFrameSkip(bool enable)
    {
        is_interval_ = enable;
    }

    int getWidth() const
    {
        return width_;
    }
    int getHeight() const
    {
        return height_;
    }
    int64_t getLastPTS() const
    {
        return last_pts_;
    }
    double getFPS() const
    {
        return fps_;
    }
    int64_t getBitrate() const
    {
        return bitrate_;
    }
    int64_t getTotalFrames() const
    {
        return total_frames_;
    }

  private:
    void cleanup()
    {
        if (buffer_)
            av_free(buffer_);
        if (frameRGB_)
            av_frame_free(&frameRGB_);
        if (frame_)
            av_frame_free(&frame_);
        if (packet_)
            av_packet_free(&packet_);
        if (codecContext_)
            avcodec_free_context(&codecContext_);
        if (formatContext_)
            avformat_close_input(&formatContext_);
        if (swsContext_)
            sws_freeContext(swsContext_);
        opened_ = false;
    }
};

// 移除全局容器，改为每个Decoder实例内部管理

typedef struct
{
    void* puller;
    void* decoder_ffmpeg;
    void*
        decoder_instance;  // Pointer to Decoder object for updating fps/bitrate

    int            decoder_init = 0;
    image_frame_t* frame;
    uint64_t       frame_pts{0};
    // int            instance_index{0};
    bool skip_next_frame{
        false};  // Boolean flag for alternating frame skip (avoids overflow)
    int  decode_type{0};
    bool stop{false};
    bool is_interval = true;
    bool is_mpp      = false;
} app_context_t;
typedef struct
{
    image_frame_t* frame;
    uint64_t       frame_pts{0};
} frame_info;

class Decoder : public IDecoder {
  public:
    Decoder()
    {
        memset(&app_ctx_, 0, sizeof(app_context_t));
    };
    ~Decoder()
    {
        stop();
        if (worker_ && worker_->joinable()) {
            worker_->join();
        }

        // Clean up app_ctx_.frame
        if (app_ctx_.frame) {
            delete app_ctx_.frame;
            app_ctx_.frame = nullptr;
        }

        // Ensure local video reader is stopped
        if (local_video_reader_) {
            local_video_reader_->stopReading();
            local_video_reader_.reset();
        }
    };
    int               init(int timeout_open_ms = 10000) override;
    vector<long long> get_frame() override;
    int               get_null_times() override;
    int               start_pull(string video_path,
                                 int    is_mpp           = 1,
                                 int    interval         = 1,
                                 int    timeout_frame_ms = 200) override;
    int               start_pull();
    int               stop() override;
    string            get_rtsp() override;
    double            get_fps() override;
    int64_t           get_bitrate() override;
    int64_t           get_total_frames() override;
    int               get_status() override;
    void              set_loop_playback(bool loop) override;

    // Setter methods for updating video info (used in callbacks)
    void set_fps(double fps)
    {
        fps_ = fps;
    }
    void set_bitrate(int64_t bitrate)
    {
        bitrate_ = bitrate;
    }
    void set_total_frames(int64_t frames)
    {
        total_frames_ = frames;
    }
    void set_status(int status)
    {
        decoder_status_.store(status);
    }

    // Public access to synchronization primitives for callback functions
    std::mutex                 stack_mutex_;
    std::condition_variable    stack_cond_;
    std::stack<image_frame_t*> frame_stack_;

    std::atomic<bool> stop_{false};
    ;
    std::atomic<int> reopen_times_{0};

  private:
    std::atomic<int> null_frame_times_{0};
    int              failed_times_{DecoderConfig::MAX_RETRY_TIMES};

    std::shared_ptr<VideoDecoder>     decoder_soft_ = nullptr;
    std::shared_ptr<PullFramer>       puller_ = PullFramer::CreateShared();
    std::shared_ptr<LocalVideoReader> local_video_reader_ = nullptr;

    void*                        mat_addr_ = nullptr;
    string                       rtsp_url_ = "";
    app_context_t                app_ctx_;
    std::shared_ptr<std::thread> worker_;
    int  timeout_open_ms_{DecoderConfig::DEFAULT_TIMEOUT_MS};
    int  timeout_frame_ms_{DecoderConfig::DEFAULT_TIMEOUT_MS};
    bool is_local_file_{false};
    bool loop_local_video_{true};  // Loop local video by default

    // Video info cache
    double  fps_{0.0};
    int64_t bitrate_{0};
    int64_t total_frames_{-1};

    // Decoder status
    std::atomic<int> decoder_status_{DECODER_STATUS_IDLE};

    int process_video(app_context_t*      ctx,
                      const char*         path,
                      std::promise<bool>& pro);
    int process_local_video(const char* path, std::promise<bool>& pro);
};
// void mpp_decoder_frame_callback(void*    userdata,
//                                 int      width_stride,
//                                 int      height_stride,
//                                 int      width,
//                                 int      height,
//                                 int      format,
//                                 int      fd,
//                                 void*    data,
//                                 uint64_t frame_pts,
//                                 size_t   data_size);
void onGetFrame(const FrameData::Ptr& frame, void* userData1);

// Helper logging functions
static void log_error(const std::string& msg)
{
    std::cerr << "[ERROR] " << msg << std::endl;
}

static void log_info(const std::string& msg)
{
    std::cout << "[INFO] " << msg << std::endl;
}

// Helper function to check if path is RTSP URL
static bool is_rtsp_url(const std::string& path)
{
    return (path.find("rtsp://") == 0 || path.find("rtmp://") == 0);
}

// Helper function to clear frame stack
static void clear_frame_stack(Decoder* decoder)
{
    if (!decoder)
        return;

    std::unique_lock<std::mutex> lock(decoder->stack_mutex_);
    while (!decoder->frame_stack_.empty()) {
        image_frame_t* frame_item = decoder->frame_stack_.top();
        decoder->frame_stack_.pop();
        if (frame_item) {
            if (frame_item->virt_addr) {
                free(frame_item->virt_addr);
                frame_item->virt_addr = nullptr;
            }
            delete frame_item;
        }
    }
}

int Decoder::init(int timeout_open_ms)
{
    if (app_ctx_.frame) {
        delete app_ctx_.frame;
        app_ctx_.frame = nullptr;
    }
    app_ctx_.frame = new image_frame_t();

    timeout_open_ms_ = timeout_open_ms;
    // timeout_frame_ms_ = timout_frame_ms;

    app_ctx_.puller         = static_cast<void*>(&puller_);
    app_ctx_.decoder_ffmpeg = static_cast<void*>(&decoder_soft_);
    puller_->setOnGetFrame(onGetFrame, static_cast<void*>(&app_ctx_));

    return 0;
}
string Decoder::get_rtsp()
{
    return rtsp_url_;
}

double Decoder::get_fps()
{
    if (is_local_file_ && local_video_reader_) {
        return local_video_reader_->getFPS();
    }
    // For RTSP streams, return cached value
    return fps_;
}

int64_t Decoder::get_bitrate()
{
    if (is_local_file_ && local_video_reader_) {
        return local_video_reader_->getBitrate();
    }
    // For RTSP streams, return cached value
    return bitrate_;
}

int64_t Decoder::get_total_frames()
{
    if (is_local_file_ && local_video_reader_) {
        return local_video_reader_->getTotalFrames();
    }
    // For RTSP streams, return -1 (not supported)
    return -1;
}

int Decoder::get_status()
{
    return decoder_status_.load();
}

void Decoder::set_loop_playback(bool loop)
{
    loop_local_video_ = loop;
}

int Decoder::start_pull(string video_path,
                        int    is_mpp,
                        int    interval,
                        int    timeout_frame_ms)
{
    // Check if already running, stop first
    if (worker_ && worker_->joinable()) {
        log_info("Decoder already running, stopping first...");
        stop();
    }
    timeout_frame_ms_ = timeout_frame_ms;

    stop_.store(false);
    app_ctx_.stop        = false;
    app_ctx_.is_mpp      = is_mpp;
    app_ctx_.is_interval = interval;

    rtsp_url_ = video_path;

    // Set status to opening
    decoder_status_.store(DECODER_STATUS_OPENING);

    // Auto-detect if it's RTSP URL or local file
    is_local_file_ = !is_rtsp_url(video_path);

    std::promise<bool> pro;

    if (is_local_file_) {
        log_info(std::string("Starting local video file: ") + video_path +
                 (loop_local_video_ ? " (loop mode)" : " (once mode)"));
        worker_ =
            std::make_shared<std::thread>(&Decoder::process_local_video, this,
                                          video_path.c_str(), std::ref(pro));
    }
    else {
        log_info(std::string("Starting RTSP stream: ") + video_path);
        worker_ = std::make_shared<std::thread>(&Decoder::process_video, this,
                                                &app_ctx_, video_path.c_str(),
                                                std::ref(pro));
    }

    bool result = pro.get_future().get();
    if (!result) {
        decoder_status_.store(DECODER_STATUS_FAILED);
    }
    return result ? 0 : -1;
}
int Decoder::start_pull()
{
    stop_.store(false);
    app_ctx_.stop = false;

    // Set status to opening
    decoder_status_.store(DECODER_STATUS_OPENING);

    std::promise<bool> pro;

    if (is_local_file_) {
        worker_ =
            std::make_shared<std::thread>(&Decoder::process_local_video, this,
                                          rtsp_url_.c_str(), std::ref(pro));
    }
    else {
        worker_ = std::make_shared<std::thread>(&Decoder::process_video, this,
                                                &app_ctx_, rtsp_url_.c_str(),
                                                std::ref(pro));
    }

    bool result = pro.get_future().get();
    if (!result) {
        decoder_status_.store(DECODER_STATUS_FAILED);
    }
    return result ? 0 : -1;
}
void API_CALL on_mk_play_event_func(void*       user_data,
                                    int         err_code,
                                    const char* err_msg,
                                    mk_track    tracks[],
                                    int         track_count);
void API_CALL on_mk_shutdown_func(void*       user_data,
                                  int         err_code,
                                  const char* err_msg,
                                  mk_track    tracks[],
                                  int         track_count);
int           Decoder::process_video(app_context_t*      ctx,
                                     const char*         path,
                                     std::promise<bool>& pro)
{
    mk_config config;
    memset(&config, 0, sizeof(mk_config));

    config.ini         = NULL;
    config.ini_is_path = 0;
    config.log_level   = 0;
    config.log_mask    = LOG_CONSOLE;
    config.ssl         = NULL;
    config.ssl_is_path = 1;
    config.ssl_pwd     = NULL;
    config.thread_num  = 0;
    mk_env_init(&config);

    // Set decoder instance pointer for callback to update fps/bitrate
    ctx->decoder_instance = static_cast<void*>(this);

    bool      reconnect_flag = false;
    mk_player player         = mk_player_create();

    mk_player_set_on_result(player, on_mk_play_event_func, ctx);
    mk_player_set_on_shutdown(player, on_mk_shutdown_func, ctx);
    mk_player_set_option(player, "rtp_type", "0");
    mk_player_set_option(player, "protocol_timeout_ms",
                         std::to_string(timeout_open_ms_).c_str());
    mk_player_set_option(player, "media_timeout_ms",
                         std::to_string(timeout_frame_ms_).c_str());
    mk_player_set_option(player, "wait_track_ready", "true");

    std::cout << "timeout_open_ms_: " << timeout_open_ms_ << std::endl;
    std::cout << "timeout_frame_ms_: " << timeout_frame_ms_ << std::endl;
    // mk_player_set_option(player, "beat_interval_ms", "1000");

    mk_player_play(player, path);
    // if (play_ret != 0) {
    //     // log_error("Failed to start player for: " + std::string(path));
    // }
    pro.set_value(true);

    while (!stop_.load()) {
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    this_thread::sleep_for(chrono::milliseconds(1000));
    if (player) {
        // log_info("Releasing player");
        mk_player_release(player);
    }
    this_thread::sleep_for(chrono::milliseconds(1000));
    return 0;
}
void* YV12ToBGR24_OpenCV_FFMPEG(unsigned char* pYUV, int width, int height);
void  onGetFrame(const FrameData::Ptr& framePtr, void* userData1)
{
    app_context_t* ctx = (app_context_t*)userData1;

    // Check if decoder is stopping, avoid memory allocation
    if (ctx->stop || !ctx->decoder_instance) {
        return;
    }

    std::shared_ptr<VideoDecoder>* decoderPtr =
        static_cast<std::shared_ptr<VideoDecoder>*>(ctx->decoder_ffmpeg);
    auto decoder = *decoderPtr;
    if (ctx->is_interval) {
        if (ctx->skip_next_frame) {
            ctx->skip_next_frame = false;  // 翻转标志，下一帧解码
            // printf("跳过一帧\n");
            return;  // 跳过当前帧
        }
        ctx->skip_next_frame = true;  // 翻转标志，下一帧跳过
    }
    // 跳帧处理：当 is_interval=true 时，交替跳过帧（解码一帧，跳过一帧）
    // 使用布尔标志避免计数器在长时间RTSP流中溢出

    size_t data_size = 0;
    if (decoder) {
        int32_t pixel_width  = 0;
        int32_t pixel_height = 0;
        int32_t pixel_format = 0;

        // dstYUV is allocated by decoder, ownership transferred to frame
        auto dstYUV =
            decoder->decode(framePtr->data(), framePtr->size(), pixel_width,
                            pixel_height, pixel_format, data_size);
        if (dstYUV == nullptr) {
            // // log_error("Failed to decode frame");
            return;
        }

        image_frame_t* frame = new image_frame_t();
        frame->height        = pixel_height;
        frame->width         = pixel_width;
        frame->data_size     = data_size;
        frame->virt_addr     = dstYUV;  // Transfer ownership to frame

        uint64_t pts_cur = framePtr->pts() * 1000;
        ctx->frame_pts += pts_cur;
        frame->pts = ctx->frame_pts;

        // Get Decoder instance to access member variables
        Decoder* decoder_obj = static_cast<Decoder*>(ctx->decoder_instance);
        if (!decoder_obj) {
            // Cleanup if decoder instance is null
            if (frame->virt_addr) {
                free(frame->virt_addr);
                frame->virt_addr = nullptr;
            }
            delete frame;
            return;
        }

        {
            std::unique_lock<std::mutex> lock(decoder_obj->stack_mutex_);

            while (decoder_obj->frame_stack_.size() >=
                   DecoderConfig::MAX_STACK_SIZE) {
                image_frame_t* frame_item = decoder_obj->frame_stack_.top();
                decoder_obj->frame_stack_.pop();
                if (frame_item) {
                    if (frame_item->virt_addr) {
                        free(frame_item->virt_addr);
                        frame_item->virt_addr = nullptr;
                    }
                    delete frame_item;
                }
            }
            decoder_obj->frame_stack_.push(frame);
            decoder_obj->stack_cond_.notify_all();
        }
    }
}
void* YV12ToBGR24_OpenCV_FFMPEG(unsigned char* pYUV, int width, int height)
{
    if (width < 1 || height < 1 || pYUV == NULL) {
        // log_error("Invalid input parameters for YUV to BGR conversion");
        return nullptr;
    }

    unsigned char* pBGR24 = (unsigned char*)malloc(width * height * 3);
    if (pBGR24 == NULL) {
        // log_error("Failed to allocate memory for BGR24 conversion");
        return nullptr;
    }

    cv::Mat dst(height, width, CV_8UC3, pBGR24);
    cv::Mat src(height + height / 2, width, CV_8UC1, pYUV);
    cv::cvtColor(src, dst, cv::COLOR_YUV2BGR_I420);

    return pBGR24;
}
void* YV12ToBGR24_OpenCV(unsigned char* pYUV, int width, int height)
{
    if (width < 1 || height < 1 || pYUV == NULL) {
        // log_error("Invalid input parameters for YUV to BGR conversion");
        return nullptr;
    }

    unsigned char* pBGR24 = (unsigned char*)malloc(width * height * 3);
    if (pBGR24 == NULL) {
        // log_error("Failed to allocate memory for BGR24 conversion");
        return nullptr;
    }
    cv::Mat dst(height, width, CV_8UC3, pBGR24);
    cv::Mat src(height + height / 2, width, CV_8UC1, pYUV);
    cv::cvtColor(src, dst, cv::COLOR_YUV420sp2RGB);

    return pBGR24;
}
int Decoder::get_null_times()
{
    return null_frame_times_;
}

vector<long long> Decoder::get_frame()
{
    // For local video files: synchronous blocking read, no cache, no frame drop
    if (is_local_file_) {
        if (!local_video_reader_) {
            log_error("Local video reader not initialized");
            return {};
        }

        cv::Mat frame_mat;
        if (!local_video_reader_->readFrame(frame_mat)) {
            log_error("Failed to read frame from local video file");
            return {};
        }

        // Allocate memory for BGR data
        int            data_size = frame_mat.total() * frame_mat.elemSize();
        unsigned char* bgr_data  = (unsigned char*)malloc(data_size);
        if (bgr_data == nullptr) {
            log_error("Failed to allocate memory for frame data");
            return {};
        }

        memcpy(bgr_data, frame_mat.data, data_size);

        // Get actual video frame PTS timestamp (milliseconds)
        // int64_t frame_pts = local_video_reader_->getLastPTS();

        // Get video info
        double  fps          = local_video_reader_->getFPS();
        int64_t bitrate      = local_video_reader_->getBitrate();
        int64_t total_frames = local_video_reader_->getTotalFrames();

        vector<long long> mat_info = {
            (long long)bgr_data, frame_mat.cols, frame_mat.rows,
            // (long long)(fps),        // FPS * 100 (e.g., 3000 = 30.00fps)
            // (long long)bitrate/1000000.0,            // [5] Bitrate in bps
            // (long long)total_frames,        // [6] Total frames
            // (long long)frame_pts          // [3] PTS timestamp
        };

        null_frame_times_.store(0);
        return mat_info;
    }

    // For RTSP streams: use frame stack with timeout and retry logic
    image_frame_t* frame{nullptr};
    {
        std::unique_lock<std::mutex> lock(stack_mutex_);
        stack_cond_.wait_for(lock, std::chrono::milliseconds(timeout_frame_ms_),
                             [this] { return !frame_stack_.empty(); });
        if (frame_stack_.empty()) {
            stack_cond_.notify_all();
            lock.unlock();

            int null_times = null_frame_times_.fetch_add(1) + 1;
            if (null_times > failed_times_) {
                // Max retry times reached, mark as failed
                // Let upper layer handle reconnection to avoid recursion risks
                std::cout << "[WARNING] Max retry times reached (" << null_times
                          << "), connection may be lost. Call stop() and "
                             "start_pull() to reconnect."
                          << std::endl;
                decoder_status_.store(DECODER_STATUS_FAILED);

                int           reopen_count = reopen_times_.fetch_add(1) + 1;
                std::ofstream f("reopened_log.log", std::ios::app);
                if (f.is_open()) {
                    f << "Connection lost, retry count: " << reopen_count
                      << ", null_times: " << null_times << std::endl;
                    f.close();
                }
            }
            return {};
        }

        frame = frame_stack_.top();
        frame_stack_.pop();
        stack_cond_.notify_all();
    }

    // For RTSP streams, convert YUV to BGR
    void* mdata = nullptr;
    if (app_ctx_.is_mpp) {
        mdata = YV12ToBGR24_OpenCV((unsigned char*)frame->virt_addr,
                                   frame->width, frame->height);
    }
    else {
        mdata = YV12ToBGR24_OpenCV_FFMPEG((unsigned char*)frame->virt_addr,
                                          frame->width, frame->height);
    }

    // Check if conversion succeeded
    if (mdata == nullptr) {
        log_error("Failed to convert YUV to BGR");
        if (frame->virt_addr) {
            free(frame->virt_addr);
        }
        delete frame;
        return {};
    }

    // Get video info from cached values
    // double fps = get_fps();
    // int64_t bitrate = get_bitrate();
    // int64_t total_frames = get_total_frames();

    vector<long long> mat_info = {
        (long long)mdata,  // [0] BGR data
        frame->width,      // [1] Width
        frame->height,     // [2] Height
        // (long long)app_ctx_.frame_pts,   // [3] PTS timestamp
        // (long long)(fps * 100),          // [4] FPS * 100
        // (long long)bitrate,              // [5] Bitrate in bps
        // (long long)total_frames          // [6] Total frames (-1 for RTSP)
    };
    if (frame->virt_addr) {
        free(frame->virt_addr);
        frame->virt_addr = nullptr;
    }
    delete frame;

    null_frame_times_.store(0);
    return mat_info;
}

// Process local video file - synchronous mode (open and wait)
int Decoder::process_local_video(const char* path, std::promise<bool>& pro)
{
    local_video_reader_ = std::make_shared<LocalVideoReader>();

    if (!local_video_reader_->open(path)) {
        log_error(std::string("Failed to open local video file: ") +
                  std::string(path));
        decoder_status_.store(DECODER_STATUS_FAILED);
        pro.set_value(false);
        return -1;
    }

    // Set loop playback option
    local_video_reader_->setLoopPlayback(loop_local_video_);

    // Set frame skip option based on interval setting
    local_video_reader_->setFrameSkip(app_ctx_.is_interval);

    if (app_ctx_.is_interval) {
        log_info("Local video file opened with frame skipping enabled (decode "
                 "1 skip 1)");
    }
    else {
        log_info("Local video file opened, using synchronous blocking mode (no "
                 "cache, no frame drop)");
    }
    decoder_status_.store(DECODER_STATUS_OPENED);
    pro.set_value(true);

    // Just wait for stop signal, frames will be read on-demand in get_frame()
    while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (local_video_reader_) {
        local_video_reader_->stopReading();
        local_video_reader_.reset();
    }

    log_info("Local video processing stopped");
    return 0;
}

void API_CALL on_track_frame_out(void* user_data, mk_frame frame)
{
    app_context_t* ctx = (app_context_t*)user_data;

    if (!ctx->is_mpp) {
        auto pullerPtr = static_cast<std::shared_ptr<PullFramer>*>(ctx->puller);
        std::shared_ptr<PullFramer> puller = *pullerPtr;
        if (puller) {
            puller->onFrame(frame);
        }
    }
}

void API_CALL on_mk_play_event_func(void*       user_data,
                                    int         err_code,
                                    const char* err_msg,
                                    mk_track    tracks[],
                                    int         track_count)
{
    app_context_t* ctx = (app_context_t*)user_data;
    if (err_code == 0) {
        log_info("SDKPlay started successfully");

        // Update decoder status to opened
        if (ctx->decoder_instance) {
            Decoder* decoder_obj = static_cast<Decoder*>(ctx->decoder_instance);
            decoder_obj->set_status(DECODER_STATUS_OPENED);
        }
        int i;
        for (i = 0; i < track_count; ++i) {
            if (mk_track_is_video(tracks[i])) {
                // Get video info from track and update Decoder cache
                if (ctx->decoder_instance) {
                    Decoder* decoder_obj =
                        static_cast<Decoder*>(ctx->decoder_instance);

                    // Get FPS from track
                    int fps_mk = mk_track_video_fps(tracks[i]);
                    if (fps_mk > 0) {
                        double fps = static_cast<double>(fps_mk);
                        decoder_obj->set_fps(fps);
                    }
                    int bitrate = mk_track_bit_rate(tracks[i]);
                    if (bitrate > 0) {
                        decoder_obj->set_bitrate(static_cast<int64_t>(bitrate));
                    }
                }

                if (!ctx->is_mpp) {
                    auto decoderPtr =
                        static_cast<std::shared_ptr<VideoDecoder>*>(
                            ctx->decoder_ffmpeg);
                    std::shared_ptr<VideoDecoder> decoder_ffmpeg = *decoderPtr;
                    if (decoder_ffmpeg == nullptr) {
                        decoder_ffmpeg = std::make_shared<VideoDecoder>(
                            mk_track_codec_id(tracks[i]));
                        *decoderPtr = decoder_ffmpeg;
                    }
                }

                // 监听track数据回调
                mk_track_add_delegate(tracks[i], on_track_frame_out, user_data);
            }
        }
    }
    else {
        // Update decoder status to failed
        app_context_t* ctx = (app_context_t*)user_data;
        if (ctx->decoder_instance) {
            Decoder* decoder_obj = static_cast<Decoder*>(ctx->decoder_instance);
            decoder_obj->set_status(DECODER_STATUS_FAILED);
        }
    }
}

void API_CALL on_mk_shutdown_func(void*       user_data,
                                  int         err_code,
                                  const char* err_msg,
                                  mk_track    tracks[],
                                  int         track_count)
{
    // log_error("Play interrupted: code=" + std::to_string(err_code) +
    //   ", msg=" + std::string(err_msg));

    // Update decoder status to failed on shutdown
    app_context_t* ctx = (app_context_t*)user_data;
    if (ctx->decoder_instance) {
        Decoder* decoder_obj = static_cast<Decoder*>(ctx->decoder_instance);
        decoder_obj->set_status(DECODER_STATUS_FAILED);
    }
}
int Decoder::stop()
{
    stop_.store(true);
    app_ctx_.stop = true;

    // Update status to idle
    decoder_status_.store(DECODER_STATUS_IDLE);

    // Stop local video reader first
    if (local_video_reader_) {
        local_video_reader_->stopReading();
    }

    // Notify waiting threads to unblock
    stack_cond_.notify_all();

    this_thread::sleep_for(chrono::milliseconds(200));

    // Properly join and reset the worker thread
    if (worker_ != nullptr && worker_->joinable()) {
        worker_->join();
        worker_.reset();  // Reset shared_ptr to avoid keeping old thread
    }

    // Clear all remaining frames in stack
    clear_frame_stack(this);

    // log_info("Decoder stopped for instance: " +
    //          std::to_string(app_ctx_.instance_index));
    return 0;
}

static unsigned char* load_data(FILE* fp, size_t ofst, size_t sz)
{
    unsigned char* data;
    int            ret;

    data = NULL;

    if (NULL == fp) {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0) {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char*)malloc(sz);
    if (data == NULL) {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char* read_file_data(const char* filename, int* model_size)
{
    FILE*          fp;
    unsigned char* data;

    fp = fopen(filename, "rb");
    if (NULL == fp) {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}
IDecoder* xtkj::createDecoder()
{
    return new Decoder();
}

void xtkj::releaseDecoder(IDecoder* pIDecoder)
{
    delete pIDecoder;
    pIDecoder = nullptr;
}
#ifdef __cplusplus
}
#endif
