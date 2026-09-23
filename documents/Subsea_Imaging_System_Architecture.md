# 水下成像巡检取证系统 - 架构方案

## 一、项目概述

### 1.1 系统名称
**Subsea Imaging Inspection and Evidence System**（水下成像巡检取证系统）

### 1.2 目标平台
- **硬件**: NVIDIA Jetson Xavier NX（主网络 192.168.10.165）
- **操作系统**: Ubuntu 18.04 (L4T R32.6.1, 内核4.9.253-tegra)
- **开发框架**: Qt5 + C++11
- **构建系统**: qmake (.pro)

### 1.3 参考项目
`/home/tianheng/project/Qt_TensorRT_YOLOv12` — 沿用其代码风格和项目结构

---

## 二、硬件设备规格

### 2.1 设备网络连接信息

| 设备 | 型号 | IP地址 | 端口 | 用户名 | 密码 | 序列号 | 备注 |
|------|------|--------|------|--------|------|--------|------|
| 可见光一体化摄像机 | iDS-2ZMN2312S | **192.168.10.212** | 80 | admin | cisdi135 | iDS-2ZMN2312S20250822AARRGE7689474 | ISAPI/RTSP控制 |
| 声波成像仪(热像仪) | HM-TD2069N-18D | **192.168.10.211** | 80 | admin | cisdi135 | HM-TD2069N-18D20250823AACHEA5173214 | ISAPI/RTSP控制 |
| Jetson Xavier NX | 主控板 | 192.168.10.165 | - | - | - | - | Ubuntu 18.04 |

> 2026-09 变更：两台相机IP已由 192.168.200.x 专网迁移至 192.168.10.0/24 主网络，
> 相机与主板处于同一二层网络，无需额外网口/静态路由配置。声波成像仪密码由空改为 cisdi135。

**网络拓扑：**
```
Jetson Xavier NX
    └── eth0 → 主网络 (板子 192.168.10.165/24, 网关192.168.10.1)
        ├── 声波成像仪   HM-TD2069N-18D   192.168.10.211
        └── 一体化摄像机 iDS-2ZMN2312S    192.168.10.212
```

**网口规划（2026-09 实测与整改后）：**

| 接口 | 物理设备 | 驱动 | 用途 | 网络配置 |
|------|----------|------|------|----------|
| eth0 | Intel I350 双口卡 端口0 (PCIe `0004:01:00.0`) | igb | **主口**（外网 + 两台相机） | 静态 `192.168.10.165/24` + 网关 `192.168.10.1`，**唯一默认路由**（metric 101） |
| eth1 | Intel I350 双口卡 端口1 (`0004:01:00.1`) | igb | 备份口（当前未建链） | DHCP + `never-default` + route-metric 400（原 192.168.200.115 专网已废弃） |
| eth2 | 板载 GbE (`2490000.ethernet`) | nvethernet | 备份口 | DHCP + `never-default` + route-metric 500 |
| eth3 | USB 千兆网卡 RTL8153（Realtek `0bda:8153`，挂于 USB Hub 之后） | r8152 | 备份口 | DHCP + `never-default` + route-metric 600 |

> **接口命名核对（2026-09 实测，用于排除"命名错乱"疑虑）**：内核命名与总线拓扑完全一致，可用
> `ethtool -i <iface>` 的 `bus-info` 与 `/sys/class/net/<iface>/device` 直接验证：
> - `eth0` → PCIe `0004:01:00.0`（igb，I350 端口0，MAC `a0:00:00:c9:0d:eb`）
> - `eth1` → PCIe `0004:01:00.1`（igb，I350 端口1，MAC `a0:00:00:c9:0d:ec`，与 eth0 MAC 连号同卡）
> - `eth2` → 平台设备 `2490000.ethernet`（nvethernet，Jetson 板载，MAC `48:b0:2d:50:f2:be`）
> - `eth3` → USB `usb-3610000.xhci-3.1`（r8152，Realtek RTL8153，MAC `c4:c6:e6:2a:6c:9e`）
>
> 因此 `eth3` 的报文经 **USB 总线**（`lsusb` 显示 `0bda:8153 Realtek RTL8153 Gigabit Ethernet Adapter`，
> 位于 Realtek USB Hub `0bda:0411` 之后），**不可能是 Intel I350 的端口**；I350 的端口1 对应 `eth1`
> （igb / PCIe），该口自开机以来从未建链且累计收发包为 0。若载板实际只引出 3 个 RJ45 插座，则
> I350 第二路（`eth1`）处于"有控制器、无外部插座"状态，属**载板接线/丝印**范畴，而非 Linux 命名问题。
> 物理插座对应关系可用 `sudo ethtool -p eth0 10`（I350 两口支持 LED 闪烁定位）或插拔对比 `./check_links.sh` 确认。

