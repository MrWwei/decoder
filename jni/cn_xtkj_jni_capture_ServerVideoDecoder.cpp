#include "cn_xtkj_jni_capture_ServerVideoDecoder.h"
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
int                                      instanceNum = 1;

/**
 * 初始化资源，创建解码器实例
 * @param timeOutSeconds 超时时间（秒）
 * @return 0 成功，-1 失败
 */
JNIEXPORT jint JNICALL
Java_cn_xtkj_jni_capture_ServerVideoDecoder_initResource(JNIEnv* env,
                                                         jobject obj,
                                                         jint    timeOutSeconds)
{
    std::lock_guard<std::mutex> lock(mutexs_handle);
    xtkj::IDecoder*             decoder;
    decoder    = xtkj::createDecoder();
    int handle = -1;
    for (int i = 1; i <= instanceNum; i++) {
        if (decoders.count(i) < 1) {
            int ret = decoder->init(timeOutSeconds * 1000);
            if (ret < 0)
                return ret;
            decoders.emplace(i, decoder);
            return i;
        }
    }

    instanceNum++;

    int instanceId = instanceNum;
    int ret        = decoder->init(timeOutSeconds * 1000);
    if (ret < 0) {
        xtkj::releaseDecoder(decoder);
        return ret;
    }
    decoders.emplace(instanceId, decoder);
    return instanceId;
}

/**
 * 打开视频流
 * @param rtspurl RTSP流地址或本地视频文件路径
 * @param handleid 解码器句柄ID
 * @param is_hardware 是否使用硬件解码（0=软解，1=硬解）
 * @param interval 帧间隔（0=全部帧，1=跳帧）
 * @param autoReopen 是否自动重连（0=不重连，1=重连）
 * @param takeMatTimeOutMilliseconds 获取帧超时时间（毫秒）
 * @return 0 成功，非0 失败
 */
JNIEXPORT jint JNICALL Java_cn_xtkj_jni_capture_ServerVideoDecoder_open(
    JNIEnv* env,
    jobject obj,
    jstring rtspurl,
    jint    handleid,
    jint    is_hardware,
    jint    interval,
    jint    autoReopen,
    jint    takeMatTimeOutMilliseconds)
{
    std::lock_guard<std::mutex> lock(mutexs_handle);
    xtkj::IDecoder*             decoder;
    decoder = decoders[handleid];
    if (decoder == nullptr) {
        return -1;
    }

    const char* rtspurl_cs = env->GetStringUTFChars(rtspurl, NULL);
    int ret = decoder->start_pull(rtspurl_cs, 0, 0, takeMatTimeOutMilliseconds);
    env->ReleaseStringUTFChars(rtspurl, rtspurl_cs);
    return ret;
}

/**
 * 获取解码器状态
 * @param handleid 解码器句柄ID
 * @return 状态值：0=空闲，1=打开中，2=已打开，3=失败
 */
JNIEXPORT jint JNICALL
Java_cn_xtkj_jni_capture_ServerVideoDecoder_takeState(JNIEnv* env,
                                                      jobject obj,
                                                      jint    handleid)
{
    std::lock_guard<std::mutex> lock(mutexs_handle);

    try {
        auto it = decoders.find(handleid);
        if (it == decoders.end()) {
            return -1;
        }

        xtkj::IDecoder* decoder = it->second;
        return decoder->get_status();
    }
    catch (const exception& e) {
        cout << "获取状态异常: " << e.what() << endl;
        return -1;
    }
}

/**
 * 获取视频信息（FPS、码率、总帧数）
 * @param handleid 解码器句柄ID
 * @return VideoInfo对象，包含视频信息
 */
