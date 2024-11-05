#include <stdio.h>
#include <sys/time.h>

#include "mpp_decoder.h"
#include "mpp_log.h"
#include "mpp_time.h"
#include <chrono>
#include <iostream>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LOGD printf
// #define LOGD

static unsigned long GetCurrentTimeMS()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

MppDecoder::MppDecoder() {}

MppDecoder::~MppDecoder()
{
    exit(0);
    if (loop_data.packet) {
        mpp_packet_deinit(&loop_data.packet);
        loop_data.packet = NULL;
    }
    if (frame) {
        mpp_frame_deinit(&frame);
        frame = NULL;
    }
    if (mpp_ctx) {
        mpp_destroy(mpp_ctx);
        mpp_ctx = NULL;
    }

    if (loop_data.frm_grp) {
        mpp_buffer_group_put(loop_data.frm_grp);
        loop_data.frm_grp = NULL;
    }
}

int MppDecoder::Init(int video_type, int fps, void* userdata)
{
    MPP_RET ret              = MPP_OK;
    this->userdata           = userdata;
    this->fps                = fps;
    this->last_frame_time_ms = 0;
    if (video_type == 0) {
        printf("video type:H264\n");
        mpp_type = MPP_VIDEO_CodingAVC;
    }
    else if (video_type == 1) {
        printf("video type:H265\n");
        mpp_type = MPP_VIDEO_CodingHEVC;
    }
    else {
        LOGD("unsupport video_type %d", video_type);
        return -1;
    }
    LOGD("mpi_dec_test start ");
    memset(&loop_data, 0, sizeof(loop_data));
    LOGD("mpi_dec_test decoder test start mpp_type %d ", mpp_type);

    MppDecCfg cfg = NULL;

    MppCtx mpp_ctx = NULL;
    ret            = mpp_create(&mpp_ctx, &mpp_mpi);
    if (MPP_OK != ret) {
        LOGD("mpp_create failed ");
        return 0;
    }

    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, mpp_type);
    if (ret) {
        LOGD("%p mpp_init failed ", mpp_ctx);
        return -1;
    }

    mpp_dec_cfg_init(&cfg);

    /* get default config from decoder context */
    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        LOGD("%p failed to get decoder cfg ret %d ", mpp_ctx, ret);
        return -1;
    }

    /*
     * split_parse is to enable mpp internal frame spliter when the input
     * packet is not aplited into frames.
     */
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        LOGD("%p failed to set split_parse ret %d ", mpp_ctx, ret);
        return -1;
    }

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        LOGD("%p failed to set cfg %p ret %d ", mpp_ctx, cfg, ret);
        return -1;
    }

    mpp_dec_cfg_deinit(cfg);

    loop_data.ctx         = mpp_ctx;
    loop_data.mpi         = mpp_mpi;
    loop_data.eos         = 0;
    loop_data.packet_size = packet_size;
    loop_data.frame       = 0;
    loop_data.frame_count = 0;
    return 1;
}