**网口排障结论（2026-09）：**
- I350 主口曾长时间 `NO-CARRIER`：软件侧已逐项排除（管理状态 UP、NM 显示 connected、自协商 on 且 10/100/1000 全双工半双工全部已宣告、igb 无 NVM 校验报错、PCIe x1 Gen2 正常、错误计数全 0），**仅更换网线后同一端口立即 1000M/Full 建链**，故故障属**硬件介质层（网线 / 交换机端口）**，与驱动和配置无关。
- 整改项 1：`/etc/network/interfaces` 曾与 NetworkManager 双重管理 eth0/eth1，且文件尾部残留无效配置行；已改为仅保留 `source-directory`，全部网口由 NM 统一管理。
- 整改项 2：eth0 静态 `192.168.10.165` 与 eth2 的 DHCP 租约曾**同地址冲突**（导致双默认路由 + ARP flux + 交换机 MAC 漂移）；现通过更换 eth2 的 DHCP client-id（`duid`）取得独立地址，并对备份口统一启用 `never-default` + 高 route-metric，确保**全机仅 1 条默认路由**。

### 2.2 海康微影在线测温热像仪 HM-TD2069N-18D（声波成像仪）
| 参数 | 值 |
|------|-----|
| 传感器 | 氧化钒非制冷型探测器 |
| 分辨率 | 640×512 |
| 帧频 | 50fps |
| 像元尺寸 | 12μm |
| 焦距 | 18mm |
| 视场角 | 24.2°(H) × 19.4°(V) |
| MRAD | 0.67 |
| 测温范围 | -20°C~150°C 或 0°C~650°C |
| 测温精度 | ±2°C或读数的±2% |
| 视频编码 | H.265/H.264/MJPEG |
| 接口 | RJ45 10M/100M/1000M |
| 协议 | RTSP, ONVIF, SDK 16/32bit |
| 测温规则 | 点/线/框，最多21个 |
| 伪彩 | 18种 |

**关键FOV参数用于角度计算：**
- 水平视场角: **24.2°** → 每像素角度 = 24.2° / 640 = **0.0378125°/像素**
- 垂直视场角: **19.4°** → 每像素角度 = 19.4° / 512 = **0.037890625°/像素**

### 2.3 海康威视一体化摄像机 iDS-2ZMN2312S
| 参数 | 值 |
|------|-----|
| 型号 | iDS-2ZMN2312S |
| 类型 | 一体化网络摄像机（23倍光学变焦） |
| 分辨率 | 200万像素 (1920×1080) |
| 变焦 | 23倍光学变焦 |
| 接口 | RJ45 网口 |
| 协议 | RTSP, ONVIF, ISAPI, Hikvision SDK |
| 功能 | 自动对焦、手动变焦、拍照 |
| **IP地址** | **192.168.10.212** |
| **HTTP端口** | **80** |
| **RTSP端口** | **554** |
| **用户名** | **admin** |
| **密码** | **cisdi135** |
| 视频编码 | H.265(HEVC) — 通道101(4K)/102(704×576)/103(1080p) |
| 预览码流 | **通道103** (1920×1080, 硬件解码约23fps) |
| 抓拍码流 | **通道101** (3840×2160, 经 ISAPI /Streaming/channels/101/picture 取全分辨率JPEG) |

