/* 
filename: client.cpp
author: Linductor
data: 2025/05/03
*/
#include <gst/gst.h>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <json/json.h>

// 全局配置
const int DISCOVERY_PORT = 37020;
const int HEARTBEAT_PORT = 5001;
const int VIDEO_PORT = 5000;
std::atomic<bool> is_connected{false};
std::atomic<bool> exit_program{false};
std::atomic<int> receiver_status{200}; // 200=正常，300=拥塞
std::vector<Json::Value> servers;
std::mutex servers_mutex;
Json::Value last_server_info;
int last_cam_index = -1;
std::atomic<bool> abnormal_disconnect{false};
int heartbeat_socket = -1;

// ================== 服务发现模块 ==================
void discover_servers() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buffer[1024];
    while (!exit_program) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buffer, sizeof(buffer), 0,
                       (struct sockaddr*)&from, &from_len);

        if (n > 0) {
            Json::Value server_info;
            if (Json::Reader().parse(buffer, buffer + n, server_info)) {
                server_info["ip"] = inet_ntoa(from.sin_addr);
                std::lock_guard<std::mutex> lock(servers_mutex);
                
                bool exists = false;
                for (const auto& s : servers) {
                    if (s["ip"] == server_info["ip"] && 
                        s["heartbeat_port"] == server_info["heartbeat_port"]) {
                        exists = true;
                        break;
                    }
                }
                
                if (!exists) {
                    servers.push_back(server_info);
                    std::cout << "[发现] " << server_info["name"].asString()
                            << " @ " << server_info["ip"].asString() << std::endl;
                }
            }
        }
    }
    close(sock);
}

// ================== 心跳维护模块 ==================
void handle_heartbeat() {
    char buffer[16];
    while (is_connected && !exit_program) {
        int bytes_received = recv(heartbeat_socket, buffer, sizeof(buffer), 0);
        
        // 检测连接断开
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                abnormal_disconnect = false;
                std::cout << "[心跳] 连接正常关闭" << std::endl;
            } else {
                abnormal_disconnect = true;
                perror("[心跳] 接收错误");
            }
            is_connected = false;
            break;
        }
        
        // 正常处理心跳
        std::string status = std::to_string(receiver_status.load());
        if (send(heartbeat_socket, status.c_str(), status.size(), 0) <= 0) {
            perror("[心跳] 发送状态失败");
            is_connected = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // 清理资源
    if (heartbeat_socket != -1) {
        close(heartbeat_socket);
        heartbeat_socket = -1;
    }
}

// ================== 摄像头选择处理 ==================
int select_camera(int sock, int auto_cam_index = -1) {
    char buffer[1024];
    int n = recv(sock, buffer, sizeof(buffer), 0);
    if (n <= 0) return -1;

    Json::Value cam_list;
    if (!Json::Reader().parse(buffer, buffer + n, cam_list)) return -1;

    if (auto_cam_index != -1) {
        // 自动选择之前的摄像头
        for (Json::Value::ArrayIndex i = 0; i < cam_list["cameras"].size(); ++i) {
            if (cam_list["cameras"][i].asInt() == auto_cam_index) {
                Json::Value response;
                response["camera_index"] = auto_cam_index;
                std::string json_str = Json::FastWriter().write(response);
                send(sock, json_str.c_str(), json_str.size(), 0);
                return auto_cam_index;
            }
        }
        return -1; // 摄像头不存在
    }
    
    std::cout << "\n===== 可用摄像头列表 =====" << std::endl;
    for (Json::Value::ArrayIndex i = 0; i < cam_list["cameras"].size(); ++i) {
        std::cout << "[" << i << "] 摄像头索引 " 
                << cam_list["cameras"][i].asInt() << std::endl;
    }

    int selected = -1;
    while (true) {
        std::cout << "请选择摄像头序号 (q退出): ";
        std::string input;
        std::getline(std::cin, input);
        
        if (input == "q") break;
        
        try {
            selected = std::stoi(input);
            if (selected >= 0 && selected < cam_list["cameras"].size()) {
                break;
            }
            std::cerr << "无效序号!" << std::endl;
        } catch (...) {
            std::cerr << "输入错误!" << std::endl;
        }
    }

    if (selected != -1) {
        Json::Value response;
        response["camera_index"] = cam_list["cameras"][selected].asInt();
        std::string json_str = Json::FastWriter().write(response);
        send(sock, json_str.c_str(), json_str.size(), 0);
    }
    return selected;
}

