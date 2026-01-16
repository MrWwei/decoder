#ifndef DATA_TYPE_H  // 防止头文件被重复包含
#define DATA_TYPE_H
#include <cstdint>
typedef struct
{
    int   width;
    int   height;
    int   width_stride;
    int   height_stride;
    int   format;
    void* virt_addr = nullptr;
    // void* packet_data = nullptr;
    int      data_size = 0;
    int      fd{0};
    uint64_t pts;

} image_frame_t;
#endif