### 2.4 红外激光变焦模组 IR4W850B-T01
| 参数 | 值 |
|------|-----|
| 有效距离 | ≥800m |
| 波长 | 850±10nm |
| 变焦范围 | 2°~65°连续可调 |
| 变倍时间 | ≤4秒（远角-近角） |
| 激光功率 | 4.4±0.3W |
| 控制方式 | TTL232/RS-485 |
| 通信协议 | 视辉专用协议 / Pelco_D |
| 默认波特率 | 9600bps |
| 工作电压 | DC12V±10% |

---

## 三、系统架构

### 3.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                        Qt Main Window                           │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │ 热像仪视频区  │  │ 可见光视频区  │  │    控制面板          │  │
│  │  QLabel+QImage│  │  QLabel+QImage│  │  对焦/变焦/曝光/增益 │  │
│  └──────┬───────┘  └──────┬───────┘  │  光斑/亮度/拍照      │  │
│         │                 │          │  角度差显示           │  │
│         │                 │          └──────────────────────┘  │
│  ┌──────┴─────────────────┴──────────────────────────────────┐  │
│  │                    数据处理层                               │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐ │  │
│  │  │ 热源检测  │ │ 角度计算  │ │ TCP服务   │ │ 参数配置管理  │ │  │
│  │  │ Detector │ │ AngleCalc│ │ TcpServer│ │ ConfigMgr   │ │  │
│  │  └──────────┘ └──────────┘ └──────────┘ └──────────────┘ │  │
│  └───────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    硬件控制层                               │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐ │  │
│  │  │ 热像仪    │ │ 可见光    │ │ 红外激光  │ │ 串口管理     │ │  │
│  │  │ ThermalCam│ │ VisCam   │ │ IRLaser  │ │ SerialMgr   │ │  │
│  │  │ (ISAPI)   │ │ (ISAPI)  │ │ (UART)   │ │ (QSerial)   │ │  │
│  │  └──────────┘ └──────────┘ └──────────┘ └──────────────┘ │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
         │网口             │网口            │串口
    ┌────┴────┐      ┌────┴────┐     ┌────┴────┐
    │热像仪    │      │可见光    │     │红外激光  │
    │HM-TD2069│      │IDS-2ZMN │     │IR4W850B │
    └─────────┘      └─────────┘     └─────────┘
