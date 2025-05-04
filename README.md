# VideoStreamer - 实时视频传输系统

## 项目概述
本项目实现了一个基于C++的实时视频传输系统，包含服务端和客户端组件，支持动态分辨率调整和网络状态自适应。使用GStreamer进行视频流处理，JSON进行网络通信协议。

## 主要功能

### 服务端 (video_server)
- 服务广播：通过UDP广播（端口37020）宣告服务存在
- 心跳检测：TCP端口5001维护连接状态
- 摄像头管理：
  - 自动检测可用摄像头设备(/dev/video*)
  - 支持分辨率动态调整（1280x720 / 640x360 / 320x180）
- 视频流传输：
  - 使用H.264编码通过UDP端口5000传输
  - 自适应网络状况调整视频质量

### 客户端 (video_client)
- 服务发现：自动扫描局域网内可用服务端
- 交互式控制：
  - 列出可用服务端
  - 摄像头选择界面
- 视频接收：
  - 实时解码显示视频流
  - 支持网络拥塞检测（QoS反馈）
- 连接管理：
  - 心跳维护（500ms间隔）
  - 异常断开重连机制

## 依赖要求
- 编译器：支持C++11的编译器（g++/clang）
- 必需库：
  - GStreamer 1.0及开发文件
  - OpenCV 4.x（需V4L2支持）
  - jsoncpp 1.9.x
- 系统库：pthread, rt

## 编译指南

```bash
# 安装依赖（Ubuntu示例）
sudo apt install build-essential cmake libgstreamer1.0-dev libopencv-dev libjsoncpp-dev

# 编译项目
mkdir build
cd build
cmake ..
make -j4

# 生成的可执行文件
./server  # 服务端
./client  # 客户端
```

## 使用说明

### 启动服务端
1. 连接摄像头设备
2. 运行服务端程序：
   ```bash
   ./server
   ```
3. 程序将自动检测可用摄像头并通过UDP广播服务

### 使用客户端
1. 运行客户端程序：
   ```bash
   ./client
   ```
2. 命令行交互流程：
   ```
   1) 按"r"刷新服务器列表
   2) 输入数字选择服务器
   3) 选择摄像头编号
   4) 输入"q"退出当前连接
   ```
3. 视频窗口自动弹出显示实时画面

## 注意事项
1. 确保摄像头设备权限：
   ```bash
   sudo usermod -aG video $USER
   ```
2. 防火墙设置：
   - 开放UDP 37020, 5000
   - 开放TCP 5001
3. 网络要求：
   - 确保处于同一局域网
4. GStreamer插件：
   ```bash
   sudo apt install gstreamer1.0-plugins-base gstreamer1.0-plugins-good
   ```

## 协议说明
- 发现协议：UDP 37020端口，JSON格式广播
- 心跳协议：TCP 5001端口，500ms间隔
- 视频传输：RTP over UDP 5000端口

## 更新日志

- 2025-05-03 v1.0.0 初始功能实现，需要修改支持的ip广播
- 2025-05-04 v1.0.1 新增多网卡广播支持
