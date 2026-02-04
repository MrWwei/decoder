#include "xtkj_decoder.h"
#include "data_type.h"
#include "decoder_internal.h"
#include "decoder_utils.h"
#include "ffmpeg_decoder.h"
#include "local_video_reader.h"
#include "opencv2/opencv.hpp"
#include "pullFramer.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
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

using namespace xtkj;

// Define static constexpr members (required for ODR-usage in C++11/14)
constexpr int DecoderConfig::MAX_STACK_SIZE;
constexpr int DecoderConfig::DEFAULT_TIMEOUT_MS;
constexpr int DecoderConfig::MAX_RETRY_TIMES;
constexpr int DecoderConfig::MAX_RECONNECT_TIMES;
constexpr int DecoderConfig::RECONNECT_DELAY_MS;



#ifdef __cplusplus
extern "C" {
#endif
// Forward declarations
static void clear_frame_buffers(Decoder* decoder);
// Decoder class implementation
Decoder::Decoder()
{
    memset(&app_ctx_, 0, sizeof(app_context_t));
    mk_player_ = nullptr;
}

Decoder::~Decoder()
{
    stop();
    stop_monitor_thread();

    if (worker_ && worker_->joinable()) {
        worker_->join();
    }

    // Clean up app_ctx_.frame
    if (app_ctx_.frame) {
        delete app_ctx_.frame;
        app_ctx_.frame = nullptr;
    }

    // Clean up double buffer frames
    clear_frame_buffers(this);

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

void Decoder::set_frame_height(int frame_height)
{
    frame_height_ = frame_height;
}
void Decoder::set_frame_width(int frame_width)
{
    frame_width_ = frame_width;
}
void Decoder::set_bitrate(int64_t bitrate)
{
    bitrate_ = bitrate;
}

void Decoder::set_keep_reopen(bool keep_reopen)
{
    keep_reopen_ = keep_reopen;
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

// Helper function to clear double buffer frames
static void clear_frame_buffers(Decoder* decoder)
{
    if (!decoder)
        return;

    // 清理 current_frame_
    image_frame_t* current = decoder->current_frame_.exchange(nullptr);
    if (current) {
        if (current->virt_addr) {
            free(current->virt_addr);
            current->virt_addr = nullptr;
        }
        delete current;
    }

    // 清理 next_frame_
    image_frame_t* next = decoder->next_frame_.exchange(nullptr);
    if (next) {
        if (next->virt_addr) {
            free(next->virt_addr);
            next->virt_addr = nullptr;
        }
        delete next;
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

int Decoder::get_frame_height()
{
    return frame_height_;
}

int Decoder::get_frame_width()
{
    return frame_width_;
}

int Decoder::get_status()
{
    return decoder_status_.load();
}

void Decoder::set_loop_playback(bool loop)
{
    loop_local_video_ = loop;
}

void Decoder::set_auto_reopen(bool auto_reopen)
{
    app_ctx_.auto_reopen = auto_reopen;
    log_info(std::string("Auto-reopen set to: ") +
             (auto_reopen ? "enabled" : "disabled"));
}

bool Decoder::get_auto_reopen()
{
    return app_ctx_.auto_reopen;
}

bool Decoder::get_keep_reopen()
{
    return keep_reopen_;
}

int Decoder::start_pull(string video_path,
                        int    is_mpp,
                        int    interval,
                        int    timeout_frame_ms,
                        bool   auto_reopen)
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
    app_ctx_.auto_reopen = auto_reopen;

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

    // 启动监测线程，用于监控视频流状态
    if (result) {
        start_monitor_thread();
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
    this_thread::sleep_for(chrono::milliseconds(3000));

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

    bool reconnect_flag  = false;
    ctx->url             = string(path);
    ctx->skip_next_frame = false;
    ctx->player          = mk_player_create();
    mk_player_set_on_result((mk_player)ctx->player, on_mk_play_event_func, ctx);
    mk_player_set_on_shutdown((mk_player)ctx->player, on_mk_shutdown_func, ctx);
    // mk_player_set_option((mk_player)ctx->player, "rtp_type", "0");
    mk_player_set_option((mk_player)ctx->player, "protocol_timeout_ms",
                         std::to_string(timeout_open_ms_).c_str());
    int media_timeout_ms = 1000;
    mk_player_set_option((mk_player)ctx->player, "media_timeout_ms",
                         std::to_string(media_timeout_ms).c_str());
    mk_player_set_option((mk_player)ctx->player, "wait_track_ready", "true");

    mk_player_play((mk_player)ctx->player, path);
    printf("mk player started to play: %s\n", path);

    pro.set_value(true);

    while (!stop_.load()) {
        // printf("Decoder keep alive...\n");
        this_thread::sleep_for(chrono::milliseconds(1000));
        // mk_player_play((mk_player)ctx->player, path);
    }

    // 立即释放player，不再等待
    if (ctx->player) {
        mk_player_release((mk_player)ctx->player);
        // printf("mk player released!!!\n");
    }

    return 0;
}
void onGetFrame(const FrameData::Ptr& framePtr, void* userData1)
{
    app_context_t* ctx = (app_context_t*)userData1;

    // Check if decoder is stopping, avoid memory allocation
    if (ctx->stop || !ctx->decoder_instance) {
        return;
    }

    std::shared_ptr<VideoDecoder>* decoderPtr =
        static_cast<std::shared_ptr<VideoDecoder>*>(ctx->decoder_ffmpeg);
    auto decoder = *decoderPtr;

    // 跳帧逻辑已移到on_track_frame_out中处理，此处直接解码
    size_t data_size = 0;
    if (decoder) {
        int32_t pixel_width  = 0;
        int32_t pixel_height = 0;

        // 直接解码为BGR，避免YUV中间拷贝和转换
        auto bgr_data =
            decoder->decodeToBGR(framePtr->data(), framePtr->size(),
                                 pixel_width, pixel_height, data_size);
        if (bgr_data == nullptr) {
            return;
        }
        // 跳帧实现
        if (ctx->is_interval && ctx->skip_next_frame) {
            // printf("Skipping frame pts=%llu\n", framePtr->pts());
            // 跳过当前帧
            ctx->skip_next_frame = false;  // 重置跳帧标志
            free(bgr_data);                // 释放解码数据
            return;
        }
        else if (ctx->is_interval && !ctx->skip_next_frame) {
            // 本帧不跳，下一帧跳
            ctx->skip_next_frame = true;
        }

        image_frame_t* frame = new image_frame_t();
        frame->height        = pixel_height;
        frame->width         = pixel_width;
        frame->data_size     = data_size;
        frame->virt_addr     = bgr_data;  // 现在存储的是BGR数据
        frame->format        = 0;         // BGR format

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

        // 双缓冲机制：将新帧放入next_frame_，原子交换并释放旧帧
        // 无锁设计，性能最优，始终保存最新帧
        image_frame_t* old_frame = decoder_obj->next_frame_.exchange(frame);
        
        // 释放被替换的旧帧
        if (old_frame) {
            if (old_frame->virt_addr) {
                free(old_frame->virt_addr);
                old_frame->virt_addr = nullptr;
            }
            delete old_frame;
        }
        
        // 通知等待线程有新帧到达（可选，用于超时等待）
        {
            std::lock_guard<std::mutex> lock(decoder_obj->frame_mutex_);
            decoder_obj->frame_cond_.notify_all();
        }
    }
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

        int    frame_width  = 0;
        int    frame_height = 0;
        size_t data_size    = 0;

        // 直接获取BGR数据指针，无需memcpy
        unsigned char* bgr_data = local_video_reader_->readFrameDirect(
            frame_width, frame_height, data_size);

        if (bgr_data == nullptr) {
            log_error("Failed to read frame from local video file");
            return {};
        }

        vector<long long> mat_info = {
            (long long)bgr_data,
            frame_width,
            frame_height,
        };

        null_frame_times_.store(0);
        return mat_info;
    }

    // For RTSP streams: use double buffering with timeout and retry logic
    image_frame_t* frame{nullptr};
    
    // 尝试直接获取帧（无锁，快速路径）
    frame = next_frame_.exchange(nullptr);
    
    // 如果没有帧，等待新帧到达
    if (!frame) {
        std::unique_lock<std::mutex> lock(frame_mutex_);
        frame_cond_.wait_for(lock, std::chrono::milliseconds(timeout_frame_ms_),
                             [this] { return next_frame_.load() != nullptr; });
        lock.unlock();
        
        // 再次尝试获取帧
        frame = next_frame_.exchange(nullptr);
        
        if (!frame) {
            // 超时且仍无帧
            return {};
        }
    }

    // frame->virt_addr 现在已经是BGR数据，无需再转换
    void* bgr_data = frame->virt_addr;

    if (bgr_data == nullptr) {
        log_error("Frame data is null");
        delete frame;
        return {};
    }

    vector<long long> mat_info = {
        (long long)bgr_data,  // [0] BGR data (已经是BGR，无需转换)
        frame->width,         // [1] Width
        frame->height,        // [2] Height
    };

    // 释放frame结构体，但不释放virt_addr（调用者负责）
    frame->virt_addr = nullptr;  // 转移所有权给调用者
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
    fps_          = local_video_reader_->getFPS();
    frame_width_  = local_video_reader_->getWidth();
    frame_height_ = local_video_reader_->getHeight();
    bitrate_      = local_video_reader_->getBitrate();
    total_frames_ = local_video_reader_->getTotalFrames();

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
    printf("SDKPlay on_result called with code: %d, msg: %s\n", err_code,
           (err_msg ? err_msg : "unknown"));
    if (err_code == 0) {
        log_info("SDKPlay started successfully");

        // Update decoder status to opened

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
                    int frame_width  = mk_track_video_width(tracks[i]);
                    int frame_height = mk_track_video_height(tracks[i]);
                    decoder_obj->set_frame_height(frame_height);
                    decoder_obj->set_frame_width(frame_width);
                    if (ctx->decoder_instance) {
                        Decoder* decoder_obj =
                            static_cast<Decoder*>(ctx->decoder_instance);
                        decoder_obj->set_status(DECODER_STATUS_OPENED);
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
    std::cout << "SDKPlay shutdown called with code: " << err_code
              << ", msg: " << (err_msg ? err_msg : "unknown") << std::endl;

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

    // Stop monitor thread
    stop_monitor_thread();

    // Stop local video reader first
    if (local_video_reader_) {
        local_video_reader_->stopReading();
    }

    // Notify waiting threads to unblock
    frame_cond_.notify_all();

    this_thread::sleep_for(chrono::milliseconds(200));

    // Properly join and reset the worker thread
    if (worker_ != nullptr && worker_->joinable()) {
        worker_->join();
        worker_.reset();  // Reset shared_ptr to avoid keeping old thread
    }
    decoder_status_.store(DECODER_STATUS_IDLE);

    // Clear all remaining frames in stack
    clear_frame_stack(this);
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

// 启动监测线程
void Decoder::start_monitor_thread()
{
    // 如果监测线程已经在运行，先停止
    if (monitor_running_.load()) {
        stop_monitor_thread();
    }

    monitor_running_.store(true);
    monitor_thread_ =
        std::make_shared<std::thread>(&Decoder::monitor_stream_status, this);
    log_info("Stream monitor thread started");
}

// 停止监测线程
void Decoder::stop_monitor_thread()
{
    if (monitor_running_.load()) {
        monitor_running_.store(false);

        if (monitor_thread_ && monitor_thread_->joinable()) {
            monitor_thread_->join();
            monitor_thread_.reset();
        }
        log_info("Stream monitor thread stopped");
    }
}

// 监测线程主函数
void Decoder::monitor_stream_status()
{
    log_info("Stream monitoring started for: " + rtsp_url_);

    while (monitor_running_.load()) {
        // 每隔2秒检查一次状态
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        if (!monitor_running_.load()) {
            break;
        }

        // 检查解码器状态
        int current_status = decoder_status_.load();

        // 如果状态为失败，且设置了自动重连，则尝试重新打开流
        if (current_status == DECODER_STATUS_FAILED) {
            if (app_ctx_.auto_reopen && keep_reopen_) {
                log_info(
                    "Detected FAILED status, attempting to reopen stream: " +
                    rtsp_url_);

                // 避免重复重连
                std::lock_guard<std::mutex> lock(reconnect_mutex_);

                // 记录当前配置
                std::string video_path = rtsp_url_;

                // 停止当前流（但不停止监测线程）
                stop_.store(true);
                app_ctx_.stop = true;

                // 清理worker线程
                if (worker_ && worker_->joinable()) {
                    worker_->join();
                    worker_.reset();
                }

                // 清理帧栈
                clear_frame_stack(this);

                // 等待一段时间再重连
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    DecoderConfig::RECONNECT_DELAY_MS));

                if (!monitor_running_.load()) {
                    break;
                }

                // 重新初始化状态
                stop_.store(false);
                app_ctx_.stop = false;
                decoder_status_.store(DECODER_STATUS_OPENING);

                // 重新启动拉流
                std::promise<bool> pro;

                if (is_local_file_) {
                    log_info("Reopening local video file: " + video_path);
                    worker_ = std::make_shared<std::thread>(
                        &Decoder::process_local_video, this, video_path.c_str(),
                        std::ref(pro));
                }
                else {
                    log_info("Reopening RTSP stream: " + video_path);
                    worker_ = std::make_shared<std::thread>(
                        &Decoder::process_video, this, &app_ctx_,
                        video_path.c_str(), std::ref(pro));
                }

                bool result = pro.get_future().get();
                if (result) {
                    log_info("Successfully reopened stream: " + video_path);
                }
                else {
                    log_error("Failed to reopen stream: " + video_path);
                    decoder_status_.store(DECODER_STATUS_FAILED);
                }
            }
            else {
                log_info("Stream failed but auto-reopen is disabled");
                // 如果不自动重连，退出监测循环
                if (!keep_reopen_) {
                    break;
                }
            }
        }
    }

    log_info("Stream monitoring stopped");
}

#ifdef __cplusplus
}
#endif
