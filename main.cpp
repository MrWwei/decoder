#include "opencv2/opencv.hpp"
#include "xtkj_decoder.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using namespace std;
vector<int> tmp_data(8);
// int main()
// {
//     // void* test = malloc(2);
//     std::cout << tmp_data.size() << std::endl;
//     printf("test\n");
//     // free(test);
//     return 0;
// }
void read_txt_rtsps(std::string filename, std::vector<std::string>& lines)
{
    // std::string filename = "example.txt";

    // 创建输入文件流对象
    std::ifstream file(filename);

    // 检查文件是否成功打开
    if (!file.is_open()) {
        std::cerr << "无法打开文件: " << filename << std::endl;
        return;
    }

    // 创建一个 vector 用于存储文件的每一行
    // std::vector<std::string> lines;
    std::string line;

    // 逐行读取文件
    while (std::getline(file, line)) {
        if (!line.empty())
            lines.push_back(line);
    }

    // 关闭文件
    file.close();
}
void run(xtkj::IDecoder* decoder, string rtsp_url, int instance_id)
{
    int  count       = 0;
    bool keep_flag   = true;
    int  empty_count = 0;
    while (keep_flag) {
        count++;
        // std::this_thread::sleep_for(
        //     std::chrono::milliseconds(1));  // 线程休眠1ms
        // std::cout << "out:" << decoder->get_frame() << std::endl;
        auto start = std::chrono::high_resolution_clock::now();

        vector<long long> mat_info = decoder->get_frame();
        // std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        if (mat_info.empty()) {
            empty_count++;
            // printf("instance %d out is null; count is %d!\n", instance_id,
            // empty_count);
            // if(empty_count>20){
            //     printf("%d restart open stream\n", instance_id);
            //     decoder->stop();
            //     // break;
            //     this_thread::sleep_for(std::chrono::milliseconds(1000));
            //     decoder->start(rtsp_url);
            //     empty_count = 0;
            //     // continue;
            // }
            continue;
        }

        cv::Mat mat =
            cv::Mat(mat_info[2], mat_info[1], CV_8UC3, (cv::Mat*)mat_info[0]);
        // cv::cvtColor(mat, mat, cv::COLOR_BGRA2BGR);

        // printf("pts:%lld\n",mat_info[3]);
        std::cout << instance_id << ":" << count << std::endl;
        // cv::imwrite("out_frames/"+std::to_string(instance_id)+"_out_" +
        // std::to_string(count) + ".jpg", mat);
        // if(mat_info[0]>0)
        // free(reinterpret_cast<void*>(mat_info[0]));
        auto end = std::chrono::high_resolution_clock::now();
        // 计算时间差
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                .count();
        printf("代码运行时间: %d 毫秒\n", duration);
        // if(count > 10) decoder->stop();

        // cv::imwrite("out_frames/out_" + std::to_string(count) + ".jpg",
        // mat);

        // if (count > 2) {
        //     printf("stop---------------------!\n");
        //     // decoder->stop();
        //     printf("11111111111111111\n");
        //     break;
        // }
    }
}
int main(int argc, char* argv[])
{
    printf("decoder sdk test!\n");
    vector<string> video_paths;
    // = {
    // "rtsp://admin:xtkj12345@192.168.1.87:554/h264/ch33/main/av_stream",
    // "rtsp://admin:admin123@192.168.1.65:554/h264/ch33/main/av_stream",
    // "rtsp://admin:xtkj12345@192.168.1.86:554/h264/ch33/main/av_stream",
    //             // "rtsp://192.168.1.11/test.mp4"
    //             // "rtsp://192.168.1.137/video/S2N-1920x1080-2M.mp4"
    //             "rtsp://192.168.1.11/test.mp4"
    //                         };
    string rtsp_file = argv[1];
    read_txt_rtsps(rtsp_file, video_paths);
    int thread_num = video_paths.size();
    for (auto video_path : video_paths)
        std::cout << "video path:" << video_path << std::endl;
    std::cout << "thread num:" << thread_num << std::endl;
    vector<xtkj::IDecoder*> decoders(thread_num);
    // xtkj::IDecoder* decoder1    = xtkj::createDecoder();
    // xtkj::IDecoder* decoder2   = xtkj::createDecoder();
    // int             thread_num = 4;
    // int             timeout_ms = 200;
    int            timeout_ms = 200;
    vector<string> enc_type{"H264", "H265", "H264", "H264"};
    for (int i = 0; i < thread_num; i++) {
        decoders[i] = xtkj::createDecoder();
        decoders[i]->init(i, 1, timeout_ms);
        printf("init ok!\n");
        decoders[i]->start_pull(video_paths[i], 0);
        // std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
    // std::this_thread::sleep_for(std::chrono::milliseconds(2000000));
    // for(int i = 0;i<thread_num;i++){
    //     run(decoders[i], i);
    // }

    std::vector<std::thread> threads;
    threads.resize(thread_num);
    for (int i = 0; i < thread_num; i++) {
        threads[i] = thread(run, decoders[i], video_paths[i], i);
    }
    for (int i = 0; i < thread_num; i++) {
        threads[i].join();
    }

    // decoder1->init(1, 0, timeout_ms);
    // decoder2->init(2, 0, timeout_ms);
    // string video_path =
    //     "/home/user/videos/device195_2022-11-28_15-20_15-40.mp4";
    // string video_path =
    //     "/home/user/videos/device2349_2022-10-31_10-00_10-20.mp4";
    // string video_path =
    //     "/home/linaro/device1025_2022-10-24_09-00_09-20.mp4";
    // string video_path1 =
    // "rtsp://admin:xtkj12345@192.168.1.87:554/h264/ch33/main/av_stream";
    // string video_path2 =
    // "rtsp://192.168.1.137/video/S2N-1920x1080-2M.mp4";
    // string video_path = "rtmp://192.168.1.111:1935/live/livestream";
    // string video_path =
    // "rtsp://admin:123456@192.168.1.123:554/h264/ch33/main/av_stream";
    // string video_path =
    // "rtsp://admin:xtkj12345@192.168.1.82:554/h264/ch33/main/av_stream";
    // string video_path = "rtsp://192.168.1.137/video/1.mp4";

    // bool ret1   = decoder->start(video_path, "H264");
    // bool ret2   = decoder->start(video_path, "H264");
    // std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    // int  count = 0;

    // while (true) {
    //     count++;
    //     // std::this_thread::sleep_for(
    //     //     std::chrono::milliseconds(1));  // 线程休眠1ms
    //     // std::cout << "out:" << decoder->get_frame() << std::endl;
    //     auto start = std::chrono::high_resolution_clock::now();
    //     vector<long long> mat_info = decoder->get_frame();
    //     std::this_thread::sleep_for(std::chrono::milliseconds(20));

    //     if (mat_info.empty()) {
    //         printf("out is null!\n");
    //         continue;
    //     }

    //     cv::Mat mat =
    //         cv::Mat(mat_info[2], mat_info[1], CV_8UC4,
    // (cv::Mat*)mat_info[0]);
    //     cv::cvtColor(mat, mat, cv::COLOR_BGRA2BGR);

    //     printf("pts:%lld\n",mat_info[3]);
    //     std::cout << "get frame " << count << " ok" << std::endl;
    //     // cv::imwrite("out_frames/out_" + std::to_string(count) + ".jpg",
    // mat);
    //     // if(mat_info[0]>0)
    //     free(reinterpret_cast<void*>(mat_info[0]));
    //     // if(count > 10) decoder->stop();

    //     // cv::imwrite("out_frames/out_" + std::to_string(count) + ".jpg",
    // mat);

    //     // if (count > 2) {
    //     //     printf("stop---------------------!\n");
    //     //     // decoder->stop();
    //     //     printf("11111111111111111\n");
    //     //     break;
    //     // }
    // }
    // printf("11111122222222222222\n");
    // xtkj::releaseDecoder(decoder);

    // ret = decoder->start(video_path);
    // while (true) {
    //     count++;
    //     // std::this_thread::sleep_for(
    //     //     std::chrono::milliseconds(1));  // 线程休眠1ms
    //     // std::cout << "out:" << decoder->get_frame() << std::endl;
    //     auto              start    =
    //     std::chrono::high_resolution_clock::now(); vector<long long>
    // mat_info
    //     = decoder->get_frame(); if (mat_info.empty()) {
    //         printf("out is null!\n");
    //         continue;
    //     }
    //     cv::Mat mat =
    //         cv::Mat(mat_info[2], mat_info[1], CV_8UC3,
    //         (cv::Mat*)mat_info[0]);
    //     cv::imwrite("out_frames/out_" + std::to_string(count) + ".jpg",
    // mat);
    //     free(reinterpret_cast<void*>(mat_info[0]));
    //     // 获取当前时间点
    //     auto end = std::chrono::high_resolution_clock::now();
    //     // 计算时间差
    //     auto duration =
    //         std::chrono::duration_cast<std::chrono::milliseconds>(end -
    //         start)
    //             .count();

    //     // 输出时间差
    //     printf("代码运行时间: %d 毫秒\n", duration);
    //     printf("current idx:%d\n", count);
    //     if (count == 20) {
    //         // printf("stop!\n");
    //         decoder->stop();

    //         break;
    //     }
    // }
    return 0;
}