int MppDecoder::Reset()
{
    if (mpp_mpi != NULL) {
        mpp_mpi->reset(mpp_ctx);
    }

    return 0;
}
typedef struct
{
    // MpiDecTestCmd *cmd;
    MppCtx  ctx;
    MppApi* mpi;

    /* end of stream flag when set quit the loop */
    RK_U32 loop_end;

    /* input and output */
    // DecBufMgr buf_mgr;
    MppBufferGroup frm_grp;
    MppPacket      packet;
    MppFrame       frame;

    FILE*  fp_output;
    RK_S32 packet_count;
    RK_S32 frame_count;
    RK_S32 frame_num;

    RK_S64 first_pkt;
    RK_S64 first_frm;

    /* runtime flag */
    RK_U32 quiet;
} MpiDecMultiCtx;
int MppDecoder::multi_dec_simple(uint8_t* pkt_data,
                                 int      pkt_size,
                                 uint64_t frame_pts,
                                 int      pkt_eos,
                                 int      instance_id)
{
    MpiDecLoopData* data     = &loop_data;
    MppCtx          ctx      = data->ctx;
    MppApi*         mpi      = data->mpi;
    RK_U32          pkt_done = 0;
    MPP_RET         ret      = MPP_OK;
    if (packet == NULL) {
        ret = mpp_packet_init(&packet, NULL, 0);
    }
    RK_U32 quiet = data->quiet;

    mpp_packet_set_data(packet, pkt_data);
    mpp_packet_set_size(packet, pkt_size);
    mpp_packet_set_pos(packet, pkt_data);
    mpp_packet_set_length(packet, pkt_size);
    // setup eos flag
    if (pkt_eos)
        mpp_packet_set_eos(packet);

    do {
        RK_U32 frm_eos = 0;
        RK_S32 times   = 5;
        // send the packet first if packet is not done
        if (!pkt_done) {
            ret = mpi->decode_put_packet(ctx, packet);
            if (MPP_OK == ret) {
                pkt_done = 1;
            }
        }

        // then get all available frame and release
        do {
            RK_S32   get_frm = 0;
            MppFrame frame   = NULL;

        try_again:
            ret = mpi->decode_get_frame(ctx, &frame);
            if (MPP_ERR_TIMEOUT == ret) {
                if (times > 0) {
                    times--;
                    msleep(2);
                    goto try_again;
                }
                mpp_err("decode_get_frame failed too much time\n");
            }
            if (ret) {
                mpp_err("decode_get_frame failed ret %d\n", ret);
                break;
            }

            if (frame) {
                RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
                RK_U32 hor_width  = mpp_frame_get_width(frame);
                RK_U32 ver_height = mpp_frame_get_height(frame);
                RK_U32 buf_size   = mpp_frame_get_buf_size(frame);
                RK_S64 pts        = mpp_frame_get_pts(frame);
                RK_S64 dts        = mpp_frame_get_dts(frame);
                if (mpp_frame_get_info_change(frame)) {
                    // MppBufferGroup grp = NULL;
                    // MppDecBufMode buf_mode =
                    //     MppDecBufMode::MPP_DEC_BUF_HALF_INT;
                    /* Set buffer to mpp decoder */
                    ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP,
                                       data->frm_grp);
                    if (ret) {
                        mpp_err("%p set buffer group failed ret %d\n", ctx,
                                ret);
                        break;
                    }
                    // data->frm_grp = grp;

                    /*
                     * All buffer group config done. Set info change ready to
                     * let decoder continue decoding
                     */
                    ret =
                        mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (ret) {
                        mpp_err("info change ready failed ret %d\n", ret);
                        break;
                    }
                }
                else {
                    RK_U32 err_info = 0;

                    err_info = mpp_frame_get_errinfo(frame) |
                               mpp_frame_get_discard(frame);
                    if (err_info) {
                        mpp_log_q(
                            quiet,
                            "decoder_get_frame get err info:%d discard:%d.\n",
                            mpp_frame_get_errinfo(frame),
                            mpp_frame_get_discard(frame));
                    }
                    // mpp_log_q(quiet, "decode_get_frame get frame %d\n",
                    //           data->frame_count);

                    data->frame_count++;
                    if (!err_info) {
                        if (callback != nullptr) {
                            MppFrameFormat format   = mpp_frame_get_fmt(frame);
                            char*          data_vir = (char*)mpp_buffer_get_ptr(
                                mpp_frame_get_buffer(frame));
                            size_t data_size = mpp_frame_get_buf_size(frame);
                            int    fd =
                                mpp_buffer_get_fd(mpp_frame_get_buffer(frame));
                            callback(this->userdata, hor_stride, ver_stride,
                                     hor_width, ver_height, format, fd,
                                     data_vir, frame_pts, data_size);
                        }
                    }
                }
                frm_eos = mpp_frame_get_eos(frame);
                mpp_frame_deinit(&frame);
                get_frm = 1;
            }
            ret = MPP_NOK;
            // if last packet is send but last frame is not found continue
            if (pkt_eos && pkt_done && !frm_eos) {
                msleep(1);
                continue;
            }

            if ((data->frame_num > 0 &&
                 (data->frame_count >= data->frame_num)) ||
                ((data->frame_num == 0) && frm_eos))
                break;

            if (get_frm)
                continue;
            break;
        } while (1);

        if ((data->frame_num > 0 && (data->frame_count >= data->frame_num)) ||
            ((data->frame_num == 0) && frm_eos)) {
            // data->loop_end = 1;
            break;
        }

        if (pkt_done)
            break;

        /*
         * why sleep here:
         * mpi->decode_put_packet will failed when packet in internal queue is
         * full,waiting the package is consumed .Usually hardware decode one
         * frame which resolution is 1080p needs 2 ms,so here we sleep 3ms
         * * is enough.
         */
        msleep(1);
    } while (1);

    return ret;
}
void* MppDecoder::multi_dec_decode(uint8_t*      pkt_data,
                                   int           pkt_size,
                                   uint64_t      frame_pts,
                                   int           pkt_eos,
                                   MppCodingType dec_type)
{
    // MpiDecMultiCtxInfo *info = (MpiDecMultiCtxInfo *)cmd_ctx;
    // MpiDecMultiCtx *dec_ctx = &info->ctx;
    // MpiDecMultiCtxRet *rets = &info->ret;
    // MpiDecTestCmd *cmd = info->cmd;
    MpiDecLoopData* data = &loop_data;
    MPP_RET         ret  = MPP_OK;

    // base flow context
    MppCtx  ctx = data->ctx;
    MppApi* mpi = data->mpi;

    // input / output

    MppPacket packet = data->packet;
    MppFrame  frame  = data->frame;

    // config for runtime mode
    MppDecCfg cfg        = NULL;
    RK_U32    need_split = 1;

    // paramter for resource malloc
    // RK_U32 width = cmd->width;
    // RK_U32 height = cmd->height;
    // MppCodingType type = cmd->type;

    // resources
    MppBuffer frm_buf = NULL;

    // ret = dec_buf_mgr_init(&dec_ctx->buf_mgr);
    if (ret) {
        mpp_err("dec_buf_mgr_init failed\n");
        goto MPP_TEST_OUT;
    }

    ret = mpp_packet_init(&packet, NULL, 0);
    if (ret) {
        mpp_err("mpp_packet_init failed\n");
        goto MPP_TEST_OUT;
    }

    // decoder demo
    ret = mpp_create(&ctx, &mpi);
    if (ret) {
        mpp_err("mpp_create failed\n");
        goto MPP_TEST_OUT;
    }

    // mpp_log("%p mpi_dec_test decoder test start w %d h %d type %d\n",
    //         ctx, width, height, type);

    ret = mpp_init(ctx, MPP_CTX_DEC, dec_type);
    if (ret) {
        mpp_err("mpp_init failed\n");
        goto MPP_TEST_OUT;
    }

    mpp_dec_cfg_init(&cfg);

    /* get default config from decoder context */
    ret = mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        mpp_err("%p failed to get decoder cfg ret %d\n", ctx, ret);
        goto MPP_TEST_OUT;
    }

    /*
     * split_parse is to enable mpp internal frame spliter when the input
     * packet is not aplited into frames.
     */
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        mpp_err("%p failed to set split_parse ret %d\n", ctx, ret);
        goto MPP_TEST_OUT;
    }

    ret = mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        mpp_err("%p failed to set cfg %p ret %d\n", ctx, cfg, ret);
        goto MPP_TEST_OUT;
    }

    // dec_ctx->cmd = cmd;
    // dec_ctx->ctx = ctx;
    // dec_ctx->mpi = mpi;
    // dec_ctx->packet = packet;
    // dec_ctx->frame = frame;
    // dec_ctx->packet_count = 0;
    // dec_ctx->frame_count = 0;
    // dec_ctx->frame_num = cmd->frame_num;
    // dec_ctx->quiet = cmd->quiet;

    RK_S64 t_s, t_e;

    t_s = mpp_time();
    // multi_dec_simple(pkt_data, pkt_size, frame_pts, pkt_eos);

    t_e = mpp_time();

    ret = mpi->reset(ctx);
    if (ret) {
        mpp_err("mpi->reset failed\n");
        goto MPP_TEST_OUT;
    }

    // rets->elapsed_time = t_e - t_s;
    // rets->frame_count = dec_ctx->frame_count;
    // rets->frame_rate = (float)dec_ctx->frame_count * 1000000 /
    // rets->elapsed_time; rets->delay = dec_ctx->first_frm -
    // dec_ctx->first_pkt;