JNIEXPORT jobject JNICALL
Java_cn_xtkj_jni_capture_ServerVideoDecoder_takeInfo(JNIEnv* env,
                                                     jobject obj,
                                                     jint    handleid)
{
    std::lock_guard<std::mutex> lock(mutexs_handle);

    try {
        auto it = decoders.find(handleid);
        if (it == decoders.end()) {
            cout << "解码器 " << handleid << " 不存在" << endl;
            return nullptr;
        }

        xtkj::IDecoder* decoder = it->second;

        // 获取视频信息
        double  fps          = decoder->get_fps();
        int64_t bitrate      = decoder->get_bitrate();
        int64_t total_frames = decoder->get_total_frames();
        int     frame_width  = decoder->get_frame_width();
        int     frame_height = decoder->get_frame_height();

        // 查找VideoInfo类
        jclass videoInfoClass =
            env->FindClass("cn/xtkj/jni/capture/data/VideoInfo");
        if (videoInfoClass == nullptr) {
            cout << "找不到VideoInfo类" << endl;
            return nullptr;
        }

        // 获取构造函数
        jmethodID constructor =
            env->GetMethodID(videoInfoClass, "<init>", "()V");
        if (constructor == nullptr) {
            cout << "找不到VideoInfo构造函数" << endl;
            return nullptr;
        }

        // 创建VideoInfo对象
        jobject videoInfo = env->NewObject(videoInfoClass, constructor);
        if (videoInfo == nullptr) {
            cout << "创建VideoInfo对象失败" << endl;
            return nullptr;
        }

        // 设置frameRate字段 (int类型)
        jfieldID frameRateField =
            env->GetFieldID(videoInfoClass, "frameRate", "I");
        if (frameRateField != nullptr) {
            env->SetIntField(videoInfo, frameRateField, static_cast<jint>(fps));
        }

        // 设置bitrate字段 (int类型)
        jfieldID bitrateField = env->GetFieldID(videoInfoClass, "bitrate", "I");
        if (bitrateField != nullptr) {
            env->SetIntField(videoInfo, bitrateField,
                             static_cast<jint>(bitrate));
        }

        // 设置frameLen字段 (int类型)
        jfieldID frameLenField =
            env->GetFieldID(videoInfoClass, "frameLen", "I");
        if (frameLenField != nullptr) {
            env->SetIntField(videoInfo, frameLenField,
                             static_cast<jint>(total_frames));
        }

        // 设置frameWidth字段 (int类型)
        jfieldID frameWidthField =
            env->GetFieldID(videoInfoClass, "frameWidth", "I");
        if (frameWidthField != nullptr) {
            env->SetIntField(videoInfo, frameWidthField,
                             static_cast<jint>(frame_width));
        }

        // 设置frameHeight字段 (int类型)
        jfieldID frameHeightField =
            env->GetFieldID(videoInfoClass, "frameHeight", "I");
        if (frameHeightField != nullptr) {
            env->SetIntField(videoInfo, frameHeightField,
                             static_cast<jint>(frame_height));
        }

        return videoInfo;
    }
    catch (const exception& e) {
        cout << "获取视频信息异常: " << e.what() << endl;
        return nullptr;
    }
}

/**
 * 解码获取一帧图像数据（返回Mat引用信息）
 * @param handleid 解码器句柄ID
 * @return long数组 [mat数据地址, 宽度, 高度]
 *         返回null表示获取失败或超时
 */
JNIEXPORT jlongArray JNICALL
Java_cn_xtkj_jni_capture_ServerVideoDecoder_decodeToMatRef(JNIEnv* env,
                                                           jobject obj,
                                                           jint    handleid)
{
    // 不使用全局锁，避免阻塞其他解码器
    try {
        xtkj::IDecoder* decoder = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutexs_handle);
            auto                        it = decoders.find(handleid);
            if (it == decoders.end()) {
                return nullptr;
            }
            decoder = it->second;
        }
        vector<long long> mat_info = decoder->get_frame();
        if (mat_info.empty()) {
            // 超时或错误
            // std::cout << "jni解码器 " << handleid << "
            // 获取帧数据超时或错误。"
            //           << std::endl;
            return nullptr;
        }
        cv::Mat image =
            cv::Mat(mat_info[2], mat_info[1], CV_8UC3, (cv::Mat*)mat_info[0]);
        cv::Mat* matCopy = new cv::Mat(image.rows, image.cols, image.type());
        image.copyTo(*matCopy);
        image.release();
        free(reinterpret_cast<void*>(mat_info[0]));
        // 创建JNI jlongArray
        int        arr_len    = 1;
        jlongArray jLongArray = env->NewLongArray(arr_len);
        long*      longArray  = new long[1];
        longArray[0]          = reinterpret_cast<jlong>(matCopy);
        // longArray[1]          = mat_info[3];
        // 将C++中的long数组的值拷贝到JNI jlongArray
        env->SetLongArrayRegion(jLongArray, 0, arr_len,
                                reinterpret_cast<const jlong*>(longArray));
        delete[] longArray;
        longArray = nullptr;
        // return reinterpret_cast<jlong>(matCopy);
        return jLongArray;
    }
    catch (const exception& e) {
        cout << "解码异常: " << e.what() << endl;
        return nullptr;
    }
}

/**
 * 释放解码器资源
 * @param handleid 解码器句柄ID
 * @return 0 成功，-1 失败
 */
JNIEXPORT jint JNICALL
Java_cn_xtkj_jni_capture_ServerVideoDecoder_release(JNIEnv* env,
                                                    jobject obj,
                                                    jint    handleid)
{
    std::lock_guard<std::mutex> lock(mutexs_handle);

    try {
        auto it = decoders.find(handleid);
        if (it == decoders.end()) {
            cout << "解码器 " << handleid << " 不存在" << endl;
            return -1;
        }

        xtkj::IDecoder* decoder = it->second;

        // 停止解码器
        decoder->stop();

        // 释放解码器
        xtkj::releaseDecoder(decoder);

        // 从map中移除
        decoders.erase(it);

        cout << "解码器 " << handleid << " 已释放" << endl;
        return 0;
    }
    catch (const exception& e) {
        cout << "释放解码器异常: " << e.what() << endl;
        return -1;
    }
}