// ================== 视频接收模块 ==================
void start_video_reception(const std::string& server_ip) {
    GstElement *pipeline = nullptr;
    gst_init(nullptr, nullptr);

    std::string pipeline_str = 
        "udpsrc port=" + std::to_string(VIDEO_PORT) + " ! "
        "application/x-rtp,media=video,encoding-name=H264 ! "
        "rtpjitterbuffer latency=100 ! "
        "rtph264depay ! avdec_h264 ! videoconvert ! videoscale ! "
        "video/x-raw,width=640,height=360 ! autovideosink";

    pipeline = gst_parse_launch(pipeline_str.c_str(), nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    while (is_connected && !exit_program) {
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 
            100 * GST_MSECOND, // 将超时设置为100毫秒
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_QOS));
        
        if (msg) {
            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_QOS: {
                    guint64 timestamp;
                    gst_message_parse_qos(msg, nullptr, nullptr, nullptr, &timestamp, nullptr);
                    static guint64 last_timestamp = 0;
                    if (timestamp - last_timestamp > 20000000) {
                        receiver_status.store(300);
                    } else {
                        receiver_status.store(200);
                    }
                    last_timestamp = timestamp;
                    break;
                }
                case GST_MESSAGE_ERROR: {
                    gchar *debug;
                    GError *err;
                    gst_message_parse_error(msg, &err, &debug);
                    std::cerr << "视频错误: " << err->message << std::endl;
                    g_error_free(err);
                    g_free(debug);
                    break;
                }
                case GST_MESSAGE_EOS:
                    std::cout << "视频流结束" << std::endl;
                    break;
                default: break;
            }
            gst_message_unref(msg);
        }
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// ================== 主控制逻辑 ==================
int main() {
    std::thread discovery_thread(discover_servers);
    std::thread video_thread;

    while (!exit_program) {
        // 显示服务端列表
        std::cout << "\n===== 可用服务端列表 =====" << std::endl;
        {
            std::lock_guard<std::mutex> lock(servers_mutex);
            for (size_t i = 0; i < servers.size(); ++i) {
                std::cout << "[" << i << "] " << servers[i]["name"].asString()
                        << " (" << servers[i]["ip"].asString() << ")" << std::endl;
            }
        }

        // 用户选择
        std::cout << "\n输入编号选择服务端 (r刷新/q退出): ";
        std::string input;
        std::getline(std::cin, input);
        if (input == "q") {
            exit_program = true;
            break;
        } else if (input == "r") {
            servers.clear();
            continue;
        }

        // 添加输入验证和异常捕获
        int choice = -1;
        try {
            choice = std::stoi(input);
        } catch (const std::invalid_argument& e) {
            std::cerr << "错误：请输入数字编号！" << std::endl;
            continue;
        } catch (const std::out_of_range& e) {
            std::cerr << "错误：编号超出有效范围！" << std::endl;
            continue;
        }


        // 建立连接
        Json::Value target;
        {
            std::lock_guard<std::mutex> lock(servers_mutex);
            if (choice < 0 || choice >= servers.size()) {
            std::cerr << "错误：无效的服务器编号！" << std::endl;
            continue;
            }
            target = servers[choice];
        }

        heartbeat_socket = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(target["heartbeat_port"].asInt());
        inet_pton(AF_INET, target["ip"].asString().c_str(), &addr.sin_addr);

        last_server_info = target; // 保存上次连接信息

        // 处理连接断开后的逻辑
        if (abnormal_disconnect.load()) {
            std::cout << "尝试重新连接..." << std::endl;
            int retries = 3;
            while (retries-- > 0 && !exit_program) {
                heartbeat_socket = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in addr;
                addr.sin_family = AF_INET;
                addr.sin_port = htons(last_server_info["heartbeat_port"].asInt());
                inet_pton(AF_INET, last_server_info["ip"].asString().c_str(), &addr.sin_addr);

                if (connect(heartbeat_socket, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    is_connected = true;
                    abnormal_disconnect = false;

                    // 自动选择之前的摄像头
                    int cam_index = select_camera(heartbeat_socket, last_cam_index);
                    if (cam_index != -1) {
                        std::thread(handle_heartbeat).detach();
                        video_thread = std::thread(start_video_reception, last_server_info["ip"].asString());
                        video_thread.join();
                        break;
                    }
                }
                close(heartbeat_socket);
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            abnormal_disconnect = false;
        }

        if (connect(heartbeat_socket, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            is_connected = true;
            std::cout << "已连接至服务端: " << target["name"].asString() << std::endl;

            // 选择摄像头
            int cam_index = select_camera(heartbeat_socket);
            if (cam_index != -1) {
                std::thread(handle_heartbeat).detach();
                video_thread = std::thread(start_video_reception, target["ip"].asString());
                video_thread.join();
            }

            // 连接状态监控
            std::atomic<bool> video_running{true};
            std::thread([&](){
                while (video_running && !exit_program) {
                    if (!is_connected) {
                        std::cout << "连接已断开，返回服务器列表" << std::endl;
                        video_running = false; // 触发视频线程退出
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }).detach();

            // 启动视频线程
            video_thread = std::thread(start_video_reception, target["ip"].asString());
            video_thread.join();
            video_running = false;

            // 重置状态
            is_connected = false;
            abnormal_disconnect = false;
            // exit_program = false; // 重置退出标志
            close(heartbeat_socket);
        } else {
            std::cerr << "连接失败!" << std::endl;
            close(heartbeat_socket);
        }
    }

    exit_program = true;
    discovery_thread.join();
    if (video_thread.joinable()) video_thread.join();
    return 0;
}