MPP_TEST_OUT:
    if (packet) {
        mpp_packet_deinit(&packet);
        packet = NULL;
    }

    if (frame) {
        mpp_frame_deinit(&frame);
        frame = NULL;
    }

    if (ctx) {
        mpp_destroy(ctx);
        ctx = NULL;
    }

    data->frm_grp = NULL;
    // if (dec_ctx->buf_mgr)
    // {
    //     dec_buf_mgr_deinit(dec_ctx->buf_mgr);
    //     dec_ctx->buf_mgr = NULL;
    // }

    if (cfg) {
        mpp_dec_cfg_deinit(cfg);
        cfg = NULL;
    }

    return NULL;
}
int MppDecoder::DecodeV1(uint8_t* pkt_data,
                         int      pkt_size,
                         uint64_t frame_pts,
                         int      pkt_eos)
// int multi_dec_simple(uint8_t* pkt_data, int pkt_size, uint64_t frame_pts, int
// pkt_eos)
{
    printf("1111111111111111\n");
    // MpiDecTestCmd *cmd = data->cmd;
    MpiDecLoopData* data     = &loop_data;
    RK_U32          pkt_done = 0;
    // RK_U32 pkt_eos = 0;
    MppCtx  ctx = data->ctx;
    MppApi* mpi = data->mpi;
    // MppPacket packet = data->packet;
    // FileReader reader = cmd->reader;
    // FileBufSlot *slot = NULL;
    // RK_U32 quiet = data->quiet;
    // MPP_RET ret = reader_index_read(reader, data->packet_count++, &slot);

    // mpp_assert(ret == MPP_OK);
    // mpp_assert(slot);

    // pkt_eos = slot->eos;

    if (pkt_eos) {
        if (data->frame_num < 0 || data->frame_num > data->frame_count) {
            // mpp_log_q(quiet, "%p loop again\n", ctx);
            // data->packet_count = 0;
            pkt_eos = 0;
        }
        else {
            // mpp_log_q(quiet, "%p found last packet\n", ctx);
            // data->loop_end = 1;
        }
    }
    size_t packet_size = data->packet_size;

    MPP_RET ret;
    if (packet == NULL) {
        ret = mpp_packet_init(&packet, NULL, 0);
    }
    // mpp_packet_set_data(packet, slot->data);
    // mpp_packet_set_size(packet, slot->size);
    // mpp_packet_set_pos(packet, slot->data);
    // mpp_packet_set_length(packet, slot->size);
    mpp_packet_set_data(packet, pkt_data);
    mpp_packet_set_size(packet, pkt_size);
    mpp_packet_set_pos(packet, pkt_data);
    mpp_packet_set_length(packet, pkt_size);
    // setup eos flag
    if (pkt_eos)
        mpp_packet_set_eos(packet);

    do {
        RK_U32 frm_eos = 0;
        RK_S32 times   = 5;
        // send the packet first if packet is not done
        if (!pkt_done) {
            ret = mpi->decode_put_packet(ctx, packet);
            if (MPP_OK == ret) {
                pkt_done = 1;
                // if (!data->first_pkt)
                //     data->first_pkt = mpp_time();
            }
        }

        // then get all available frame and release
        do {
            RK_S32   get_frm = 0;
            MppFrame frame   = NULL;

        try_again:
            ret = mpi->decode_get_frame(ctx, &frame);
            if (MPP_ERR_TIMEOUT == ret) {
                if (times > 0) {
                    times--;
                    usleep(2000);
                    goto try_again;
                }
                mpp_err("decode_get_frame failed too much time\n");
            }
            if (ret) {
                mpp_err("decode_get_frame failed ret %d\n", ret);
                break;
            }

            if (frame) {
                if (mpp_frame_get_info_change(frame)) {
                    RK_U32 width      = mpp_frame_get_width(frame);
                    RK_U32 height     = mpp_frame_get_height(frame);
                    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
                    RK_U32 buf_size   = mpp_frame_get_buf_size(frame);
                    // MppBufferGroup grp = NULL;
                    if (NULL == data->frm_grp) {
                        /* If buffer group is not set create one and limit it */
                        ret = mpp_buffer_group_get_internal(
                            &data->frm_grp, MPP_BUFFER_TYPE_DRM);
                        if (ret) {
                            LOGD("%p get mpp buffer group failed ret %d ", ctx,
                                 ret);
                            break;
                        }

                        /* Set buffer to mpp decoder */
                        ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP,
                                           data->frm_grp);
                        if (ret) {
                            LOGD("%p set buffer group failed ret %d ", ctx,
                                 ret);
                            break;
                        }
                    }
                    else {
                        /* If old buffer group exist clear it */
                        ret = mpp_buffer_group_clear(data->frm_grp);
                        if (ret) {
                            LOGD("%p clear buffer group failed ret %d ", ctx,
                                 ret);
                            break;
                        }
                    }

                    // mpp_log_q(quiet, "decode_get_frame get info changed
                    // found\n"); mpp_log_q(quiet, "decoder require buffer w:h
                    // [%d:%d] stride [%d:%d] buf_size %d",
                    //           width, height, hor_stride, ver_stride,
                    //           buf_size);

                    // grp = dec_buf_mgr_setup(data->buf_mgr, buf_size, 24,
                    // cmd->buf_mode);
                    /* Set buffer to mpp decoder */

                    // ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP,
                    // data->frm_grp); if (ret)
                    // {
                    //     mpp_err("%p set buffer group failed ret %d\n", ctx,
                    //     ret); break;
                    // }
                    // // data->frm_grp = grp;

                    // /*
                    //  * All buffer group config done. Set info change ready to
                    //  let
                    //  * decoder continue decoding
                    //  */
                    // ret = mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY,
                    // NULL); if (ret)
                    // {
                    //     mpp_err("info change ready failed ret %d\n", ret);
                    //     break;
                    // }
                }
                else {
                    RK_U32 err_info = 0;

                    // if (!data->first_frm)
                    //     data->first_frm = mpp_time();

                    err_info = mpp_frame_get_errinfo(frame) |
                               mpp_frame_get_discard(frame);
                    if (err_info) {
                        // mpp_log_q(quiet, "decoder_get_frame get err info:%d
                        // discard:%d.\n",
                        //           mpp_frame_get_errinfo(frame),
                        //           mpp_frame_get_discard(frame));
                    }
                    // mpp_log_q("decode_get_frame get frame %d\n",
                    //           data->frame_count);

                    data->frame_count++;
                    ///////////////////////////////////////////////////*************************************************
                    ///*/
                    printf("V1 get data ok\n");
                    // if (data->fp_output && !err_info)
                    //     dump_mpp_frame_to_file(frame, data->fp_output);

                    // fps_calc_inc(cmd->fps);
                }
                frm_eos = mpp_frame_get_eos(frame);
                mpp_frame_deinit(&frame);
                get_frm = 1;
            }

            // if last packet is send but last frame is not found continue
            if (pkt_eos && pkt_done && !frm_eos) {
                // msleep(1);
                usleep(1000);
                continue;
            }

            if ((data->frame_num > 0 &&
                 (data->frame_count >= data->frame_num)) ||
                ((data->frame_num == 0) && frm_eos))
                break;

            if (get_frm)
                continue;
            break;
        } while (1);

        if ((data->frame_num > 0 && (data->frame_count >= data->frame_num)) ||
            ((data->frame_num == 0) && frm_eos)) {
            // data->loop_end = 1;
            break;
        }

        if (pkt_done)
            break;

        /*
         * why sleep here:
         * mpi->decode_put_packet will failed when packet in internal queue is
         * full,waiting the package is consumed .Usually hardware decode one
         * frame which resolution is 1080p needs 2 ms,so here we sleep 3ms
         * * is enough.
         */
        usleep(1000);
    } while (1);

    return ret;
}