```

### 3.2 模块划分

```
SubseaImagingSystem/
├── SubseaImagingSystem.pro          # 主项目文件
├── src/
│   ├── main.cpp                     # 程序入口
│   ├── mainwindow.h/cpp/ui          # 主窗口
│   ├── config/
│   │   └── configmanager.h/cpp      # 配置管理（INI文件）
│   ├── camera/
│   │   ├── thermalcamera.h/cpp      # 热像仪控制（ISAPI/RTSP）
│   │   └── visiblecamera.h/cpp      # 可见光摄像机控制（ISAPI/RTSP）
│   ├── laser/
│   │   └── irlaser.h/cpp            # 红外激光模组（串口Pelco_D）
│   ├── detection/
│   │   ├── heatsource.h/cpp         # 热源检测（温度阈值+连通域）
│   │   └── anglecalculator.h/cpp    # 角度差计算
│   ├── network/
│   │   └── tcpserver.h/cpp          # TCP服务器（角度差推送）
│   ├── serial/
│   │   └── serialmanager.h/cpp      # 串口管理（QSerialPort）
│   └── utils/
│       ├── imagedata.h              # 图像数据结构
│       └── logger.h/cpp             # 日志工具
├── resources/
│   ├── icons/                       # 图标资源
│   └── resources.qrc                # Qt资源文件
├── config/
│   └── default.ini                  # 默认配置文件
└── build/                           # 构建输出目录
```

---

## 四、核心模块详细设计

### 4.1 热像仪模块 (ThermalCamera)

**通信方式**: ISAPI协议（HTTP REST API）+ RTSP视频流

**核心功能：**
- 连接热像仪设备（IP地址可配置）
- 通过RTSP获取实时视频流（H.264/H.265解码为QImage）
- 通过ISAPI获取温度数据（640×512像素级温度矩阵）
- 支持测温规则配置（点、线、框）

**ISAPI关键接口：**
```
GET /ISAPI/Thermal/channels/1/streaming    # 获取热成像流
GET /ISAPI/Thermal/thermometry/rules       # 获取测温规则
PUT /ISAPI/Thermal/thermometry/rules       # 设置测温规则
GET /ISAPI/Thermal/thermometry/data        # 获取温度数据
```

**RTSP URL格式：**
```
rtsp://admin:cisdi135@192.168.10.211:554/Streaming/Channels/101
```

**实际流参数（已实测）：**
| 通道 | 编码 | 分辨率 | 说明 |
|------|------|--------|------|
| 101 | H.264 High | 640×512 | 主码流（默认使用） |
| 102 | H.264 Main | 640×512 | 子码流 |

> 解码注意：本平台 GStreamer **未提供 avdec_h264/avdec_h265**，
> GStreamer 回退管线必须使用 NVIDIA 硬件解码器 `nvv4l2decoder`（实测 640×512 约 45fps）。

**线程模型：**
- 视频解码在独立线程（QThread）中运行
- 温度数据获取在独立定时器中轮询（~10Hz）
- 解码后的QImage通过信号槽传递到UI线程

### 4.2 可见光摄像机模块 (VisibleCamera)

**通信方式**: ISAPI协议（HTTP REST API）+ RTSP视频流

**核心功能：**
- 连接可见光摄像机
- RTSP获取实时视频流
- 控制自动/手动对焦
- 控制变焦（放大/缩小）
- 控制曝光、增益、白平衡
- 拍照并保存图像

**ISAPI关键接口（2026-09 实测校正）：**
```
GET  /ISAPI/Streaming/channels                    # 视频流通道列表(101/102/103, 均为 H.265)
GET  /ISAPI/Streaming/channels/1/picture          # 抓图 (与 /101/picture 等价, 3840x2160 JPEG)
GET  /ISAPI/Image/channels/1                      # 图像参数总文档(含 FocusConfiguration/focusStyle)
GET  /ISAPI/Image/channels/1/exposure|iris|gain   # 曝光模式 / 光圈 / 增益
GET  /ISAPI/Image/channels/1/shutter|whiteBalance # 快门 / 白平衡 (存在)
GET  /ISAPI/PTZCtrl/channels/1/status             # 回读 absoluteZoom (10..230)
PUT  /ISAPI/PTZCtrl/channels/1/continuous         # 变焦: <zoom>-100..100</zoom>, 0=停止
PUT  /ISAPI/Image/channels/1/exposure             # ExposureType: auto/manual/IrisFirst/ShutterFirst
PUT  /ISAPI/Image/channels/1/iris                 # IrisLevel: 160..2200 (数值越大画面越暗)
PUT  /ISAPI/Image/channels/1/gain                 # GainLevel/GainLimit: 0..100
PUT  /ISAPI/Image/channels/1                      # 整篇图像文档, 用于切换对焦模式 focusStyle
```

> **控制端点踩坑记录（2026-09，控制按钮"点了没反应"的根因）**：
> 旧实现使用 `PUT /ISAPI/Image/channels/1/{focus, zoom, settings}`，这三个端点在
> iDS-2ZMN2312S 上**全部返回 HTTP 404**（`app.log` 中可见连续的 `server replied: Not Found`），
> 因此变焦/自动对焦/曝光点击后画面毫无变化。已按实测结果改为上表端点：
> - **变焦** → PTZ `continuous`（实测 zoom=+60 使 absoluteZoom 由 10 增到 67）
> - **自动对焦** → 整篇 PUT `/ISAPI/Image/channels/1`，改写 `focusStyle`（AUTO/SEMIAUTOMATIC/MANUAL）
> - **曝光/光圈/增益** → 先切 `ExposureType=manual`，再下发 `iris` 与 `gain`
>
> **量化验证（通过 ISAPI 抓图计算平均亮度，证明指令确实改变画面）**：
> IrisLevel 160→亮度 251.1，2200→42.0；GainLevel 0→0.9，100→91.3。
> **`ShutterLevel` 该机芯无实际效果**（1/25 与 1/30000 亮度均为 251.1），故曝光控制不使用快门。
> `PUT /ISAPI/PTZCtrl/channels/1/absolute`（绝对变焦）返回 400，不可用。

> 抓图路径说明（2026-09 实测）：
> - 一体化摄像机 iDS-2ZMN2312S：`/1/picture` 与 `/101/picture` 均可用（3840×2160 JPEG），程序统一使用 `/1/picture`
> - 声波成像仪 HM-TD2069N-18D：**仅** `/1/picture` 可用（`/101/picture` 返回 403），抓图约 640×512
> - 测温能力：`GET /ISAPI/Thermal/capabilities` 返回 `isSupportThermometry=true`、
>   `isSupportRealTimeThermometryForHTTP=true`，可用于后续实时温度读取

### 4.3 红外激光模组模块 (IRLaser)

**通信方式**: UART串口（RS-232/TTL）

**协议**: Pelco_D协议，9600bps，8N1

**Pelco_D协议格式：**
```
| 同步字节 | 地址 | 命令1 | 命令2 | 数据1 | 数据2 | 校验和 |
|  0xFF    | 0x01 |  cmd1 |  cmd2 |  d1   |  d2   |  sum  |
```

**核心功能：**
- 光斑大小调节（变焦控制）：2°~65°连续可调
- 亮度调节
- 自动调光模式
- 与可见光摄像机对焦联动

**Pelco_D命令映射：**
- 变焦拉近/拉远 → 控制光斑大小
- 聚焦远/近 → 辅助功能
- 设置预置位 → 快速切换光斑模式

### 4.4 热源检测模块 (HeatSourceDetector)

**算法流程：**
```
温度矩阵(640×512) → 阈值分割 → 连通域分析 → 热源定位
```

**详细步骤：**
1. 获取640×512温度矩阵（float数组，单位°C）
2. 设定温度阈值（可配置，默认比环境温度高10°C）
3. 二值化：温度 > 阈值的像素标记为前景
4. 形态学操作：开运算去噪、闭运算填充
5. 连通域分析：找到所有热源区域
6. 选择最大热源（或最热热源）
7. 计算热源中心像素坐标 (cx, cy)

**输出结构：**
```cpp
struct HeatSource {
    float maxTemp;        // 最高温度
    float avgTemp;        // 平均温度
    int centerX;          // 中心X像素坐标
    int centerY;          // 中心Y像素坐标
    int boundingBoxX;     // 边界框
    int boundingBoxY;
    int boundingBoxW;
    int boundingBoxH;
    float area;           // 面积
};
```

### 4.5 角度计算模块 (AngleCalculator)

**计算原理：**

热像仪参数：
- 水平视场角: HFOV = 24.2°
- 垂直视场角: VFOV = 19.4°
- 图像宽度: W = 640 像素
- 图像高度: H = 512 像素
- 图像中心: (320, 256)

**角度差计算公式：**
```
ΔX = (sourceCenterX - imageCenterX) × (HFOV / W)
   = (sourceCenterX - 320) × (24.2° / 640)
   = (sourceCenterX - 320) × 0.0378125°

