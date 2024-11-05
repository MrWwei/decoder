/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class cn_xtkj_jni_capture_VideoDecoder */

#ifndef _Included_cn_xtkj_jni_capture_VideoDecoder
#    define _Included_cn_xtkj_jni_capture_VideoDecoder
#    ifdef __cplusplus
extern "C" {
#    endif
/*
 * Class:     cn_xtkj_jni_capture_VideoDecoder
 * Method:    initResource
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL
Java_cn_xtkj_jni_capture_VideoDecoder_initResource(JNIEnv*, jobject, jint);

/*
 * Class:     cn_xtkj_jni_capture_VideoDecoder
 * Method:    open
 * Signature: (Ljava/lang/String;ILjava/lang/String;I)I
 */
JNIEXPORT jint JNICALL Java_cn_xtkj_jni_capture_VideoDecoder_open(JNIEnv*,
                                                                  jobject,
                                                                  jstring,
                                                                  jint,
                                                                  jstring,
                                                                  jint);

/*
 * Class:     cn_xtkj_jni_capture_VideoDecoder
 * Method:    close
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL Java_cn_xtkj_jni_capture_VideoDecoder_close(JNIEnv*,
                                                                   jobject,
                                                                   jint);

/*
 * Class:     cn_xtkj_jni_capture_VideoDecoder
 * Method:    decodeToMatRef
 * Signature: (I)[J
 */
JNIEXPORT jlongArray JNICALL
Java_cn_xtkj_jni_capture_VideoDecoder_decodeToMatRef(JNIEnv*, jobject, jint);

/*
 * Class:     cn_xtkj_jni_capture_VideoDecoder
 * Method:    release
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL Java_cn_xtkj_jni_capture_VideoDecoder_release(JNIEnv*,
                                                                     jobject,
                                                                     jint);

#    ifdef __cplusplus
}
#    endif
#endif