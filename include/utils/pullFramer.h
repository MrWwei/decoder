#ifndef PULL_FRAMER_H  // 防止头文件被重复包含
#define PULL_FRAMER_H

#include <iostream>
#include <memory>
#include <functional>
#include "mk_frame.h"
#include "opencv2/opencv.hpp"
class FrameData {
public:
    using Ptr = std::shared_ptr<FrameData>;
    static Ptr CreateShared(const mk_frame frame) {
        return Ptr(new FrameData(frame));
    }
    static Ptr CreateShared(uint8_t* data_, size_t size_, const mk_frame frame) {
        return Ptr(new FrameData(data_, size_, frame));
    }
    ~FrameData();

    uint64_t dts() { return dts_; };
    uint64_t pts() const { return pts_; }
    size_t prefixSize() const { return prefixSize_; }
    bool keyFrame() const { return keyFrame_; }
    bool dropAble() const { return dropAble_; }
    bool configFrame() const { return configFrame_; }
    bool decodeAble() const { return decodeAble_; }
    uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    friend std::ostream& operator<<(std::ostream& os, const FrameData::Ptr& Frame);
private:
    FrameData(const mk_frame frame);
    FrameData(uint8_t *data_, size_t size_, const mk_frame frame);
    FrameData(const FrameData& other) = delete;
private:
    uint64_t dts_;
    uint64_t pts_;
    size_t prefixSize_;
    bool keyFrame_;
    bool dropAble_;
    bool configFrame_;
    bool decodeAble_;
    uint8_t* data_;
    size_t size_;
};

class PullFramer {
public:
    using Ptr = std::shared_ptr<PullFramer>;
    using onGetFrame = std::function<void(const FrameData::Ptr&, void* decoder, int instance_id)>;
    // using onConver = std::function<void(const FFmpegFrame::Ptr &)>;
    static PullFramer::Ptr CreateShared() {
        return std::make_shared<PullFramer>();
    }
    void setOnGetFrame(const onGetFrame& onGetFrame, void* decoder, int instance_id);
    PullFramer();
    ~PullFramer();

    bool onFrame(const mk_frame frame, int instance_id);

private:
    onGetFrame cb_;
    void* decoder_;
    void* displayer_;
    unsigned char* configFrames;
    size_t configFramesSize;
    void clearConfigFrames();
};
#endif