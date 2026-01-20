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
void run(xtkj::IDecoder* decoder, int instance_id)
{
    int  count       = 0;
    bool keep_flag   = true;
    int  empty_count = 0;
    while (keep_flag) {
        int state_decoder = decoder->get_status();
        // std::cout << "Instance " << instance_id
        //           << " - Decoder State: " << state_decoder << std::endl;
        continue;
        count++;
        auto start = std::chrono::high_resolution_clock::now();

        vector<long long> mat_info = decoder->get_frame();

        if (mat_info.empty()) {
            empty_count++;
            continue;
        }

        cv::Mat mat =
            cv::Mat(mat_info[2], mat_info[1], CV_8UC3, (cv::Mat*)mat_info[0]);

        long long pts_ms      = mat_info[3];
        double    fps_val     = mat_info[4] / 100.0;
        long long bitrate_val = mat_info[5];

        // std::cout << "Instance " << instance_id
        //           << " - Frame #" << count
        //           << " - PTS: " << pts_ms << " ms"
        //           << " - FPS: " << fps_val
        //           << " - Bitrate: " << (bitrate_val ) << " Mbps"
        //           << std::endl;
        cv::imwrite("out_frames/output_instance_" +
                        std::to_string(instance_id) + "_frame_" +
                        std::to_string(count) + ".jpg",
                    mat);
        if (mat_info[0] > 0)
            free(reinterpret_cast<void*>(mat_info[0]));

        auto end = std::chrono::high_resolution_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                .count();
        printf("Instance %d - 处理时间: %lld ms\n", instance_id, duration);
    }
}

