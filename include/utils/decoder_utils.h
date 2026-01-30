#ifndef DECODER_UTILS_H
#define DECODER_UTILS_H

#include <string>
#include <cstdio>

namespace xtkj {

// Forward declaration
class Decoder;

// URL helper
bool is_rtsp_url(const std::string& path);

// Frame stack management
void clear_frame_stack(Decoder* decoder);

// YUV to BGR conversion functions
void* YV12ToBGR24_OpenCV(unsigned char* pYUV, int width, int height);
void* YV12ToBGR24_OpenCV_FFMPEG(unsigned char* pYUV, int width, int height);

// File I/O helpers
unsigned char* load_data(FILE* fp, size_t ofst, size_t sz);
unsigned char* read_file_data(const char* filename, int* model_size);

}  // namespace xtkj

#endif  // DECODER_UTILS_H
