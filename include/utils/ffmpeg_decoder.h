#ifndef __FFMPEG_DECODER_H__
#define __FFMPEG_DECODER_H__
#include <cstdint>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <libavutil/opt.h>
}
typedef enum DecPixelFormat_ {
    DEC_FMT_NONE = -1,
    DEC_FMT_YUV420P,  ///< planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per
    ///< 2x2 Y samples)
    DEC_FMT_NV12,  ///< planar YUV 4:2:0, 12bpp, 1 plane for Y and 1 plane
    ///< for the UV components, which are interleaved (first
    ///< byte U and the following byte V)
    DEC_FMT_NV21,  ///< as above, but U and V bytes are swapped

} DecPixelFormat;

class VideoDecoder {
  public:
    VideoDecoder(int32_t codec_id);  // 0 : h264, 1 : hevc
    ~VideoDecoder();

    uint8_t* decode(const uint8_t* src,
                    uint32_t       len,
                    int32_t&       pix_w,
                    int32_t&       pix_h,
                    int32_t&       format,
                    size_t&        data_size);

  private:
    int32_t AVPixelFormat2Format(int32_t av_fmt);

  private:
    AVCodecContext* dec_context = nullptr;
};
#endif