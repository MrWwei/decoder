#include "ffmpeg_decoder.h"
#include "opencv2/opencv.hpp"
#include <iostream>

VideoDecoder::VideoDecoder(int32_t codec_id)
{
    // 根据输入参数选择解码器
    const AVCodec* codec = nullptr;
    if (codec_id == 0) {
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    else if (codec_id == 1) {
        codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    }
    else {
        std::cerr << "unknow codec_id" << std::endl;
    }

    if (!codec) {
        std::cerr << "Codec [" << codec_id << "] not found\n";
        return;
    }

    dec_context = avcodec_alloc_context3(codec);
    if (!dec_context) {
        std::cerr << "Could not allocate video codec context\n";
        return;
    }

    // 打开解码器
    if (avcodec_open2(dec_context, codec, NULL) < 0) {
        std::cerr << "Could not open codec\n";
        return;
    }
    std::cout << "open codec success" << std::endl;
}

VideoDecoder::~VideoDecoder()
{
    std::cout << "VideoDecoder::~VideoDecoder()" << std::endl;
    if (dec_context) {
        avcodec_free_context(&dec_context);
        dec_context = nullptr;
    }
}
cv::Mat avframe2mat(AVFrame* frame)
{
    int     width  = frame->width;
    int     height = frame->height;
    cv::Mat image(height, width, CV_8UC3);
    int     cvLinesizes[1];
    cvLinesizes[0]         = image.step1();
    SwsContext* conversion = sws_getContext(
        width, height, (AVPixelFormat)frame->format, width, height,
        AVPixelFormat::AV_PIX_FMT_BGR24, SWS_FAST_BILINEAR, NULL, NULL, NULL);

    sws_scale(conversion, frame->data, frame->linesize, 0, height, &image.data,
              cvLinesizes);

    //     sws_scale(sws_ctx_, av_frame_->data, av_frame_->linesize, 0,
    //     video_codec_ctx_->height,
    //  av_frame_rgb_->data, av_frame_rgb_->linesize);
    sws_freeContext(conversion);
    return image;
}
uint8_t* VideoDecoder::decode(const uint8_t* src,
                              uint32_t       len,
                              int32_t&       pix_w,
                              int32_t&       pix_h,
                              int32_t&       format,
                              size_t&        data_size)
{
    AVPacket*     pkt        = nullptr;
    AVFrame*      frame      = NULL;
    AVFrame*      tmp_frame  = NULL;
    uint8_t*      buffer     = NULL;
    AVPixelFormat tmp_pixFmt = AV_PIX_FMT_NONE;
    int           size       = 0;
    pix_w                    = 0;
    pix_h                    = 0;
    format                   = DEC_FMT_NONE;
    if (dec_context == nullptr) {
        std::cerr << "dec_context is nullptr\n";
        return NULL;
    }
    if (src == nullptr || len == 0) {
        std::cerr << "src or len is null" << std::endl;
        return nullptr;
    }
    pkt = av_packet_alloc();
    if (pkt == nullptr) {
        std::cerr << "Could not allocate packet\n";
        return NULL;
    }
    pkt->data =
        const_cast<uint8_t*>(src);  // FFmpeg expects non-const data pointer
    pkt->size = len;
    int ret   = avcodec_send_packet(dec_context, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        pkt = nullptr;
        fprintf(stderr, "Error during decoding\n");
        return buffer;
    }
    cv::Mat outMat;

    while (1) {
        if (!(frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(dec_context, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            return 0;
        }
        else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        outMat     = avframe2mat(frame);
        tmp_frame  = frame;
        tmp_pixFmt = static_cast<AVPixelFormat>(tmp_frame->format);
        size       = av_image_get_buffer_size(tmp_pixFmt, tmp_frame->width,
                                        tmp_frame->height, 1);
        buffer     = (uint8_t*)malloc(sizeof(uint8_t) * size);
        data_size  = sizeof(uint8_t) * size;
        if (!buffer) {
            fprintf(stderr, "Can not alloc buffer\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = av_image_copy_to_buffer(
            buffer, size, (const uint8_t* const*)tmp_frame->data,
            (const int*)tmp_frame->linesize, tmp_pixFmt, tmp_frame->width,
            tmp_frame->height, 1);
        if (ret < 0) {
            fprintf(stderr, "Can not copy image to buffer\n");
            goto fail;
        }
        pix_w  = tmp_frame->width;
        pix_h  = tmp_frame->height;
        format = AVPixelFormat2Format(tmp_pixFmt);

    fail:
        av_packet_free(&pkt);
        pkt = nullptr;
        av_frame_free(&frame);
        frame = NULL;
        if (ret < 0 && buffer != NULL) {
            av_free(buffer);
            buffer = NULL;
        }
        return buffer;
    }
    return buffer;
}

int32_t VideoDecoder::AVPixelFormat2Format(int32_t pix_fmt)
{
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: return DEC_FMT_YUV420P;
        case AV_PIX_FMT_NV12: return DEC_FMT_NV12;
        case AV_PIX_FMT_NV21: return DEC_FMT_NV21;
        default: return DEC_FMT_NONE;
    }
    return DEC_FMT_NONE;
}