ΔY = (sourceCenterY - imageCenterY) × (VFOV / H)
   = (sourceCenterY - 256) × (19.4° / 512)
   = (sourceCenterY - 256) × 0.037890625°
```

**输出结构：**
```cpp
struct AngleDifference {
    float deltaX;         // X方向角度差（度）
    float deltaY;         // Y方向角度差（度）
    float distance;       // 偏离中心的总角度
    bool valid;           // 是否有效（检测到热源）
};
```

### 4.6 TCP服务器模块 (TcpServer)

**功能：**
- 监听可配置端口（默认8888）
- 接受多个客户端连接
- 实时推送角度差数据（JSON格式）
- 心跳检测

**数据格式（JSON）：**
```json
{
    "type": "angle_data",
    "timestamp": 1694000000,
    "delta_x": 1.234,
    "delta_y": -0.567,
    "distance": 1.358,
    "valid": true,
    "max_temp": 45.6,
    "source_x": 350,
    "source_y": 240
}
```

**发送频率**: 10Hz（每100ms发送一次）

### 4.7 配置管理模块 (ConfigManager)

**配置文件格式**: INI

**配置项：**
```ini
[network]
# 热像仪 HM-TD2069N-18D (声波成像仪)
thermal_camera_ip=192.168.10.211
thermal_camera_port=80
thermal_camera_user=admin
thermal_camera_pass=cisdi135
# 可见光一体化摄像机 iDS-2ZMN2312S
visible_camera_ip=192.168.10.212
visible_camera_port=80
visible_camera_user=admin
visible_camera_pass=cisdi135
tcp_server_port=8888

