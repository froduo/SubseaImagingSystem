# SubseaImagingSystem · Qt 主程序

水下成像巡检取证系统的**本地 Qt 主程序**（运行于 Jetson Xavier NX）。负责两路相机取流显示、
热源检测与相对角度解算、红外激光控制、TCP 对外服务，以及录像取证 / 抓图。

> 配套的**远程 Web 操作台**（Python 网关 + go2rtc + 单文件前端）见本仓库 **`web`** 分支。

---

## 1. 功能概览

- **热像仪**（HM-TD2069N-18D，640×512）实时显示、热源检测（灰度→伪温度→阈值→形态学→连通域→取最热）、
  相对角度解算（HFOV/VFOV），叠加网格 / 中心十字 / 目标框 / 鼠标探针（坐标·灰度·温度）。
- **可见光相机**（iDS-2ZMN2312S）RTSP 取流与切换：主码流 4K(101) / 第三码流 1080p(103) / 子码流 704×576(102)。
- **低延时硬解预览**：GStreamer + `nvv4l2decoder`（NVDEC），`nvvidconv` 在 GPU(VIC) 内缩放，
  采集队列深度 1（始终只取最新帧），背压丢帧防 OOM。
- **相机控制（ISAPI）**：PTZ 云台、变焦步进（闭环单向不反向）、聚焦距离档位、光圈 IrisLevel、
  增益 GainLevel、曝光 auto/manual、日夜 IR-CUT、抓图、相机校时。
- **红外激光**：串口 Pelco-D 控制（开/关光、亮度、光斑角度、行程位置、电机复位）。
- **TCP 服务器**：默认 8888，对外推送角度/检测数据并接收控制指令（zoom/focus/iris/ptz/stop/status）。
- **录像取证**：外部 `ffmpeg -c:v copy` 直拷所选码流（零重编码），`+faststart`，SIGINT 优雅收尾。

---

## 2. 目录结构

```text
.
├── SubseaImagingSystem.pro     # qmake 工程
├── README.md                   # 本文件
├── config/
│   └── default.ini             # 相机/网络/检测/显示/串口/录像 等配置
├── resources/
│   └── resources.qrc           # Qt 资源
├── documents/                  # 设计/优化/使用说明（关键字 .md）
│   ├── Subsea_Imaging_System_Architecture.md
│   ├── 可见光预览延迟优化.md
│   ├── 录像回放说明.md
│   └── 相机自动校时说明.md
└── src/
    ├── main.cpp                # 入口：加载配置、启动主窗口
    ├── mainwindow.{h,cpp,ui}   # 主界面：画面渲染 / 控制面板 / TCP 日志 / 状态栏
    ├── visiblecamera.{h,cpp}   # 可见光：RTSP 硬解采集 + ISAPI 控制 + 录像
    ├── thermalcamera.{h,cpp}   # 热像仪：RTSP 采集
    ├── heatsourcedetector.{h,cpp}  # 热源检测算法
    ├── anglecalculator.{h,cpp} # 相对角度解算
    ├── irlaser.{h,cpp}         # 红外激光串口控制（Pelco-D）
    ├── tcpserver.{h,cpp}       # TCP 对外服务
    ├── configmanager.{h,cpp}   # 配置读写（QSettings/INI）
    └── logger.{h,cpp}          # 日志
```

---

## 3. 构建与运行

依赖：Qt5（Widgets/Gui/Network/SerialPort）、OpenCV 4（含 GStreamer 支持）、
Jetson `nvv4l2` 系列 GStreamer 插件、（可选）`ffmpeg`（录像）。

```bash
qmake SubseaImagingSystem.pro
make -j4
./build/SubseaImagingSystem
```

程序默认从以下路径查找配置（见 `src/main.cpp`）：
`<可执行文件目录>/../config/default.ini` → `~/project/.../config/default.ini` → `~/project/SubseaImagingSystem/config/default.ini`。

---

## 4. 关键配置（`config/default.ini`）

| 键 | 说明 |
|---|---|
| `visible/rtsp_url` | 可见光码流地址（`Channels/101` 主码流 4K / `103` / `102`） |
| `visible/native_preview_4k` | `true`(默认) 预览输出 4K 原生；`false` 由 GPU 缩放到显示区尺寸 |
| `network/thermal_camera_ip` 等 | 热像仪 ISAPI 地址/账号 |
| `thermal/rtsp_url` | 热像仪 RTSP |
| `detection/*` | 热源检测参数（阈值/最小面积/最大目标数/模式） |
| `display/grid` | 是否显示网格 |
| `serial/*` | 激光串口（默认 `/dev/ir_laser`，9600 8N1，RS485 RTS） |
| `network/tcp_server_port` | TCP 服务端口（默认 8888） |

---

## 5. 关键实现说明

### 5.1 可见光低延时硬解管线（`src/visiblecamera.cpp`）
```
rtspsrc latency=0 protocols=tcp drop-on-latency=true buffer-mode=0 ntp-sync=false do-retransmission=false
  ! rtph265depay ! h265parse
  ! nvv4l2decoder enable-max-performance=1 disable-dpb=true
  ! nvvidconv ! video/x-raw,format=BGRx
  ! appsink max-buffers=1 drop=true sync=false enable-last-sample=false
```
- BGRx 内存布局与 `QImage::Format_RGB32` 一致 → 界面侧零格式转换。
- `max-buffers=1 drop=true`：队列深度恒为 1，始终只取最新帧，消除“延迟随时间累积”。
- **背压**：上一帧未被界面消费则丢弃当前帧，避免事件队列/内存无限增长（并有超时自愈）。

### 5.2 4K 主码流预览策略
- **默认 4K 原生**：`setPreviewSize(0,0)`，界面分辨率栏显示 `3840×2160`，缩放查看保留 4K 细节；
- 可选 **GPU 缩放**：`nvvidconv` 缩放到显示区尺寸，界面零缩放、满帧 25fps、更省资源；
- 由界面「预览 4K 原生」复选框切换（持久化到 `visible/native_preview_4k`）；**录像始终直拷 4K，不受影响**。
- 4K 主码流在**本程序界面内直接显示**（无需外部播放器窗口）。

### 5.3 变焦闭环（`zoomStep`/`pollZoomStep`）
该机芯无绝对变焦接口，采用“读 `absoluteZoom` → 单向逼近目标 → 到达即发 `zoom=0`”的闭环，
方向全程不变、绝不反向，避免来回抖动；并有二次确认停止与总超时保护。

### 5.4 录像取证
使用**独立 ffmpeg 进程** `-rtsp_transport tcp -c copy -tag:v hvc1 -movflags +faststart`，
与采集线程解耦；结束时发送 SIGINT 让 mp4 正常写入 `moov`。

---

## 6. 相关文档

- [`documents/Subsea_Imaging_System_Architecture.md`](documents/Subsea_Imaging_System_Architecture.md) — 系统架构
- [`documents/可见光预览延迟优化.md`](documents/可见光预览延迟优化.md) — 4K 预览延迟根因与优化
- [`documents/录像回放说明.md`](documents/录像回放说明.md) — 录像与回放
- [`documents/相机自动校时说明.md`](documents/相机自动校时说明.md) — 相机时间校准
