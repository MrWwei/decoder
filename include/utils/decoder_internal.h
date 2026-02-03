#ifndef DECODER_INTERNAL_H
#define DECODER_INTERNAL_H

#include "data_type.h"
#include "ffmpeg_decoder.h"
#include "local_video_reader.h"
#include "pullFramer.h"
#include "xtkj_decoder.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stack>
#include <string>
#include <thread>

namespace xtkj {

// Configuration structure
struct DecoderConfig
{
    static constexpr int MAX_STACK_SIZE      = 1;
    static constexpr int DEFAULT_TIMEOUT_MS  = 10000;
    static constexpr int MAX_RETRY_TIMES     = 50;
    static constexpr int MAX_RECONNECT_TIMES = 20;   // 最大重连次数
    static constexpr int RECONNECT_DELAY_MS = 3000;  // 重连延迟（毫秒）
};

typedef struct
{
    void* puller;
    void* decoder_ffmpeg;
    void* decoder_instance;

    int            decoder_init = 0;
    image_frame_t* frame;
    uint64_t       frame_pts{0};
    bool           skip_next_frame{false};
    int            decode_type{0};
    bool           stop{false};
    bool           auto_reopen = true;
    bool           is_interval = true;
    bool           is_mpp      = false;
    void*          player;
    string         url;
} app_context_t;

class Decoder : public IDecoder {
  public:
    Decoder();
    ~Decoder();

    int               init(int timeout_open_ms = 10000) override;
    vector<long long> get_frame() override;
    int               get_null_times() override;
    int               start_pull(string video_path,
                                 int    is_mpp           = 1,
                                 int    interval         = 1,
                                 int    timeout_frame_ms = 200,
                                 bool   auto_reopen      = true) override;
    int               start_pull();
    int               stop() override;
    string            get_rtsp() override;
    double            get_fps() override;
    int64_t           get_bitrate() override;
    int64_t           get_total_frames() override;
    int               get_frame_width() override;
    int               get_frame_height() override;
    int               get_status() override;
    void              set_loop_playback(bool loop) override;
    void              set_auto_reopen(bool auto_reopen) override;
    bool              get_auto_reopen() override;
    bool              get_keep_reopen() override;

    // Setter methods for updating video info (used in callbacks)
    void set_fps(double fps);
    void set_bitrate(int64_t bitrate);
    void set_total_frames(int64_t frames);
    void set_frame_width(int frame_width);
    void set_frame_height(int frame_height);
    void set_status(int status);
    void set_keep_reopen(bool keep_reopen);

    // Public access to synchronization primitives for callback functions
    std::mutex                 stack_mutex_;
    std::condition_variable    stack_cond_;
    std::stack<image_frame_t*> frame_stack_;

    std::atomic<bool> stop_{false};
    std::atomic<int>  reopen_times_{0};

    // Reconnection control
    std::atomic<bool>            is_reconnecting_{false};
    std::atomic<int>             reconnect_count_{0};
    std::shared_ptr<std::thread> reconnect_thread_;

    // Stream monitoring thread
    std::shared_ptr<std::thread> monitor_thread_;
    std::atomic<bool>            monitor_running_{false};
    void                         start_monitor_thread();
    void                         stop_monitor_thread();
    void                         monitor_stream_status();

  private:
    std::atomic<int> null_frame_times_{0};
    int              failed_times_{DecoderConfig::MAX_RETRY_TIMES};

    // Stream monitoring
    std::atomic<int64_t> last_frame_time_{0};
    std::mutex           reconnect_mutex_;

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
    bool loop_local_video_{false};

    // Video info cache
    double  fps_{0.0};
    int64_t bitrate_{0};
    int64_t total_frames_{-1};
    int     frame_width_{0};
    int     frame_height_{0};
    bool    keep_reopen_{true};

    // Decoder status
    std::atomic<int> decoder_status_{DECODER_STATUS_IDLE};

    // mk_player pointer and mutex for safe stop
    std::mutex player_mutex_;
    void*      mk_player_{nullptr};

    int process_video(app_context_t*      ctx,
                      const char*         path,
                      std::promise<bool>& pro);
    int process_local_video(const char* path, std::promise<bool>& pro);
};

}  // namespace xtkj

#endif  // DECODER_INTERNAL_H