int MppDecoder::Decode(uint8_t* pkt_data,
                       int      pkt_size,
                       uint64_t frame_pts,
                       int      pkt_eos)
{
    MpiDecLoopData* data     = &loop_data;
    RK_U32          pkt_done = 0;
    RK_U32          err_info = 0;
    MPP_RET         ret      = MPP_OK;
    MppCtx          ctx      = data->ctx;
    MppApi*         mpi      = data->mpi;

    size_t read_size   = 0;
    size_t packet_size = data->packet_size;

    if (packet == NULL) {
        ret = mpp_packet_init(&packet, NULL, 0);
    }

    ///////////////////////////////////////////////
    // ret = mpp_packet_init(&packet, frame_data, frame_size);
    mpp_packet_set_data(packet, pkt_data);
    mpp_packet_set_size(packet, pkt_size);
    mpp_packet_set_pos(packet, pkt_data);
    mpp_packet_set_length(packet, pkt_size);
    // setup eos flag
    if (pkt_eos)
        mpp_packet_set_eos(packet);
    do {
        RK_S32 times = 5;
        // send the packet first if packet is not done
        if (!pkt_done) {
            ret = mpi->decode_put_packet(ctx, packet);
            if (MPP_OK == ret)
                pkt_done = 1;
        }
        // then get all available frame and release
        do {
            RK_S32 get_frm = 0;
            RK_U32 frm_eos = 0;

        try_again:
            ret = mpi->decode_get_frame(ctx, &frame);
            if (MPP_ERR_TIMEOUT == ret) {
                if (times > 0) {
                    times--;
                    usleep(2000);
                    goto try_again;
                }
                LOGD("decode_get_frame failed too much time ");
            }

            if (MPP_OK != ret) {
                LOGD("decode_get_frame failed ret %d ", ret);
                break;
            }
            printf("1111111111111111111111111111111\n");
            if (frame) {
                printf("222222222222222222222222222222\n");
                RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
                RK_U32 hor_width  = mpp_frame_get_width(frame);
                RK_U32 ver_height = mpp_frame_get_height(frame);
                RK_U32 buf_size   = mpp_frame_get_buf_size(frame);
                RK_S64 pts        = mpp_frame_get_pts(frame);
                RK_S64 dts        = mpp_frame_get_dts(frame);

                if (mpp_frame_get_info_change(frame)) {
                    if (NULL == data->frm_grp) {
                        /* If buffer group is not set create one and limit it */
                        ret = mpp_buffer_group_get_internal(
                            &data->frm_grp, MPP_BUFFER_TYPE_DRM);
                        if (ret) {
                            LOGD("%p get mpp buffer group failed ret %d ", ctx,
                                 ret);
                            break;
                        }

                        /* Set buffer to mpp decoder */
                        ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP,
                                           data->frm_grp);
                        if (ret) {
                            LOGD("%p set buffer group failed ret %d ", ctx,
                                 ret);
                            break;
                        }
                    }
                    else {
                        /* If old buffer group exist clear it */
                        ret = mpp_buffer_group_clear(data->frm_grp);
                        if (ret) {
                            LOGD("%p clear buffer group failed ret %d ", ctx,
                                 ret);
                            break;
                        }
                    }

                    /* Use limit config to limit buffer count to 24 with
                     * buf_size */
                    ret = mpp_buffer_group_limit_config(data->frm_grp, buf_size,
                                                        24);
                    if (ret) {
                        LOGD("%p limit buffer group failed ret %d ", ctx, ret);
                        break;
                    }

                    /*
                     * All buffer group config done. Set info change ready to
                     * let decoder continue decoding
                     */
                    ret =
                        mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (ret) {
                        LOGD("%p info change ready failed ret %d ", ctx, ret);
                        break;
                    }

                    this->last_frame_time_ms = GetCurrentTimeMS();
                }
                else {
                    err_info = mpp_frame_get_errinfo(frame) |
                               mpp_frame_get_discard(frame);
                    if (err_info) {
                        LOGD("decoder_get_frame get err info:%d discard:%d. ",
                             mpp_frame_get_errinfo(frame),
                             mpp_frame_get_discard(frame));
                    }
                    data->frame_count++;
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    // LOGD("get one frame %ld ",  (tv.tv_sec * 1000 +
                    // tv.tv_usec/1000)); mpp_frame_get_width(frame); char
                    // *input_data =(char *)
                    // mpp_buffer_get_ptr(mpp_frame_get_buffer(frame));
                    printf("V0 get data ok\n");
                    // if (callback != nullptr) {
                    //     MppFrameFormat format = mpp_frame_get_fmt(frame);
                    //     char *data_vir =(char *)
                    //     mpp_buffer_get_ptr(mpp_frame_get_buffer(frame)); int
                    //     fd = mpp_buffer_get_fd(mpp_frame_get_buffer(frame));
                    //     callback(this->userdata, hor_stride, ver_stride,
                    //     hor_width, ver_height, format, fd, data_vir,
                    //     frame_pts);

                    // }
                    unsigned long cur_time_ms = GetCurrentTimeMS();
                    long          time_gap    = 1000 / this->fps -
                                    (cur_time_ms - this->last_frame_time_ms);
                    // LOGD("time_gap=%ld", time_gap);
                    if (time_gap > 0) {
                        usleep(time_gap * 1000);
                    }
                    this->last_frame_time_ms = GetCurrentTimeMS();
                }
                frm_eos = mpp_frame_get_eos(frame);

                ret   = mpp_frame_deinit(&frame);
                frame = NULL;

                // if(frame_pre!=NULL)
                // {
                //     mpp_frame_deinit(&frame_pre);
                // }
                // &frame_pre=&frame;

                get_frm = 1;
            }
            else {
                printf("frame is null\n");
            }

            // try get runtime frame memory usage
            if (data->frm_grp) {
                size_t usage = mpp_buffer_group_usage(data->frm_grp);
                if (usage > data->max_usage)
                    data->max_usage = usage;
            }

            // if last packet is send but last frame is not found continue
            if (pkt_eos && pkt_done && !frm_eos) {
                usleep(1 * 1000);
                continue;
            }

            if (frm_eos) {
                LOGD("found last frame ");
                break;
            }

            if (data->frame_num > 0 && data->frame_count >= data->frame_num) {
                data->eos = 1;
                break;
            }

            if (get_frm)
                continue;
            break;
        } while (1);

        if (data->frame_num > 0 && data->frame_count >= data->frame_num) {
            data->eos = 1;
            LOGD("reach max frame number %d ", data->frame_count);
            break;
        }

        if (pkt_done)
            break;

        /*
         * why sleep here:
         * mpi->decode_put_packet will failed when packet in internal queue is
         * full,waiting the package is consumed .Usually hardware decode one
         * frame which resolution is 1080p needs 2 ms,so here we sleep 3ms
         * * is enough.
         */
        usleep(1000);
    } while (1);
    mpp_packet_deinit(&packet);

    return ret;
}

int MppDecoder::SetCallback(MppDecoderFrameCallback callback)
{
    this->callback = callback;
    return 0;
}