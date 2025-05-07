# 📺 VideoServer - 实时视频传输系统

![Version](https://img.shields.io/badge/Release-v1.0.2-green)
![Platform](https://img.shields.io/badge/Platform-Linux-9cf)

**新一代GUI客户端已发布！** 🎉 立即体验更直观的操作界面：[Video-Client](https://github.com/Linductor-alkaid/Video-Client)

## 🌟 项目亮点

> 基于C++构建的高性能实时视频传输系统，专为局域网环境优化设计，集成智能网络自适应算法与硬件加速技术。


## 🚀 核心功能

### 服务端 (video_server)

| 模块            | 技术细节                                                                 |
|-----------------|--------------------------------------------------------------------------|
| 📡 服务广播      | UDP 37020端口广播，支持多网卡环境                                        |
| ❤️ 心跳检测     | TCP 5001端口，500ms间隔监测                                              |
| 📷 摄像头管理    | 自动识别/dev/video*设备，支持热插拔检测                                  |
| 🎥 视频流传输    | H.264硬编码，动态分辨率（1280x720 → 320x180）                           |
| 🌐 网络自适应    | 基于RTT的QoS算法，自动降级分辨率                                         |

### 客户端 (video_client)

| 模块            | 技术细节                                                                 |
|-----------------|--------------------------------------------------------------------------|
| 🔍 服务发现      | 多线程扫描，3秒内完成局域网设备探测                                     |
| 🖥️ 交互控制      | 终端/GUI双模式，支持实时分辨率切换                                       |
| 📺 视频解码      | GStreamer硬件加速流水线，延迟<200ms                                     |
| ⚡ 连接管理      | 智能重连机制，支持断线续传                                               |

## 🛠️ 快速部署

### 依赖安装 (Ubuntu)

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libopencv-dev libjsoncpp-dev
```

### 编译指南

```bash
git clone https://github.com/Linductor-alkaid/videoserver.git
cd videoserver
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

📦 生成产物：
- `./server` - 服务端程序
- `./client` - 命令行客户端

## 🖥️ 使用手册

### 服务端启动

```bash
./server 
```
### 客户端操作

```bash
./client
```

## 🔧 故障排查



### 防火墙配置

```bash
sudo ufw allow 5000:5001/udp
sudo ufw allow 37020/udp
```

## 📚 技术文档

### 协议规范

| 协议类型 | 端口   | 格式         | 频率       |
|----------|--------|--------------|------------|
| 服务发现 | 37020  | JSON广播     | 1Hz        |
| 心跳检测 | 5001   | TCP空包      | 2Hz        |
| 视频传输 | 5000   | RTP/H.264    | 动态调整    |

## 📜 版本历史

### v1.0.2 (2025-05-06)
- ✅ 修复服务端客户端断连崩溃问题
- 🛠️ 优化多线程资源竞争

### v1.0.1 (2025-05-04)
- 🌐 新增多网卡广播支持
- 📡 增强UDP广播稳定性

### v1.0.0 (2025-05-03)
- 🎉 初始版本发布
- 🚀 实现基础视频传输框架

---
