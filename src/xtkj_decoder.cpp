#include "xtkj_decoder.h"
// #include "RgaUtils.h"
#include "data_type.h"
#include "ffmpeg_decoder.h"
// #include "im2d.h"
#include "mk_mediakit.h"
#include "opencv2/opencv.hpp"
#include "pullFramer.h"
// #include "rga.h"
#include "utils/mpp_decoder.h"
#include "utils/mpp_encoder.h"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <queue>
#include <stack>
#include <thread>
#include <unordered_set>

#ifdef __cplusplus
extern "C" {
#endif

using namespace xtkj;
static unsigned char* load_data(FILE* fp, size_t ofst, size_t sz);
static unsigned char* read_file_data(const char* filename, int* model_size);
void* YV12ToBGR24_OpenCV(unsigned char* pYUV, int width, int height);
vector<std::mutex>              stack_mutexs_(16);
vector<std::condition_variable> stack_conds_(16);

#define MAX_STACK_SIZE 1
// std::condition_variable cv_;
// bool resourceReady = false;

typedef struct
{
    MppDecoder* decoder_mpp{nullptr};
    void*       puller;
    void*       decoder_ffmpeg;

    int            decoder_init = 0;
    image_frame_t* frame;
    uint64_t       frame_pts{0};
    int            instance_index{0};
    int            frame_count{0};
    int            decode_type{0};
    bool           stop{false};
    bool           is_mpp      = true;
    bool           is_interval = true;
} app_context_t;
typedef struct
{
    image_frame_t* frame;
    uint64_t       frame_pts{0};
} frame_info;
vector<std::stack<image_frame_t*>> frame_stacks_(16);

class Decoder : public IDecoder {
  public:
    Decoder()
    {
        memset(&app_ctx_, 0, sizeof(app_context_t));
    };
    ~Decoder()
    {
        stop();
        if (worker_->joinable())
            worker_->join();
    };
    int               init(int decode_thread_num,
                           int frame_interval = 1,
                           int timeout_ms     = 10000) override;
    vector<long long> get_frame() override;
    int               get_null_times() override;
    int
           start_pull(string video_path, int is_mpp = 1, int interval = 1) override;
    int    start_pull();
    int    stop() override;
    string get_rtsp() override;
    bool   stop_ = false;
    int    failed_times{2};
    int    reopen_times_{0};

  private:
    int null_frame_times_{0};

    std::shared_ptr<VideoDecoder> decoder_soft_ = nullptr;
    std::shared_ptr<PullFramer>   puller_       = PullFramer::CreateShared();

    void*                        mat_addr_ = nullptr;
    string                       rtsp_url_ = "";
    app_context_t                app_ctx_;
    std::shared_ptr<std::thread> worker_;
    int                          timeout_ms_{5000};

    int process_video(app_context_t*      ctx,
                      const char*         path,
                      std::promise<bool>& pro);
};
void mpp_decoder_frame_callback(void*    userdata,
                                int      width_stride,
                                int      height_stride,
                                int      width,
                                int      height,
                                int      format,
                                int      fd,
                                void*    data,
                                uint64_t frame_pts,
                                size_t   data_size);
void onGetFrame(const FrameData::Ptr& frame, void* userData1, int instance_id);
int  Decoder::init(int decode_thread_num, int frame_interval, int timeout_ms)
{
    app_ctx_.frame = new image_frame_t();
    // int video_type = decode_type;
    app_ctx_.instance_index = decode_thread_num;
    // app_ctx_.is_mpp         = is_mpp > 0 ? true : false;

    timeout_ms_ = timeout_ms;

    if (app_ctx_.decoder_mpp == nullptr) {
        app_ctx_.decode_type = 0;
        app_ctx_.decoder_mpp = new MppDecoder();

        app_ctx_.decoder_mpp->Reset();
        app_ctx_.decoder_mpp->SetCallback(mpp_decoder_frame_callback);
    }

    app_ctx_.puller         = static_cast<void*>(&puller_);
    app_ctx_.decoder_ffmpeg = static_cast<void*>(&decoder_soft_);
    puller_->setOnGetFrame(onGetFrame, static_cast<void*>(&app_ctx_),
                           app_ctx_.instance_index);

    printf("init ok!\n");
    return 0;
}
string Decoder::get_rtsp()
{
    return rtsp_url_;
}
int Decoder::start_pull(string video_path, int is_mpp, int interval)
{
    stop_                = false;
    app_ctx_.stop        = stop_;
    app_ctx_.is_mpp      = is_mpp;
    app_ctx_.is_interval = interval;

    rtsp_url_ = video_path;
    std::promise<bool> pro;
    worker_ =
        std::make_shared<std::thread>(&Decoder::process_video, this, &app_ctx_,
                                      video_path.c_str(), std::ref(pro));
    return pro.get_future().get();
}
int Decoder::start_pull()
{
    stop_         = false;
    app_ctx_.stop = stop_;
    std::promise<bool> pro;
    worker_ =
        std::make_shared<std::thread>(&Decoder::process_video, this, &app_ctx_,
                                      rtsp_url_.c_str(), std::ref(pro));

    return pro.get_future().get();
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
    bool      reconnect_flag = false;
    mk_player player         = mk_player_create();

    mk_player_set_on_result(player, on_mk_play_event_func, ctx);
    mk_player_set_on_shutdown(player, on_mk_shutdown_func, ctx);
    mk_player_set_option(player, "rtp_type", "0");
    mk_player_set_option(player, "protocol_timeout_ms", "5000");
    mk_player_set_option(player, "media_timeout_ms", "5000");
    mk_player_set_option(player, "wait_track_ready", "false");
    // mk_player_set_option(player, "beat_interval_ms", "1000");

    mk_player_play(player, path);
    pro.set_value(true);
    // }
    while (!stop_) {
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    this_thread::sleep_for(chrono::milliseconds(1000));
    if (player) {
        printf("release player\n");
        mk_player_release(player);
    }
    // }
    this_thread::sleep_for(chrono::milliseconds(1000));
    return 0;
}
void* YV12ToBGR24_OpenCV_FFMPEG(unsigned char* pYUV, int width, int height);
void  onGetFrame(const FrameData::Ptr& framePtr,
                 void*                 userData1,
                 int                   instance_id)
{
    app_context_t* ctx = (app_context_t*)userData1;

    std::shared_ptr<VideoDecoder>* decoderPtr =
        static_cast<std::shared_ptr<VideoDecoder>*>(ctx->decoder_ffmpeg);
    auto decoder = *decoderPtr;
    ctx->frame_count++;
    if (ctx->is_interval)
        if (ctx->frame_count % 2 == 0) {
            return;
        }
    size_t data_size = 0;
    if (decoder) {
        int32_t pixel_width  = 0;
        int32_t pixel_height = 0;
        int32_t pixel_format = 0;
        // auto start = std::chrono::steady_clock::now();
        auto dstYUV =
            decoder->decode(framePtr->data(), framePtr->size(), pixel_width,
                            pixel_height, pixel_format, data_size);
        if (dstYUV == nullptr) {
            std::cerr << "decode error" << std::endl;
            return;
        }
        image_frame_t* frame = new image_frame_t();
        frame->height        = pixel_height;
        frame->width         = pixel_width;
        frame->data_size     = data_size;
        // frame->virt_addr     = malloc(data_size);
        frame->virt_addr = dstYUV;
        // memcpy(frame->virt_addr, dstYUV, data_size);

        uint64_t pts_cur = framePtr->pts() * 1000;
        ctx->frame_pts += pts_cur;
        frame->pts = ctx->frame_pts;
        {
            std::unique_lock<std::mutex> lock(stack_mutexs_[instance_id]);

            while (frame_stacks_[instance_id].size() >= MAX_STACK_SIZE) {
                image_frame_t* frame_item = frame_stacks_[instance_id].top();
                frame_stacks_[instance_id].pop();
                if (frame_item->virt_addr != nullptr) {
                    free(frame_item->virt_addr);
                    frame_item->virt_addr = nullptr;
                }
                if (frame_item != nullptr) {
                    delete frame_item;
                    frame_item = nullptr;
                }
            }

            frame_stacks_[instance_id].push(frame);
            stack_conds_[instance_id].notify_all();
        }
    }
}
void* YV12ToBGR24_OpenCV_FFMPEG(unsigned char* pYUV, int width, int height)
{
    unsigned char* pBGR24 = (unsigned char*)malloc(width * height * 3);

    if (width < 1 || height < 1 || pYUV == NULL || pBGR24 == NULL) {
        if (pBGR24) {
            free(pBGR24);
            pBGR24 = nullptr;
        }
        return nullptr;
    }

    cv::Mat dst(height, width, CV_8UC3, pBGR24);
    cv::Mat src(height + height / 2, width, CV_8UC1, pYUV);
    cv::cvtColor(src, dst, cv::COLOR_YUV2BGR_I420);

    return pBGR24;
}
void* YV12ToBGR24_OpenCV(unsigned char* pYUV, int width, int height)
{
    unsigned char* pBGR24 = (unsigned char*)malloc(width * height * 3);

    if (width < 1 || height < 1 || pYUV == NULL || pBGR24 == NULL) {
        if (pBGR24 != nullptr) {
            free(pBGR24);
            pBGR24 = nullptr;
        }
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

void mpp_decoder_frame_callback(void*    userdata,
                                int      width_stride,
                                int      height_stride,
                                int      width,
                                int      height,
                                int      format,
                                int      fd,
                                void*    data,
                                uint64_t frame_pts,
                                size_t   data_size)
{
    app_context_t* ctx = (app_context_t*)userdata;
    ctx->frame_count++;
    if (ctx->is_interval)
        if (ctx->frame_count % 2 == 0)
            return;

    {
        std::unique_lock<std::mutex> lock(stack_mutexs_[ctx->instance_index]);
        image_frame_t*               frame = new image_frame_t();
        frame->height                      = height;
        frame->width                       = width;
        frame->data_size                   = data_size;
        frame->height_stride               = height_stride;
        frame->width_stride                = width_stride;
        frame->virt_addr                   = malloc(data_size);
        memcpy(frame->virt_addr, data, data_size);
        ctx->frame_pts += frame_pts;
        frame->pts = ctx->frame_pts;

        while (frame_stacks_[ctx->instance_index].size() >= MAX_STACK_SIZE) {
            image_frame_t* frame_item =
                frame_stacks_[ctx->instance_index].top();
            frame_stacks_[ctx->instance_index].pop();
            if (frame_item->virt_addr != nullptr) {
                free(frame_item->virt_addr);
                frame_item->virt_addr = nullptr;
            }
            if (frame_item != nullptr) {
                delete frame_item;
                frame_item = nullptr;
            }
        }
        frame_stacks_[ctx->instance_index].push(frame);
        stack_conds_[ctx->instance_index].notify_all();
    }

    return;
}
vector<long long> Decoder::get_frame()
{
    image_frame_t* frame{nullptr};
    {
        std::unique_lock<std::mutex> lock(
            stack_mutexs_[app_ctx_.instance_index]);
        int ins_index = app_ctx_.instance_index;
        stack_conds_[app_ctx_.instance_index].wait_for(
            lock, std::chrono::milliseconds(timeout_ms_),
            [ins_index] { return !frame_stacks_[ins_index].empty(); });
        if (frame_stacks_[ins_index].empty()) {
            stack_conds_[app_ctx_.instance_index].notify_all();
            lock.unlock();
            null_frame_times_++;
            if (null_frame_times_ > failed_times) {
                stop();
                start_pull();

                null_frame_times_ = 0;
                reopen_times_++;
                std::ofstream f("sdk_log_" +
                                    std::to_string(app_ctx_.instance_index) +
                                    ".log",
                                std::ios::app);
                f << "reopen times:" << std::to_string(reopen_times_)
                  << std::endl;
                f.close();
            }
            return {};
        }

        frame = frame_stacks_[ins_index].top();
        frame_stacks_[ins_index].pop();
        stack_conds_[app_ctx_.instance_index].notify_all();
    }
    void* mdata = nullptr;
    if (app_ctx_.is_mpp) {
        mdata = YV12ToBGR24_OpenCV((unsigned char*)frame->virt_addr,
                                   frame->width_stride, frame->height_stride);
    }
    else
        mdata = YV12ToBGR24_OpenCV_FFMPEG((unsigned char*)frame->virt_addr,
                                          frame->width, frame->height);
    vector<long long> mat_info = {(long long)mdata, frame->width_stride,
                                  frame->height_stride,
                                  (long long)app_ctx_.frame_pts};
    if (frame->virt_addr != nullptr) {
        free(frame->virt_addr);
        frame->virt_addr = nullptr;
    }
    ////////////////////////////////////////// delete frame
    if (frame != nullptr) {
        delete frame;
        frame = nullptr;
    }
    null_frame_times_ = 0;
    return mat_info;
}
void API_CALL on_track_frame_out(void* user_data, mk_frame frame)
{
    app_context_t* ctx = (app_context_t*)user_data;

    if (ctx->is_mpp) {
        const char* data     = mk_frame_get_data(frame);
        size_t      size     = mk_frame_get_data_size(frame);
        uint16_t    pts_time = mk_frame_get_pts(frame) * 1000;  // 微秒
        ctx->decoder_mpp->multi_dec_simple((uint8_t*)data, size, pts_time, 0,
                                           ctx->instance_index);
    }

    else {
        auto pullerPtr = static_cast<std::shared_ptr<PullFramer>*>(ctx->puller);
        std::shared_ptr<PullFramer> puller = *pullerPtr;
        if (puller) {
            puller->onFrame(frame, ctx->instance_index);
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
        // success
        printf("play success!");
        int i;
        for (i = 0; i < track_count; ++i) {
            if (mk_track_is_video(tracks[i])) {
                // log_info("got video track: %s",
                // mk_track_codec_name(tracks[i]));
                if (ctx->is_mpp) {
                    int32_t codec_id = mk_track_codec_id(tracks[i]);
                    if (ctx->decoder_init == 0) {
                        ctx->decoder_mpp->Init(codec_id, 30, ctx);
                        std::cout << "av type:" << codec_id << std::endl;
                        ctx->decoder_init = 1;
                    }
                }
                else {
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
        printf("play failed: %d %s", err_code, err_msg);
    }
}

void API_CALL on_mk_shutdown_func(void*       user_data,
                                  int         err_code,
                                  const char* err_msg,
                                  mk_track    tracks[],
                                  int         track_count)
{
    printf("play interrupted: %d %s", err_code, err_msg);
}
int Decoder::stop()
{
    stop_         = true;
    app_ctx_.stop = stop_;
    this_thread::sleep_for(chrono::milliseconds(200));
    if (worker_ != nullptr)
        if (worker_->joinable())
            worker_->join();
    {
        std::unique_lock<std::mutex> lock(
            stack_mutexs_[app_ctx_.instance_index]);
        int ins_index = app_ctx_.instance_index;
        while (frame_stacks_[app_ctx_.instance_index].size() >=
               MAX_STACK_SIZE) {
            image_frame_t* frame_item =
                frame_stacks_[app_ctx_.instance_index].top();
            frame_stacks_[app_ctx_.instance_index].pop();
            if (frame_item->virt_addr != nullptr) {
                free(frame_item->virt_addr);
                frame_item->virt_addr = nullptr;
            }
            if (frame_item != nullptr) {
                delete frame_item;
                frame_item = nullptr;
            }
        }
    }
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
