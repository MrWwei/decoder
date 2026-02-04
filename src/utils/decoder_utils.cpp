#include "decoder_utils.h"
#include "decoder_internal.h"
#include <opencv2/opencv.hpp>
#include <iostream>

namespace {

// Helper logging functions (in anonymous namespace to avoid conflicts)
void log_error(const std::string& msg)
{
    std::cerr << "[ERROR] " << msg << std::endl;
}

void log_info(const std::string& msg)
{
    std::cout << "[INFO] " << msg << std::endl;
}

}  // anonymous namespace

namespace xtkj {

// Helper function to check if path is RTSP URL
bool is_rtsp_url(const std::string& path)
{
    return (path.find("rtsp://") == 0 || path.find("rtmp://") == 0);
}

// Helper function to clear double buffer frames
void clear_frame_stack(Decoder* decoder)
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

// YUV to BGR conversion using OpenCV and FFmpeg
void* YV12ToBGR24_OpenCV_FFMPEG(unsigned char* pYUV, int width, int height)
{
    if (width < 1 || height < 1 || pYUV == NULL) {
        return nullptr;
    }

    unsigned char* pBGR24 = (unsigned char*)malloc(width * height * 3);
    if (pBGR24 == NULL) {
        return nullptr;
    }

    cv::Mat dst(height, width, CV_8UC3, pBGR24);
    cv::Mat src(height + height / 2, width, CV_8UC1, pYUV);
    cv::cvtColor(src, dst, cv::COLOR_YUV2BGR_I420);

    return pBGR24;
}

// YUV to BGR conversion using OpenCV
void* YV12ToBGR24_OpenCV(unsigned char* pYUV, int width, int height)
{
    if (width < 1 || height < 1 || pYUV == NULL) {
        return nullptr;
    }

    unsigned char* pBGR24 = (unsigned char*)malloc(width * height * 3);
    if (pBGR24 == NULL) {
        return nullptr;
    }

    cv::Mat dst(height, width, CV_8UC3, pBGR24);
    cv::Mat src(height + height / 2, width, CV_8UC1, pYUV);
    cv::cvtColor(src, dst, cv::COLOR_YUV420sp2RGB);

    return pBGR24;
}

// File data loading
unsigned char* load_data(FILE* fp, size_t ofst, size_t sz)
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

unsigned char* read_file_data(const char* filename, int* model_size)
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

}  // namespace xtkj
