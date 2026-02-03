#include "opencv2/opencv.hpp"
#include "xtkj_decoder.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <signal.h>
#include <thread>
#include <vector>
using namespace std;
vector<int> tmp_data(8);

// 全局变量用于信号处理
static std::atomic<bool>             g_running(true);
static std::vector<cv::VideoWriter*> g_video_writers;
static std::mutex                    g_writer_mutex;
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

// 信号处理函数
void signal_handler(int signum)
{
    printf("\n收到中断信号 (Ctrl+C)，正在安全关闭...\n");
    g_running.store(false);

    // 立即释放所有VideoWriter以确保视频文件完整
    std::lock_guard<std::mutex> lock(g_writer_mutex);
    for (auto* writer : g_video_writers) {
        if (writer && writer->isOpened()) {
            writer->release();
            printf("VideoWriter已安全关闭\n");
        }
    }
}

void run(xtkj::IDecoder* decoder,
         int             instance_id,
         bool            save_video  = false,
         bool            save_images = false,
         int             max_frames  = 0)
{
    int  count       = 0;
    bool keep_flag   = true;
    int  empty_count = 0;

    cv::VideoWriter video_writer;
    bool            writer_initialized = false;

    // 注册到全局列表以便信号处理
    if (save_video) {
        std::lock_guard<std::mutex> lock(g_writer_mutex);
        g_video_writers.push_back(&video_writer);
    }

    while (keep_flag && g_running.load()) {
        int state_decoder = decoder->get_status();
        // printf("Instance %d - 解码器状态: %d\n", instance_id, state_decoder);
        // auto start = std::chrono::high_resolution_clock::now();

        // vector<long long> mat_info = decoder->get_frame();

        // if (mat_info.empty()) {
        //     empty_count++;
        //     // if (empty_count > 100) {
        //     //     printf("Instance %d - 连续空帧超过100次，停止处理\n",
        //     //            instance_id);
        //     //     break;
        //     // }
        //     continue;
        // }

        // empty_count = 0;
        // count++;  // 只在获取到有效帧时才计数

        // cv::Mat mat =
        //     cv::Mat(mat_info[2], mat_info[1], CV_8UC3,
        //     (cv::Mat*)mat_info[0]);

        // // 初始化VideoWriter（首次获取到帧时）
        // if (save_video && !writer_initialized && !mat.empty()) {
        //     std::string output_filename = "out_frames/output_instance_" +
        //                                   std::to_string(instance_id) +
        //                                   ".mp4";

        //     // 获取视频信息
        //     double fps = decoder->get_fps();
        //     if (fps <= 0)
        //         fps = 25.0;  // 默认25fps

        //     int frame_width  = mat.cols;
        //     int frame_height = mat.rows;

        //     // 使用H.264编码器 - avc1更兼容，支持更广泛的播放器
        //     int fourcc = cv::VideoWriter::fourcc('a', 'v', 'c', '1');

        //     video_writer.open(output_filename, fourcc, fps,
        //                       cv::Size(frame_width, frame_height), true);

        //     if (video_writer.isOpened()) {
        //         writer_initialized = true;
        //         // printf(
        //         //     "Instance %d - 视频写入器已初始化: %s (%.2f fps,
        //         //     %dx%d)\n", instance_id, output_filename.c_str(), fps,
        //         //     frame_width, frame_height);
        //     }
        //     else {
        //         printf("Instance %d - 警告：无法初始化视频写入器\n",
        //                instance_id);
        //     }
        // }

        // // 写入视频帧
        // if (save_video && writer_initialized && !mat.empty()) {
        //     video_writer.write(mat);
        // }

        // // 保存图片（可选）
        // if (save_images) {
        //     cv::imwrite("out_frames/output_instance_" +
        //                     std::to_string(instance_id) + "_frame_" +
        //                     std::to_string(count) + ".jpg",
        //                 mat);
        // }

        // // 释放内存
        // if (mat_info[0] > 0)
        //     free(reinterpret_cast<void*>(mat_info[0]));

        // auto end = std::chrono::high_resolution_clock::now();
        // auto duration =
        //     std::chrono::duration_cast<std::chrono::milliseconds>(end -
        //     start)
        //         .count();

        // if (count % 100 == 0) {
        //     printf("Instance %d - 已处理 %d 帧 (处理时间: %lld ms)\n",
        //            instance_id, count, duration);
        // }

        // // 检查是否达到最大帧数
        // if (max_frames > 0 && count >= max_frames) {
        //     printf("Instance %d - 达到最大帧数 %d，停止处理\n", instance_id,
        //            max_frames);
        //     keep_flag = false;
        // }
    }

    // 释放VideoWriter
    if (writer_initialized && video_writer.isOpened()) {
        video_writer.release();
        printf("Instance %d - 视频已保存，共 %d 帧\n", instance_id, count);
    }

    // 从全局列表中移除
    if (save_video) {
        std::lock_guard<std::mutex> lock(g_writer_mutex);
        auto it = std::find(g_video_writers.begin(), g_video_writers.end(),
                            &video_writer);
        if (it != g_video_writers.end()) {
            g_video_writers.erase(it);
        }
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
        // std::this_thread::sleep_for(std::chrono::milliseconds(500));

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
        int max_frames  = 50;
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

            // if (frame_count % 10 == 0) {
            // printf("Instance %d - Frame %d/%d - PTS: %lld ms (%.2f sec) -
            // FPS: %.2f - Bitrate: %.2f Mbps\n",
            //        instance_id, frame_count, max_frames, pts_ms, pts_ms /
            //        1000.0, fps_val, bitrate_val / 1000000.0);
            // }
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
        // std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));

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
        printf("用法: %s <video_list_file> [test_mode] [max_frames] "
               "[save_video] [save_images]\n",
               argv[0]);
        printf("  test_mode: 1=标准模式(默认), 2=重连测试模式\n");
        printf("  max_frames: 最大处理帧数(0=无限制，默认0)\n");
        printf("  save_video: 是否保存为视频(0=否，1=是，默认1)\n");
        printf("  save_images: 是否保存图片(0=否，1=是，默认0)\n");
        printf("\n示例:\n");
        printf("  %s rtsp_file.txt 1 300 1 0  # "
               "标准模式，处理300帧，保存视频，不保存图片\n",
               argv[0]);
        printf("  %s rtsp_file.txt 1 0 1 1    # "
               "标准模式，无限制，保存视频和图片\n",
               argv[0]);
        return -1;
    }

    // 读取测试模式
    int test_mode = 1;  // 默认标准模式
    if (argc >= 3) {
        test_mode = atoi(argv[2]);
    }

    // 读取最大帧数
    int max_frames = 0;  // 默认无限制
    if (argc >= 4) {
        max_frames = atoi(argv[3]);
    }

    // 读取是否保存视频
    bool save_video = true;  // 默认保存视频
    if (argc >= 5) {
        save_video = (atoi(argv[4]) != 0);
    }

    // 读取是否保存图片
    bool save_images = false;  // 默认不保存图片
    if (argc >= 6) {
        save_images = (atoi(argv[5]) != 0);
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
    if (test_mode == 1) {
        printf("最大帧数: %s\n",
               max_frames > 0 ? std::to_string(max_frames).c_str() : "无限制");
        printf("保存视频: %s\n", save_video ? "是" : "否");
        printf("保存图片: %s\n", save_images ? "是" : "否");
    }
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
            bool auto_reopen = true;
            int  interval    = 1;  // 帧间隔（0=全部帧，1=跳帧）
            int  ret = decoders[i]->start_pull(video_paths[i], 0, interval,
                                               timeout_ms, auto_reopen);
            if (ret != 0) {
                printf("Instance %d - 启动失败\n", i);
            }
            else {
                printf("Instance %d - 启动成功\n", i);
            }
        }
        // 等待一些时间让流初始化
        printf("\n等待视频流初始化...\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        // 显示所有视频源的信息
        printf("\n========== 视频信息 ==========\n");
        for (int i = 0; i < thread_num; i++) {
            double  fps          = decoders[i]->get_fps();
            int64_t bitrate      = decoders[i]->get_bitrate();
            int64_t total_frames = decoders[i]->get_total_frames();
            int     frame_width  = decoders[i]->get_frame_width();
            int     frame_height = decoders[i]->get_frame_height();
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
            printf("宽高%d %d\n", frame_width, frame_height);
        }
        printf("\n=================================\n\n");

        // 注册信号处理
        signal(SIGINT, signal_handler);
#ifdef SIGTERM
        signal(SIGTERM, signal_handler);
#endif

        // 创建线程处理帧
        std::vector<std::thread> threads;
        threads.resize(thread_num);
        printf("\n开始处理视频帧...\n");
        printf("提示: 按 Ctrl+C 可以安全中断并保存视频\n");
        if (save_video)
            printf("视频将保存到: out_frames/output_instance_X.mp4\n");
        if (save_images)
            printf("图片将保存到: out_frames/output_instance_X_frame_Y.jpg\n");
        printf("\n");

        for (int i = 0; i < thread_num; i++) {
            threads[i] = thread(run, decoders[i], i, save_video, save_images,
                                max_frames);
        }

        // 等待所有线程完成
        for (int i = 0; i < thread_num; i++) {
            threads[i].join();
        }

        printf("\n所有视频处理完成！\n");
    }

    // 清理资源
    printf("\n清理资源...\n");
    for (int i = 0; i < thread_num; i++) {
        xtkj::releaseDecoder(decoders[i]);
    }
    printf("完成\n");

    return 0;
}
