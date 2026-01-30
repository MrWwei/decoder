#include "xtkj_decoder.h"
#include "decoder_internal.h"
#include "decoder_utils.h"
#include "local_video_reader.h"
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

// Decoder class implementation
Decoder::Decoder()
{
    memset(&app_ctx_, 0, sizeof(app_context_t));
}

Decoder::~Decoder()
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
}

void Decoder::set_fps(double fps)
{
    fps_ = fps;
}

void Decoder::set_bitrate(int64_t bitrate)
{
    bitrate_ = bitrate;
}

void Decoder::set_total_frames(int64_t frames)
{
    total_frames_ = frames;
}

void Decoder::set_status(int status)
{
    decoder_status_.store(status);
}

void onGetFrame(const FrameData::Ptr& frame, void* userData1);
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
void* YV12ToBGR24_OpenCV_FFMPEG(unsigned char* pYUV, int width, int height);
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
                stop();
                start_pull();

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