[serial]
port_name=/dev/ttyUSB0
baud_rate=9600
data_bits=8
parity=none
stop_bits=1

[detection]
temp_threshold_delta=10.0
min_area=50
max_sources=5

[camera]
save_path=/home/tianheng/evidence
image_format=jpg
image_quality=95

[thermal]
hfov=24.2
vfov=19.4
image_width=640
image_height=512

[laser]
default_zoom=30
default_brightness=80
auto_mode=true
```

---

## 五、主界面设计

### 5.1 布局方案

```
┌─────────────────────────────────────────────────────────────────┐
│  菜单栏: 文件 | 设备 | 视图 | 帮助                               │
├─────────────────────────────────────────────────────────────────┤
│  工具栏: 连接设备 | 断开 | 拍照 | 截图 | 设置                     │
├──────────────────────────┬──────────────────────────┬───────────┤
│                          │                          │           │
│    热像仪实时画面          │   可见光实时画面          │ 控制面板   │
│    640×512               │   1920×1080             │           │
│                          │                          │ [自动对焦] │
│    ┌──┐ 热源标记          │                          │ [手动对焦] │
│    │+ │ 十字光标          │                          │ ──变焦──  │
│    └──┘                  │                          │ ◄──┼──►  │
│                          │                          │ ──曝光──  │
│                          │                          │ ◄──┼──►  │
│                          │                          │ ──增益──  │
│                          │                          │ ◄──┼──►  │
│                          │                          │ ──白平衡── │
│                          │                          │ [自动/手动]│
│                          │                          │           │
│                          │                          │ ──光斑──  │
│                          │                          │ ◄──┼──►  │
│                          │                          │ ──亮度──  │
│                          │                          │ ◄──┼──►  │
│                          │                          │           │
│                          │                          │ [拍照取证] │
│                          │                          │           │
├──────────────────────────┴──────────────────────────┤ 角度差    │
│                      状态栏                          │ ΔX: 1.23°│
│  设备状态 | 帧率 | 分辨率 | 连接状态                    │ ΔY:-0.57°│
│                                                     │ 距离:1.36°│
│                                                     │ 温度:45.6°C│
└─────────────────────────────────────────────────────┴───────────┘
```

### 5.2 控件映射

| 功能 | Qt控件 | 说明 |
|------|--------|------|
| 视频显示 | QLabel + QImage | 两个独立区域 |
| 对焦控制 | QPushButton | 自动/手动切换 |
| 变焦调节 | QSlider 或 QSpinBox | 连续调节 |
| 曝光调节 | QSlider | 范围可配置 |
| 增益调节 | QSlider | 范围可配置 |
| 白平衡 | QComboBox | 自动/手动/预设 |
| 光斑大小 | QSlider | 对应激光模组变焦2°-65° |
| 光斑亮度 | QSlider | 亮度百分比 |
| 角度差显示 | QLCDNumber 或 QLabel | 实时更新ΔX/ΔY |
| 温度显示 | QLabel | 最高温度 |
| 拍照按钮 | QPushButton | 保存当前可见光画面 |
| 保存路径 | QLineEdit + QPushButton | 可配置 |
| 端口号 | QSpinBox | TCP端口配置 |
| 串口号 | QComboBox | 可用串口列表 |

---

## 六、线程模型

```
主线程 (UI Thread)
├── MainWindow
│   ├── 更新视频画面（QImage → QLabel）
│   ├── 更新角度差显示
│   ├── 处理用户交互（按钮、滑块）
│   └── 参数配置
│
热像仪线程 (ThermalThread)
├── RTSP视频流解码（OpenCV VideoCapture）
├── ISAPI温度数据获取
├── 热源检测
├── 角度计算
└── 信号: frameReady(QImage), tempDataReady(...), heatSourceDetected(...)
│
可见光线程 (VisibleThread)
├── RTSP视频流解码
└── 信号: frameReady(QImage)
│
TCP服务器线程 (TcpServerThread)
├── 监听连接
├── 接收/发送数据
└── 信号: clientConnected/disconnected, dataReceived(...)
│
串口线程 (可选，或在主线程中用QSerialPort)
├── 发送控制命令
└── 接收响应数据
```

---

## 七、关键依赖库

| 库 | 版本 | 用途 |
|----|------|------|
| Qt5 | 5.9+ | GUI框架 |
| OpenCV | 4.x | 视频解码、图像处理 |
| QSerialPort | Qt5 | 串口通信 |
| QTcpServer/Socket | Qt5 | TCP网络通信 |
| 海康SDK | ISAPI | 设备控制（HTTP REST） |

### OpenCV编译依赖
Jetson上需要确认OpenCV是否已安装：
```bash
pkg-config --modversion opencv4
```

---

## 八、数据流

```
热像仪 ──RTSP──→ ThermalThread ──解码──→ QImage ──信号──→ UI显示
                        │
                        ├──温度数据──→ HeatSourceDetector
                        │                    │
                        │              热源中心坐标
                        │                    │
                        │              AngleCalculator
                        │                    │
                        │              ΔX, ΔY角度差
                        │                    │
                        ├──信号────────→ UI角度显示
                        │
                        └──信号────────→ TcpServer ──推送──→ 客户端