// 新增：测试重连功能
void test_reconnect(xtkj::IDecoder* decoder, string video_path, int instance_id)
{
    int reconnect_count = 0;
    int max_reconnects  = 3;  // 最大重连次数

    while (reconnect_count < max_reconnects) {
        printf("\n=== Instance %d - 连接测试 #%d ===\n", instance_id,
               reconnect_count + 1);

        // 启动解码器
        printf("Instance %d - 启动解码器...\n", instance_id);
        int ret = decoder->start_pull(video_path, 0);
        // if (ret != 0) {
        //     printf("Instance %d - 启动失败！\n", instance_id);
        //     break;
        // }
        printf("Instance %d - 启动成功！\n", instance_id);

        // 等待第一帧以获取视频信息
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 显示视频信息
        double  fps          = decoder->get_fps();
        int64_t bitrate      = decoder->get_bitrate();
        int64_t total_frames = decoder->get_total_frames();

        printf("\n------ Instance %d 视频信息 ------\n", instance_id);
        if (fps > 0) {
            printf("帧率: %.2f FPS\n", fps);
        }
        else {
            printf("帧率: 未知\n");
        }

        if (bitrate > 0) {
            printf("码率: %.2f Mbps (%lld bps)\n", bitrate / 1000000.0,
                   bitrate);
        }
        else {
            printf("码率: 未知\n");
        }

        if (total_frames >= 0) {
            printf("总帧数: %lld 帧\n", total_frames);
            if (fps > 0) {
                printf("视频时长: %.2f 秒\n", total_frames / fps);
            }
        }
        else {
            printf("总帧数: N/A (RTSP流不支持)\n");
        }
        printf("--------------------------------\n\n");

        // 运行一段时间（处理50帧）
        int frame_count = 0;
        int max_frames  = 500;
        int empty_count = 0;

        while (frame_count < max_frames) {
            vector<long long> mat_info = decoder->get_frame();

            if (mat_info.empty()) {
                empty_count++;
                std::cout << "Instance " << instance_id << " - 获取帧失败 #"
                          << empty_count << std::endl;
                continue;
            }
            std::cout << "Instance " << instance_id << " - Frame #"
                      << (frame_count + 1) << " - PTS: " << mat_info[3] << " ms"
                      << std::endl;
            // exit(0);
            empty_count = 0;
            frame_count++;

            long long pts_ms  = mat_info[3];
            double    fps_val = mat_info[4] / 100.0;  // FPS stored as fps*100
            long long bitrate_val      = mat_info[5];
            long long total_frames_val = mat_info[6];

            if (frame_count % 10 == 0) {
                // printf("Instance %d - Frame %d/%d - PTS: %lld ms (%.2f sec) -
                // FPS: %.2f - Bitrate: %.2f Mbps\n",
                //        instance_id, frame_count, max_frames, pts_ms, pts_ms /
                //        1000.0, fps_val, bitrate_val / 1000000.0);
            }
            // 释放内存
            if (mat_info[0] > 0)
                free(reinterpret_cast<void*>(mat_info[0]));
        }

        printf("Instance %d - 已处理 %d 帧\n", instance_id, frame_count);

        // 停止解码器
        printf("Instance %d - 停止解码器...\n", instance_id);
        decoder->stop();
        printf("Instance %d - 已停止\n", instance_id);

        // 等待一段时间再重连
        int wait_time = 2000;
        printf("Instance %d - 等待 %d ms 后重连...\n", instance_id, wait_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));

        reconnect_count++;
    }

    printf("\n=== Instance %d - 重连测试完成，共重连 %d 次 ===\n\n",
           instance_id, reconnect_count);
}
int main(int argc, char* argv[])
{
    printf("decoder sdk test!\n");

    // 检查参数
    if (argc < 2) {
        printf("用法: %s <video_list_file> [test_mode]\n", argv[0]);
        printf("  test_mode: 1=标准模式(默认), 2=重连测试模式\n");
        return -1;
    }

    // 读取测试模式
    int test_mode = 1;  // 默认标准模式
    if (argc >= 3) {
        test_mode = atoi(argv[2]);
    }

    // 读取视频列表
    vector<string> video_paths;
    string         rtsp_file = argv[1];
    read_txt_rtsps(rtsp_file, video_paths);
    int thread_num = video_paths.size();

    printf("===========================================\n");
    printf("视频源数量: %d\n", thread_num);
    for (int i = 0; i < video_paths.size(); i++) {
        printf("  [%d] %s\n", i, video_paths[i].c_str());
    }
    printf("测试模式: %s\n", test_mode == 2 ? "重连测试" : "标准模式");
    printf("===========================================\n\n");

    // 创建解码器实例
    vector<xtkj::IDecoder*> decoders(thread_num);
    int                     timeout_ms      = 200;
    int                     timeout_open_ms = 10000;

    for (int i = 0; i < thread_num; i++) {
        decoders[i] = xtkj::createDecoder();
        decoders[i]->init(timeout_open_ms);
        printf("Instance %d - 初始化完成\n", i);
    }

    if (test_mode == 2) {
        // 重连测试模式
        printf("\n========== 重连测试模式 ==========\n");
        printf("将测试每个视频源的重连功能\n");
        printf("每个视频源将:\n");
        printf("  1. 启动并处理50帧\n");
        printf("  2. 停止解码器\n");
        printf("  3. 等待2秒\n");
        printf("  4. 重新启动\n");
        printf("  5. 重复3次\n");
        printf("===================================\n\n");

        // 顺序测试每个视频源的重连功能
        for (int i = 0; i < thread_num; i++) {
            printf("\n>>> 测试视频源 %d: %s\n", i, video_paths[i].c_str());
            test_reconnect(decoders[i], video_paths[i], i);
        }

        printf("\n========== 所有重连测试完成 ==========\n");
    }
    else {
        // 标准模式
        printf("\n========== 标准模式 ==========\n");
        int status = 100;
        // 启动所有解码器
        for (int i = 0; i < thread_num; i++) {
            status = decoders[i]->get_status();

            printf("解码器状态： %d\n", status);
            int ret = decoders[i]->start_pull(video_paths[i], 0, 0, timeout_ms);
            if (ret != 0) {
                printf("Instance %d - 启动失败\n", i);
            }
            else {
                printf("Instance %d - 启动成功\n", i);
            }
        }

        // 等待一些时间让流初始化
        printf("\n等待视频流初始化...\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100000));

        // 显示所有视频源的信息
        printf("\n========== 视频信息 ==========\n");
        for (int i = 0; i < thread_num; i++) {
            double  fps          = decoders[i]->get_fps();
            int64_t bitrate      = decoders[i]->get_bitrate();
            int64_t total_frames = decoders[i]->get_total_frames();
            // 解码器状态
            status = decoders[i]->get_status();

            printf("\n--- Instance %d: %s ---\n", i, video_paths[i].c_str());
            printf("解码器状态: %d\n", status);
            if (fps > 0) {
                printf("帧率: %.2f FPS\n", fps);
            }
            else {
                printf("帧率: 未知\n");
            }

            if (bitrate > 0) {
                printf("码率: %.2f Mbps\n", bitrate / 1000000.0);
            }
            else {
                printf("码率: 未知\n");
            }

            if (total_frames >= 0) {
                printf("总帧数: %lld\n", total_frames);
                if (fps > 0) {
                    printf("时长: %.2f秒\n", total_frames / fps);
                }
            }
            else {
                printf("总帧数: N/A (RTSP流)\n");
            }
        }
        printf("\n=================================\n\n");

        // 创建线程处理帧
        std::vector<std::thread> threads;
        threads.resize(thread_num);
        for (int i = 0; i < thread_num; i++) {
            threads[i] = thread(run, decoders[i], i);
        }

        // 等待所有线程完成
        for (int i = 0; i < thread_num; i++) {
            threads[i].join();
        }
    }

    // 清理资源
    printf("\n清理资源...\n");
    for (int i = 0; i < thread_num; i++) {
        xtkj::releaseDecoder(decoders[i]);
    }
    printf("完成\n");

    return 0;
}
