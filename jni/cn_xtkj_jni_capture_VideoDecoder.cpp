/*
 * @Author: wtwei
 * @Date: 2023-08-10 15:46:45
 * @LastEditTime: 2023-08-25 17:40:39
 * @Description:
 *
 */
#include "cn_xtkj_jni_capture_VideoDecoder.h"
#include "xtkj_decoder.h"
#include <algorithm>
#include <future>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;
std::unordered_map<int, xtkj::IDecoder*> decoders;
std::mutex                               mutexs_handle;
std::condition_variable                  conds_handle;
int                                      netNum = 1;
JNIEXPORT jint JNICALL
               Java_cn_xtkj_jni_capture_VideoDecoder_initResource(JNIEnv* env,
                                                                  jobject thisObj,
                                                                  jint    timeOutMilliseconds)
{
    std::unique_lock<std::mutex> lock(mutexs_handle);
    xtkj::IDecoder*              decoder;
    decoder    = xtkj::createDecoder();
    int handle = -1;
    for (int i = 1; i <= netNum; i++) {
        if (decoders.count(i) < 1) {
            int ret = decoder->init(i, 1, timeOutMilliseconds);
            if (ret < 0)
                return ret;
            decoders.emplace(i, decoder);
            return i;
        }
    }

    netNum++;

    int netId = netNum;
    int ret   = decoder->init(netId, 1, timeOutMilliseconds);
    decoders.emplace(netId, decoder);
    return netId;
}
JNIEXPORT jint JNICALL
               Java_cn_xtkj_jni_capture_VideoDecoder_open(JNIEnv* env,
                                                          jobject thisObj,
                                                          jstring video_path,
                                                          jint    handle,
                                                          jstring av_type,
                                                          jint    decode_type)
{
    std::vector<uchar> rlt_data;

    xtkj::IDecoder* decoder = decoders[handle];

    const char* video_path_cs = env->GetStringUTFChars(video_path, NULL);
    int         ret           = decoder->start_pull(video_path_cs, 0, 0);
    env->ReleaseStringUTFChars(video_path, video_path_cs);
    return 1;
}

JNIEXPORT jint JNICALL
               Java_cn_xtkj_jni_capture_VideoDecoder_close(JNIEnv* env,
                                                           jobject thisObj,
                                                           jint    handle)
{
    xtkj::IDecoder* decoder = decoders[handle];
    int             ret     = decoder->stop();
    return ret;
}
JNIEXPORT jlongArray JNICALL
                     Java_cn_xtkj_jni_capture_VideoDecoder_decodeToMatRef(JNIEnv* env,
                                                                          jobject thisObj,
                                                                          jint    handle)
{
    xtkj::IDecoder*   decoder      = decoders[handle];
    int               failed_times = 20;
    vector<long long> matInfo = decoder->get_frame();  //@return mat地址 宽 高
    if (matInfo.size() < 1) {
        return {};
    }

    cv::Mat image =
        cv::Mat(matInfo[2], matInfo[1], CV_8UC3, (cv::Mat*)matInfo[0]);
    // cv::cvtColor(image, image, cv::COLOR_BGRA2BGR);

    cv::Mat* matCopy = new cv::Mat(image.rows, image.cols, image.type());
    image.copyTo(*matCopy);
    image.release();
    free(reinterpret_cast<void*>(matInfo[0]));
    // 创建JNI jlongArray
    int        arr_len    = 2;
    jlongArray jLongArray = env->NewLongArray(arr_len);
    long*      longArray  = new long[2];
    longArray[0]          = reinterpret_cast<jlong>(matCopy);
    longArray[1]          = matInfo[3];  // 时间戳
    // 将C++中的long数组的值拷贝到JNI jlongArray
    env->SetLongArrayRegion(jLongArray, 0, arr_len,
                            reinterpret_cast<const jlong*>(longArray));
    delete[] longArray;
    longArray = nullptr;
    return jLongArray;
}

JNIEXPORT jbyteArray JNICALL
                     Java_cn_xtkj_jni_capture_VideoDecoder_encodeMatToJpeg(JNIEnv*,
                                                                           jobject,
                                                                           jlongArray)
{
    return {};
}
JNIEXPORT void JNICALL
               Java_cn_xtkj_jni_capture_VideoDecoder_releaseMat(JNIEnv*, jobject, jlongArray)
{
}

JNIEXPORT jint JNICALL
               Java_cn_xtkj_jni_capture_VideoDecoder_release(JNIEnv*, jobject, jint handle)
{
    if (decoders.count(handle) > 0) {
        xtkj::IDecoder* decoder = decoders[handle];
        // decoder->stop();
        xtkj::releaseDecoder(decoder);
        decoders.erase(handle);
        return 1;
    }
    return 1;
}