可见光 ──RTSP──→ VisibleThread ──解码──→ QImage ──信号──→ UI显示
                   │
                   └──拍照命令──→ ISAPI抓图 ──保存──→ 文件

用户操作 ──UI──→ 控制命令 ──ISAPI──→ 可见光摄像机
                         ──串口──→ 红外激光模组
```

---

## 九、开发计划（分阶段）

### Phase 1: 项目框架搭建
1. 创建项目目录结构
2. 编写.pro文件（配置Qt、OpenCV、串口依赖）
3. 实现MainWindow基本UI框架
4. 实现ConfigManager配置管理

### Phase 2: 视频接入
5. 实现ThermalCamera热像仪RTSP视频流
6. 实现VisibleCamera可见光RTSP视频流
7. 视频帧解码和UI显示

### Phase 3: 热源检测
8. 实现热源检测算法（温度阈值+连通域）
9. 实现角度差计算
10. 热源标记叠加显示

### Phase 4: 设备控制
11. 实现可见光摄像机ISAPI控制（对焦、变焦、拍照）
12. 实现红外激光模组串口控制（Pelco_D协议）
13. 对焦联动逻辑

### Phase 5: 网络通信
14. 实现TCP服务器（角度差推送）
15. JSON数据格式

### Phase 6: 界面完善
16. 完善控制面板所有控件
17. 参数配置对话框
18. 日志显示
19. 状态栏信息

### Phase 7: 集成测试
20. 与实际硬件联调
21. 性能优化
22. Bug修复

---

## 十、编译与部署

### 10.1 在Jetson上编译
```bash
cd /home/tianheng/project/Subsea\ Imaging\ Inspection\ and\ Evidence\ System
qmake SubseaImagingSystem.pro
make -j4
```

### 10.2 运行
```bash
./build/SubseaImagingSystem
```

---

## 十一、界面与控制增强（2026-09 实现）

### 11.1 启动自动连接
程序启动 800ms 后自动依次连接 **红外激光（串口）→ 可见光（RTSP）→ 热像仪（RTSP + 检测定时器）**，
对应 [`MainWindow::autoConnectDevices()`](src/mainwindow.cpp:455)。

### 11.2 红外激光串口修复（ttyUSB0 打不开）
**根因**：`/dev/ttyUSB0` 权限为 `root:dialout 660`，而运行用户 `lcfc` **不在 dialout 组**，
日志表现为 `Failed to open serial port: ttyUSB0 - Permission denied`（修复前累计 30 次）。
**解决**：
- 新增 udev 规则 `/etc/udev/rules.d/99-ir-laser.rules`：FTDI(0403:6001) 生成稳定符号链接，
  避免 `ttyUSBx` 编号漂移，并放开权限
  ```
  SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", SYMLINK+="ir_laser", GROUP="dialout", MODE="0666"
  ```
- `sudo usermod -aG dialout lcfc`（对新会话生效）
- 配置项 `serial/port_name` 改为 `/dev/ir_laser`
- 修复后实测：`IR Laser serial port opened: ir_laser @ 9600bps`

### 11.3 可见光控制增强
| 控件 | 单位/范围 | 实现 |
|------|-----------|------|
| 变焦 sliderZoom | **10..230 位置 ↔ 1.0×..23.0×** | 闭环控制：读 `PTZCtrl/.../status` 的 absoluteZoom，用 continuous 逼近目标后停止（机芯不支持绝对变焦接口） |
| 自动对焦 btnAutoFocus | - | 改写 `Image/channels/1` 的 `focusStyle=AUTO` |
| 曝光 sliderExposure | 0..100 (暗..亮) | 切 manual + 光圈 + 增益 |
| 光圈 Iris / 增益 Gain | 0..100，**界面实时显示数值** | `Image/channels/1/iris`、`/gain` |
| 画面缩放 | 0.2×~8.0× | 滚轮缩放、左键拖拽平移、双击复位 |

串口下拉框 `comboSerialPort` 安装事件过滤器，**鼠标点击时自动刷新可用端口列表**。

### 11.4 热像仪画面叠加（可交互）
- **网格线**：8×6 网格并标注像素坐标，复选框"显示网格线"可开关（配置项 `display/grid`）
- **中心坐标系**：以图像中心为原点绘制十字与 `X+/X-/Y+/Y-` 方向标识
- **像素偏差**：ΔX/ΔY 以**像素**为单位显示（正X=偏右、正Y=偏下）并标注偏离方向，同时保留角度值
- **目标标记**：候选热源画细黄框，选中目标画**粗红框 + 目标中心十字 + 目标到图像中心的连线**
- **右上角状态**：实时显示"检测到热源目标 / 未检测到热源目标"

### 11.5 热源检测双模式
- **自动模式**（默认）：对候选热源按"亮度"与"面积"归一化后加权评分（各 0.5），
  取**又亮又大**者为目标，对应 [`HeatSourceDetector::selectTarget()`](src/heatsourcedetector.cpp:198)
- **手动模式**：直接以 **最低温度(min_temp)** 与 **最小面积像素(min_area_pixels)** 作为筛选参数，
  界面提供 QDoubleSpinBox / QSpinBox，切换模式时自动启用/禁用这两个参数
- 参数持久化到 `config/default.ini` 的 `[detection]` 段

### 11.6 TCP 服务器与日志窗体
- TCP 分组新增 **"服务地址: <本机IP>:<端口>"** 显示（优先选 `192.168.10.x` 主网络地址）
- 新增底部可停靠**日志窗体**：显示客户端连接/断开（含**客户端 IP:端口**）、收到的交互数据、
  串口与相机连接过程等全部 INFO 日志（最多保留 5000 行，自动滚屏）

### 11.7 状态栏分辨率
状态栏右侧常驻两个标签，实时显示 **热像仪: 640x512**、**可见光: 3840x2160**（依实际帧尺寸）。

---

## 十二、风险与注意事项

1. **海康SDK兼容性**: Jetson上需使用ISAPI（HTTP REST）方式，避免使用Windows SDK
2. **OpenCV版本**: Jetson预装的OpenCV版本需要确认，可能需要编译4.x
3. **串口权限**: 已通过 udev 规则 `99-ir-laser.rules` 解决；备用方案 `sudo usermod -aG dialout lcfc`
4. **网络带宽**: 两路高清视频流同时传输需确保网络带宽充足
5. **温度数据获取**: 当前热源检测以热像图灰度映射温度(仅用于阈值/标记定位)，
   精确温度需接入 ISAPI 实时测温接口（`isSupportRealTimeThermometryForHTTP=true`，已确认支持）
6. **Pelco_D协议**: 需要红外激光模组的实际协议文档确认命令细节
