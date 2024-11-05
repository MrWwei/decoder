#include "pullFramer.h"
#include "opencv2/opencv.hpp"
#include "stdio.h"
#include "string.h"
#include <iomanip>
#include <iostream>
using namespace std;
/*
将zlmediakit拉去的mk_frame转换
*/

static std::string dump(const void* buf, size_t size)
{
    int         len    = 0;
    char*       data   = (char*)buf;
    std::string result = "NULL";
    if (data == NULL || size <= 0)
        return result;
    size_t total = size * 3 + 1;
    char*  buff  = new char[size * 3 + 1];
    memset(buff, 0, size * 3 + 1);
    for (size_t i = 0; i < size; i++) {
        len += snprintf(buff + len, total - len, "%.2x ", (data[i] & 0xff));
    }
    result = std::string(buff);
    delete[] buff;
    buff = NULL;
    return result;
}

FrameData::FrameData(const mk_frame frame)
    : data_(nullptr), size_(0), dts_(0), pts_(0), prefixSize_(0),
      keyFrame_(false), configFrame_(false), dropAble_(true), decodeAble_(false)
{
    if (frame == NULL) {
        std::cerr << "frame is NULL" << std::endl;
        return;
    }
    auto data = mk_frame_get_data(frame);
    auto size = mk_frame_get_data_size(frame);
    if (data == NULL || size == 0) {
        std::cerr << "data is NULL or size is 0" << std::endl;
        return;
    }
    data_ = new uint8_t[size + 1];
    if (data_ == nullptr) {
        std::cout << "new failed" << std::endl;
        return;
    }
    size_ = size;
    memcpy(data_, data, size_);
    data_[size_] = 0;
    dts_         = mk_frame_get_dts(frame);
    pts_         = mk_frame_get_pts(frame);
    prefixSize_  = mk_frame_get_data_prefix_size(frame);
    auto flag    = mk_frame_get_flags(frame);
    keyFrame_    = (flag & MK_FRAME_FLAG_IS_KEY);
    configFrame_ = (flag & MK_FRAME_FLAG_IS_CONFIG);
    dropAble_    = (flag & MK_FRAME_FLAG_DROP_ABLE);
    decodeAble_  = !(flag & MK_FRAME_FLAG_NOT_DECODE_ABLE);
}

FrameData::FrameData(uint8_t* data, size_t size, const mk_frame frame)
    : data_(nullptr), size_(0), dts_(0), pts_(0), prefixSize_(0),
      keyFrame_(false), configFrame_(false), dropAble_(true), decodeAble_(false)
{
    if (frame == NULL) {
        std::cerr << "frame is NULL" << std::endl;
        return;
    }

    if (data == NULL || size == 0) {
        std::cerr << "data is NULL or size is 0" << std::endl;
        return;
    }
    data_ = new uint8_t[size + 1];
    if (data_ == nullptr) {
        std::cout << "new failed" << std::endl;
        return;
    }
    size_ = size;
    memcpy(data_, data, size_);
    data_[size_] = 0;
    dts_         = mk_frame_get_dts(frame);
    pts_         = mk_frame_get_pts(frame);
    prefixSize_  = mk_frame_get_data_prefix_size(frame);
    auto flag    = mk_frame_get_flags(frame);
    keyFrame_    = (flag & MK_FRAME_FLAG_IS_KEY);
    configFrame_ = (flag & MK_FRAME_FLAG_IS_CONFIG);
    dropAble_    = (flag & MK_FRAME_FLAG_DROP_ABLE);
    decodeAble_  = !(flag & MK_FRAME_FLAG_NOT_DECODE_ABLE);
}

FrameData::~FrameData()
{
    if (data_ != nullptr) {
        delete[] data_;
        data_ = nullptr;
    }
    size_ = 0;
}

