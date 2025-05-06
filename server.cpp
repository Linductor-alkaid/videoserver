/* 
filename: server.cpp
author: Linductor
data: 2025/05/03
*/
#include <opencv2/opencv.hpp>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <json/json.h>
#include <csignal>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>


// 全局状态管理
std::atomic<bool> is_connected{false};
std::atomic<bool> heartbeat_timeout{false};
std::atomic<bool> exit_program{false};
std::mutex heartbeat_mutex;
int heartbeat_socket = -1;
static int listen_sock = -1;

// 分辨率配置和摄像头列表
const std::vector<std::pair<int, int>> RES_LEVELS = {{1280,720}, {640,360}, {320,180}};
std::atomic<int> current_res_level{0};
std::vector<int> available_cams;

// 全局新增信号处理
void signal_handler(int signum) {
    std::cout << "\n收到终止信号，清理资源..." << std::endl;
    exit_program = true;
    close(listen_sock); 
}

// 状态处理函数
void handle_status(int code) {
    static int last_level = 0;
    int new_level = code == 300 ? 
        std::min(last_level+1, (int)RES_LEVELS.size()-1) : 
        std::max(last_level-1, 0);
    
    if (new_level != last_level) {
        current_res_level = new_level;
        last_level = new_level;
    }
}

// ================== 网络通信模块 ==================
void broadcast_server_presence() {
    struct ifaddrs *ifaddr, *ifa;
    std::vector<std::string> broadcast_addrs;

    // 获取网络接口列表
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return;
    }

    // 第一次遍历：收集所有IPv4广播地址
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        // 只处理IPv4接口
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_broadaddr;
            char buf[INET_ADDRSTRLEN];

            // 检查接口状态：UP且不是回环接口
            if ((ifa->ifa_flags & IFF_UP) && 
                !(ifa->ifa_flags & IFF_LOOPBACK) &&
                sa != nullptr) {
                
                // 获取广播地址
                inet_ntop(AF_INET, &sa->sin_addr, buf, INET_ADDRSTRLEN);
                broadcast_addrs.push_back(buf);
                
                std::cout << "发现活动接口: " << ifa->ifa_name 
                          << " 广播地址: " << buf << std::endl;
            }
        }
    }

    // 创建广播socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("广播socket创建失败");
        freeifaddrs(ifaddr);
        return;
    }

    // 设置广播选项
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        perror("设置广播选项失败");
        close(sock);
        freeifaddrs(ifaddr);
        return;
    }

    // 准备广播消息
    Json::Value discovery_msg;
    discovery_msg["name"] = "Video-Server";
    discovery_msg["heartbeat_port"] = 5001;
    discovery_msg["video_port"] = 5000;
    std::string json_str = Json::FastWriter().write(discovery_msg);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(37020);

    // 定期广播
    while (!exit_program) {
        for (const auto& bcast : broadcast_addrs) {
            addr.sin_addr.s_addr = inet_addr(bcast.c_str());
            
            if (sendto(sock, json_str.c_str(), json_str.size(), 0,
                      (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                std::cerr << "广播发送失败到 " << bcast << ": " 
                          << strerror(errno) << std::endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    // 清理
    close(sock);
    freeifaddrs(ifaddr);
}

// ================== 心跳检测模块 ==================
void heartbeat_listener() {
    std::unique_lock<std::mutex> lock(heartbeat_mutex, std::defer_lock);
    char request[] = "PING";
    char buffer[16];
    time_t last_heartbeat = time(nullptr);

    while (!exit_program && is_connected) {
        lock.lock();
        if (heartbeat_socket == -1) {
            lock.unlock();
            break;
        }

        // 发送心跳请求
        if (send(heartbeat_socket, request, sizeof(request), 0) <= 0) {
            last_heartbeat = 0; // 立即触发超时检测
        } else {
            // 设置接收超时为1秒
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(heartbeat_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // 等待响应
            int n = recv(heartbeat_socket, buffer, sizeof(buffer), 0);
            if (n > 0) {
                last_heartbeat = time(nullptr);
                handle_status(atoi(buffer));
            } else if (time(nullptr) - last_heartbeat > 3) {
                std::cerr << "心跳丢失，连接中断!" << std::endl;
                heartbeat_timeout = true;
                lock.unlock();
                break;
            }
        }
        // 接收响应（增加错误检测）
        int n = recv(heartbeat_socket, buffer, sizeof(buffer), 0);
        if (n > 0) {
            last_heartbeat = time(nullptr);
            handle_status(atoi(buffer));
        } else if (n == 0) {
            std::cerr << "客户端正常关闭连接" << std::endl;
            is_connected = false;
            break;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("心跳接收错误");
                break;
            }
        }

        // 超时检测（缩短至2秒）
        if (time(nullptr) - last_heartbeat > 2) {
            std::cerr << "心跳超时，连接中断!" << std::endl;
            break;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (heartbeat_timeout) {
        close(heartbeat_socket);
        is_connected = false;
        heartbeat_timeout = false;
    }
    if (heartbeat_socket != -1) {
        shutdown(heartbeat_socket, SHUT_RDWR);
        close(heartbeat_socket);
        heartbeat_socket = -1;
    }
}

// ================== 摄像头管理模块 ==================
std::vector<int> get_available_cameras(int max_check = 5) { // 减少检测范围
    std::vector<int> cameras;
    for (int i = 0; i < max_check; ++i) {
        cv::VideoCapture cap(i, cv::CAP_V4L2); // 明确使用V4L2后端
        if (cap.isOpened()) {
            // 验证摄像头是否真正可用
            cv::Mat test_frame;
            if (cap.read(test_frame)) {
                cameras.push_back(i);
                std::cout << "发现有效摄像头: /dev/video" << i << std::endl;
            }
            cap.release();
        }
    }
    return cameras;
}

bool send_camera_list(int socket) {  // 修改返回类型为bool
    Json::Value cam_list;
    cam_list["type"] = "camera_list";
    for (size_t i = 0; i < available_cams.size(); ++i) {
        cam_list["cameras"].append(available_cams[i]);
    }
    std::string json_str = Json::FastWriter().write(cam_list);
    ssize_t sent_bytes = send(socket, json_str.c_str(), json_str.size(), 0);
    
    if (sent_bytes < 0) {
        perror("发送摄像头列表失败");
        return false;
    } else if (sent_bytes != (ssize_t)json_str.size()) {
        std::cerr << "警告: 摄像头列表数据未完整发送" << std::endl;
        return false;
    }
    return true;
}

// ================== 视频传输模块 ==================
void start_video_stream(const std::string& client_ip, int video_port, int camera_index) {
    available_cams = get_available_cameras(); // 新增：重新检测
    if (std::find(available_cams.begin(), available_cams.end(), camera_index) == available_cams.end()) {
        std::cerr << "摄像头" << camera_index << "已不可用" << std::endl;
        return;
    }
    GstElement *pipeline = nullptr;
    gst_init(nullptr, nullptr);
    cv::VideoCapture cap(camera_index);

    if (!cap.isOpened()) {
        std::cerr << "摄像头打开失败，尝试重新初始化..." << std::endl;
        for (int i = 0; i < 3; ++i) { // 重试3次
            cap.open(camera_index);
            if (cap.isOpened()) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!cap.isOpened()) {
            std::cerr << "摄像头初始化最终失败" << std::endl;
            return; // 提前返回避免后续错误
        }
    }

    // 读取初始帧以确定参数
    cv::Mat frame;
    if (!cap.read(frame)) {
        std::cerr << "无法读取初始帧，无法确定块大小" << std::endl;
        cap.release();
        return;
    }

    int width = frame.cols;
    int height = frame.rows;
    double fps = cap.get(cv::CAP_PROP_FPS);
    if (fps <= 0) fps = 30;

    std::string pipeline_str = 
        "appsrc name=source ! "
        "videoconvert ! "
        "video/x-raw,format=I420 ! "
        "x264enc tune=zerolatency speed-preset=ultrafast ! "
        "rtph264pay config-interval=1 pt=96 ! "
        "udpsink host=" + client_ip + " port=" + std::to_string(video_port);
    
    pipeline = gst_parse_launch(pipeline_str.c_str(), nullptr);
    if (!pipeline) {
        std::cerr << "管道创建失败" << std::endl;
        cap.release();
        return;
    }

    GstAppSrc *appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "source"));
    if (!appsrc) {
        std::cerr << "无法获取appsrc元素" << std::endl;
        gst_object_unref(pipeline);
        cap.release();
        return;
    }

    g_object_set(appsrc,
        "stream-type", 0,
        "format", GST_FORMAT_TIME,
        "block", TRUE,
        "blocksize", frame.total() * frame.elemSize(),
        "emit-signals", FALSE,
        nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // 初始化时间跟踪变量
    auto last_frame_time = std::chrono::steady_clock::now();
    int last_res_level = -1;
    uint64_t frame_count = 0;

    while (!exit_program && is_connected) {
        // 检查分辨率变化
        int res_level = current_res_level.load();
        if (res_level != last_res_level) {
            int new_width = RES_LEVELS[res_level].first;
            int new_height = RES_LEVELS[res_level].second;
            
            // 设置摄像头分辨率
            cap.set(cv::CAP_PROP_FRAME_WIDTH, new_width);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, new_height);
            
            // 验证实际分辨率
            cv::Mat test_frame;
            if (cap.read(test_frame)) {
                width = test_frame.cols;
                height = test_frame.rows;
                std::cout << "分辨率调整为: " << width << "x" << height << std::endl;
                
                // 更新GStreamer参数
                g_object_set(appsrc, 
                    "blocksize", test_frame.total() * test_frame.elemSize(),
                    nullptr);
                
                // 更新caps
                GstCaps *new_caps = gst_caps_new_simple("video/x-raw",
                    "format", G_TYPE_STRING, "BGR",
                    "width", G_TYPE_INT, width,
                    "height", G_TYPE_INT, height,
                    "framerate", GST_TYPE_FRACTION, (int)fps, 1,
                    nullptr);
                gst_app_src_set_caps(appsrc, new_caps);
                gst_caps_unref(new_caps);
                
                last_res_level = res_level;
            }
        }

        if (!cap.read(frame)) {
            std::cerr << "摄像头读取失败! 尝试重新初始化..." << std::endl;
            cap.release();
            cap.open(camera_index); // 尝试重新打开摄像头
            if (!cap.isOpened()) {
                std::cerr << "摄像头无法重新打开!" << std::endl;
                break;
            }
            cap.set(cv::CAP_PROP_FRAME_WIDTH, RES_LEVELS[current_res_level].first);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, RES_LEVELS[current_res_level].second);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
    
        // 动态更新块大小和帧计数器
        size_t current_block_size = frame.total() * frame.elemSize();
        g_object_set(appsrc, 
            "blocksize", (int)current_block_size,
            nullptr);

        // 创建缓冲区并填充数据
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, current_block_size, nullptr);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            memcpy(map.data, frame.data, map.size);
            gst_buffer_unmap(buffer, &map);

            // 使用已声明的frame_count
            GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(
                frame_count, GST_SECOND, fps);
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(
                1, GST_SECOND, fps);
            frame_count++;  // 递增帧计数器
        } else {
            gst_buffer_unref(buffer);
            continue;
        }
    
        // 推送缓冲区并检查状态
        GstFlowReturn flow_status;
        g_signal_emit_by_name(appsrc, "push-buffer", buffer, &flow_status);
        gst_buffer_unref(buffer);
    
        if (flow_status != GST_FLOW_OK) {
            std::cerr << "视频推送错误: " << gst_flow_get_name(flow_status) 
                    << " (分辨率: " << width << "x" << height << ")" << std::endl;
            if (flow_status == GST_FLOW_FLUSHING) break;
            
            // 处理资源不足错误
            if (flow_status == GST_FLOW_EOS || flow_status == GST_FLOW_ERROR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    
        // 精确帧率控制
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_time);
        double target_delay_ms = 1000.0 / fps;
        int delay = std::max(1, static_cast<int>(target_delay_ms - elapsed.count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        last_frame_time = now;
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    cap.release();
}

// ================== 主控制逻辑 ==================
int main() {
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 将listen_sock改为全局变量
    

    available_cams = get_available_cameras();
    if (available_cams.empty()) {
        std::cerr << "错误: 未找到可用摄像头!" << std::endl;
        return 1;
    }

    std::thread broadcast_thread(broadcast_server_presence);

    // 创建监听socket（保持长连接）
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket creation failed");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt failed");
        close(listen_sock);
        return 1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5001);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(listen_sock);
        return 1;
    }

    if (listen(listen_sock, 5) < 0) {
        perror("listen failed");
        close(listen_sock);
        return 1;
    }

    try {
        while (!exit_program) {
            std::cout << "等待客户端连接..." << std::endl;
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &client_len);
            if (client_sock < 0) {
                if (exit_program) break; // 如果程序退出则终止循环
                perror("accept error");
                continue;
            }

            available_cams = get_available_cameras();  
            if (available_cams.empty()) {
                std::cerr << "错误: 当前无可用摄像头!" << std::endl;
                close(client_sock);
                continue;
            }

            is_connected = true;
            std::string client_ip = inet_ntoa(client_addr.sin_addr);
            std::cout << "客户端连接来自: " << client_ip << std::endl;

            // 发送摄像头列表
            if (!send_camera_list(client_sock)) {
                close(client_sock);
                continue; // 发送失败，跳过此客户端
            }

            // 接收摄像头选择
            char buffer[256];
            int n = recv(client_sock, buffer, sizeof(buffer), 0);
            if (n > 0) {
                Json::Value selection;
                if (Json::Reader().parse(buffer, buffer + n, selection)) {
                    int cam_index = selection["camera_index"].asInt();
                    
                    // 设置心跳socket并启动线程
                    heartbeat_socket = client_sock;
                    std::thread(heartbeat_listener).detach();
                    start_video_stream(client_ip, 5000, cam_index);
                }
            } else {
                close(client_sock);
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "致命错误: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "未知异常发生" << std::endl;
    }

    // 确保最终清理
    if (listen_sock != -1) {
        shutdown(listen_sock, SHUT_RDWR);
        close(listen_sock);
    }

    // close(listen_sock);
    exit_program = true;
    broadcast_thread.join();
    return 0;
}