std::ostream& operator<<(std::ostream& os, const FrameData::Ptr& Frame)
{
    size_t len = 10;
    if (Frame.get() == nullptr || Frame->data() == nullptr ||
        Frame->size() == 0) {
        os << "NULL";
        return os;
    }
    if (Frame->size() < 10) {
        len = Frame->size();
    }
    os << "[" << Frame->pts() << ", drop:" << Frame->dropAble()
       << ", key:" << Frame->keyFrame() << ", " << std::setw(6) << std::right
       << Frame->size() << "] : " << dump(Frame->data(), len);

    return os;
}

PullFramer::PullFramer()
{
    configFrames     = NULL;
    configFramesSize = 0;
    cb_              = NULL;
    userdata_        = NULL;
    // decoder_         = NULL;
    // displayer_ = NULL;
}

PullFramer::~PullFramer()
{
    clearConfigFrames();
}

void PullFramer::setOnGetFrame(const onGetFrame& onGetFrame,
                               void*             userdata,
                               int               instance_id)
{
    cb_ = onGetFrame;  // 设置函数对象
    // decoder_ = decoder;     // 设置解码器
    userdata_ = userdata;
}

void PullFramer::clearConfigFrames()
{
    if (configFrames != NULL) {
        free(configFrames);
        configFrames = nullptr;
    }
    configFramesSize = 0;
}

bool PullFramer::onFrame(const mk_frame frame_, int instance_id)
{
    if (frame_ == NULL) {
        return false;
    }
    auto frame = FrameData::CreateShared(frame_);
    if (frame.get() == nullptr || frame->data() == nullptr ||
        frame->size() == 0) {
        return false;
    }
    auto data = frame->data();
    auto size = frame->size();
    if (frame->configFrame()) {
        size_t         newSize = configFramesSize + size;
        unsigned char* newConfigFrames =
            (unsigned char*)realloc(configFrames, newSize);
        if (newConfigFrames == NULL) {
            std::cout << "realloc failed" << std::endl;
            clearConfigFrames();
            return false;
        }
        configFrames = newConfigFrames;
        memcpy(configFrames + configFramesSize, data, size);
        configFramesSize = newSize;
    }
    else {
        if (configFrames != NULL && configFramesSize != 0) {
            if (frame->dropAble()) {
                size_t         newSize = configFramesSize + size;
                unsigned char* newConfigFrames =
                    (unsigned char*)realloc(configFrames, newSize);
                if (newConfigFrames == NULL) {
                    std::cout << "realloc failed" << std::endl;
                    clearConfigFrames();
                    return false;
                }
                configFrames = newConfigFrames;
                memcpy(configFrames + configFramesSize, data, size);
                configFramesSize = newSize;
                return true;
            }
            size_t         totalSize  = configFramesSize + size;
            unsigned char* mergedData = (unsigned char*)malloc(totalSize);
            if (mergedData == NULL) {
                std::cout << "malloc failed" << std::endl;
                clearConfigFrames();
                return false;
            }
            memcpy(mergedData, configFrames, configFramesSize);
            memcpy(mergedData + configFramesSize, data, size);
            clearConfigFrames();
            if (cb_) {
                cb_(FrameData::CreateShared(mergedData, totalSize, frame_),
                    userdata_, instance_id);
            }
            else {
                printf("on Frame %zu [%.2x %.2x %.2x %.2x %.2x %.2x]\n",
                       totalSize, mergedData[0], mergedData[1], mergedData[2],
                       mergedData[3], (mergedData[4] & 0xff),
                       (mergedData[5] & 0xff));
            }

            free(mergedData);
            mergedData = NULL;
            return true;
        }
        else {
            if (cb_) {
                cb_(FrameData::CreateShared(frame_), userdata_, instance_id);
            }
            else {
                printf("on Frame %zu [%.2x %.2x %.2x %.2x %.2x %.2x]\n", size,
                       data[0], data[1], data[2], data[3], (data[4] & 0xff),
                       (data[5] & 0xff));
            }
            return true;
        }
    }
    return true;
}
