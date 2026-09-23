# 水下成像巡检取证系统 · 远程操作台与相机控制网关

> 程序：`gateway.py`（Python 3，纯标准库，无第三方 pip 依赖）
> 访问地址：<http://192.168.10.165:8080/>
> 部署目录：`/home/lcfc/remote/`
> 本文档用于下次快速恢复上下文，请优先阅读第 1、2、13、15、16 节。

---

## 1. 这是什么

一个与 Qt 主程序（SubseaImagingSystem）**解耦**的远程操作网关，跑在 Jetson（主机名 `jetson-nx-165`）上，对外提供：

1. **静态操作台页面** —— 单文件前端 `web/index.html`（标题「水下成像巡检取证系统 v15」），浏览器直接访问 8080 即可使用，无需安装任何客户端。
2. **可见光相机 ISAPI 直控** —— 云台（PTZ）/ 变焦 / 光圈 / 增益 / 曝光 / 对焦 / 日夜（IR-CUT）/ 抓图，走海康 ISAPI（HTTP Digest 认证，无状态）。
3. **红外激光直控（可选）** —— 通过串口 `/dev/ir_laser` 下发 Pelco_D 帧（开光/关光/亮度/光斑/角度/行程）。
4. **录像与取证** —— 调用 `ffmpeg` 从 go2rtc 的本地 RTSP 出口 `-c copy` 直存 mp4（不转码），抓图与录像统一落在 `/home/lcfc/evidence`。
5. **8888 桥接（可选）** —— 连接 Qt 程序的 TCP 8888，把推送（如 `angle_data`）转成 SSE 给网页；程序未启动该服务时自动降级，不影响其它功能。
6. **网关自带 TCP 控制服务器** —— 可让网页一键在 8888 起一个带 JSON 应答的 TCP 服务，供外部程序做角度闭环。

### 设计要点（改动前务必理解）

- **与现有程序解耦**：只用 ISAPI（HTTP 无状态）与本地 RTSP，**不修改** Qt 程序。
- **云台/变焦是「连续量」**：下发后按 `ms` 到时自动回零，避免「跑飞」；用「代数号（generation）」防止旧定时器误停新动作。
- **所有写操作串行化**：统一在全局锁 `_lock` 内，防止并发下发互相打断。
- **静态与取证文件均做路径防护**：只取 `basename`，`normpath` 后必须仍在允许目录内。

---

## 2. 部署与运行方式

由 **systemd** 托管，开机自启，崩溃自动重启。

| 项 | 值 |
|---|---|
| 服务名 | `subsea-console.service` |
| 单元文件 | `/etc/systemd/system/subsea-console.service` |
| 描述 | Subsea Imaging remote operator console + control gateway |
| 用户/组 | `lcfc` / `lcfc`（附加组 `dialout`，用于串口；`LimitNOFILE=65536`） |
| 工作目录 | `/home/lcfc/remote` |
| 启动命令 | `/usr/bin/python3 /home/lcfc/remote/gateway.py` |
| 依赖 | `network-online.target`、`go2rtc.service` |
| 重启策略 | `Restart=always`，`RestartSec=3` |
| 监听 | `0.0.0.0:8080` |

单元文件内容：

```ini
[Unit]
Description=Subsea Imaging remote operator console + control gateway
After=network-online.target go2rtc.service
Wants=network-online.target

[Service]
Type=simple
User=lcfc
Group=lcfc
WorkingDirectory=/home/lcfc/remote
ExecStart=/usr/bin/python3 /home/lcfc/remote/gateway.py
Restart=always
RestartSec=3
SupplementaryGroups=dialout
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

常用运维命令：

```bash
systemctl status  subsea-console.service
systemctl restart subsea-console.service
systemctl stop    subsea-console.service
journalctl -u subsea-console.service -f          # 看实时日志
ss -ltnp | grep ':8080'                          # 谁占用了 8080
```

> 程序自身几乎不打印日志（只覆盖了 `log_message` 为静默），启动时只打印 3 行相机/RTSP/监听信息。

---

## 3. 目录结构

```text
/home/lcfc/remote/
├── gateway.py              # 主程序（约 1152 行，见第 6~11 节接口清单）
├── gateway.py.bak          # 历史版本备份（.bak / .bak2 / .bak3 / .v7.bak）
├── web/
│   ├── index.html          # 单文件操作台前端（所有 CSS/JS 内联）
│   └── index.html.v1..v14.bak # 前端历史版本备份
├── go2rtc                  # go2rtc 可执行文件
├── go2rtc.yaml             # go2rtc 配置（流定义 / 端口）
├── go2rtc.yaml.bak*        # 配置历史备份
├── tc.sh                   # 可见光 H.265→H.264 硬转码脚本（1080/4k）
├── __pycache__/            # Python 字节码缓存
└── README.md               # 本文件

/home/lcfc/evidence/        # 抓图与录像输出目录（自动创建）
```

---

## 4. 运行依赖

| 依赖 | 用途 | 说明 |
|---|---|---|
| Python 3.8+ | 运行主程序 | 只用标准库：`http.server` / `urllib` / `socket` / `termios` / `subprocess` |
| `go2rtc` | RTSP/WebRTC 转发 | 本目录内自带二进制；监听 `1984`(API) / `8554`(RTSP) / `8555`(WebRTC) |
| `ffmpeg` | 录像直存 | `-rtsp_transport tcp -c copy -movflags +faststart` |
| `gst-launch-1.0` + Jetson `nvv4l2*` | 硬转码 H.264 | 仅 `vis1080h264` / `vis4kh264` 通用流与 `tc.sh` 使用 |
| 串口 `/dev/ir_laser` | 激光控制 | 9600 8N1；**被 Qt 程序独占时网关无法直控**（见第 10、13 节） |

---

## 5. 配置来源

程序**尽量复用 Qt 程序的配置作为单一数据源**，而不是各存一份。

### 5.1 相机参数（只读）

读取路径硬编码为：

```python
CONFIG_INI = "/home/tianheng/project/SubseaImagingSystem/config/default.ini"
```

解析的键：`visible_camera_ip`、`visible_camera_port`、`visible_camera_user`、`visible_camera_pass`。
读取失败或缺失时回退到内置默认值（代码里写死为 `192.168.10.212:80 / admin / cisdi135`）。

> 即：网页上的相机控制对象由 Qt 程序的 `default.ini` 决定。改相机地址 → 改那份 ini → 重启 `subsea-console.service`。

### 5.2 检测参数（可读写）

网页可在线修改以下键（`ini_write_detect()` 按行精确替换/插入，保留原文件非标准注释行）：

| 键名 | 所属 section | 校验/规范化 |
|---|---|---|
| `enable_detection` | `[detection]` | 归一化为 `true/false` |
| `manual_mode` | `[detection]` | 同上 |
| `max_sources` | `[detection]` | 钳制 `1~50` |
| `min_area_pixels` | `[detection]` | 钳制 `1~100000` |
| `min_temp` | `[detection]` | 保留 1 位小数 |
| `temp_threshold_delta` | `[detection]` | 保留 1 位小数 |
| `grid` | `[display]` | 归一化为 `true/false` |

写入后返回提示：**「已写入配置文件；需重启本地程序后生效」**。

### 5.3 程序内常量

| 常量 | 值 | 含义 |
|---|---|---|
| `PORT` | `8080` | HTTP 监听端口 |
| `WEB_DIR` | `<脚本目录>/web` | 静态文件根 |
| `GO2RTC_RTSP` | `rtsp://127.0.0.1:8554` | 录像取流出口 |
| `EVIDENCE_DIR` | `/home/lcfc/evidence` | 抓图/录像目录 |
| `PROG_TCP` | `127.0.0.1:8888` | Qt 程序控制面（桥接用途，硬编码） |
| `IRIS_TABLE` | `[160,200,240,280,340,400,480,560,680,960,1100,1400,1600,1900,2200]` | 光圈档位 |
| `FOCUS_DIST_TABLE` | `[10,30,100,150,300,600,1000,2000,65535]` | 对焦距离档位 |
| `ZOOM_POS_MIN/MAX` | `10 / 230` | 变焦行程 |
| `LASER_MAX_ON_SEC` | `60` | **已废弃**（最终版取消了自动关光，见第 10、13 节） |
| `LASER_DEV_DEFAULT` | `/dev/ir_laser` | 激光串口 |

---

## 6. HTTP 接口

统一特征：JSON 响应为 UTF-8，带 `Access-Control-Allow-Origin: *`；支持 `OPTIONS`（204）；请求体为 JSON，用 `Content-Length` 读取。

> ⚠️ 实现上对 `Handler.do_GET/do_POST` 做了**三层追加补丁**（`_do_GET`→`_do_GET3`→`_do_GET5`，POST 同理），最终生效的是 `_do_GET5`/`_do_POST5`，它会先处理自己关心的路径，再回落到前一层。下表为**最终生效行为**。

### 6.1 GET

| 路径 | 返回 | 说明 |
|---|---|---|
| `/` 、 `/<file>` | 静态文件 | `web/` 下文件；`/` → `index.html`；带路径穿越防护 |
| `/api/status` | JSON | 相机状态 + `prog_tcp`(是否连上8888) + `recording` + `record_file` |
| `/api/events` | SSE 流 | 事件推送（见 6.3） |
| `/api/detect` | JSON | 读 `default.ini` 的检测/显示参数 |
| `/api/tcp` | JSON | **网关自带** TCP 服务器状态（`running/port/clients`）+ `prog_tcp` |
| `/api/tcpserver` | JSON | 同 `/api/tcp`（网关自带 TCP 服务器状态） |
| `/api/laser` | JSON | 激光状态 + 可直控性说明 |
| `/api/laser/status` | JSON | 激光详细状态（`opened/on/on_elapsed/last/exclusive/controllable`） |
| `/api/laser/query` | JSON | 依次查询 开关/亮度/行程位置/出光角度 的原始应答 |
| `/api/streams` | JSON | 代理 go2rtc `http://127.0.0.1:1984/api/streams`，用于前端判断可用流 |
| `/evidence/<name>` | JPG/MP4 | 取证文件下载（只取 `basename`） |
| `/api/file/<name>` | JPG/MP4 | 同上 |

`/api/status` 的相机字段：`zoom_pos`、`zoom_x`(=`zoom_pos/10`)、`iris_level`、`gain`、`ircut`、`focus_style`、`focus_limited`。

### 6.2 POST

| 路径 | 请求体 | 行为 |
|---|---|---|
| `/api/ptz` | `{pan,tilt,zoom,ms}` | 连续云台/变焦；`ms` 钳制 `50~5000`（默认 300），到时自动回零 |
| `/api/ptzstop` | — | 立即回零（等价 `ptz_move(0,0,0,0)`） |
| `/api/iris` | `{level}` | 先切 `manual` 曝光，再写 `IrisLevel`（钳制到 `IRIS_TABLE` 范围 `160~2200`） |
| `/api/gain` | `{level}` | 先切 `manual` 曝光，再写 `GainLevel`（钳制 `0~100`，`GainLimit=100`） |
| `/api/exposure` | `{mode:"auto"\|"manual"}` | 切换曝光模式 |
| `/api/focus` | `{dir:±1}` | 在 `FOCUS_DIST_TABLE` 中按当前档位 ±1 档，写 `focusLimited`，并确保 `focusStyle=MANUAL` |
| `/api/ircut` | `{mode:"day"\|"night"\|"auto"}` | 写 `IrcutFilterType`（非法值回落 `auto`） |
| `/api/snapshot` | — | 抓图 `channels/101/picture` → `evidence/snap_<时间戳>.jpg`，返回 `url` |
| `/api/record/start` | `{stream}` | 起 ffmpeg 录像（默认 `vis1080`）→ `evidence/remote_<stream>_<时间戳>.mp4`；已在录则报错 |
| `/api/record/stop` | — | `SIGINT` 优雅收尾 mp4（8s 超时后 `kill`） |
| `/api/progcmd` | `{line}` | 把指令转发给 Qt 程序 TCP 8888（正则校验，见 9.2） |
| `/api/detect` | 检测参数字典 | 写回 `default.ini` |
| `/api/tcp` | `{line}` | 同 `/api/progcmd`（转发到 8888） |
| `/api/laser/open` | `{dev?}` | 打开激光串口（默认 `/dev/ir_laser`，9600 8N1 原始模式） |
| `/api/laser/close` | — | 先关光再关闭串口 |
| `/api/laser/on` | `{}` | **开光（最终版：无需 `confirm`，不自动关光）** |
| `/api/laser/off` | — | 关光 |
| `/api/laser/brightness` | `{level}` 或 `{step}` | 给定 `level` 则直接设置(0~255)；否则按 `step` 上/下步进 |
| `/api/laser/spot` | `{stop}` 或 `{dir,speed}` | 光斑移动/停止 |
| `/api/laser/angle` | `{dir,step}` | 出光角度调整（`step` 钳制 `1~255`，默认 8） |
| `/api/laser/position` | `{pos}` | 行程定位（钳制 `0~0x4000`） |
| `/api/laser/reset` | — | 复位 |
| `/api/laser/query` | — | 同 GET |
| `/api/tcpserver/start` | `{port?}` | 启动网关自带 TCP 服务器（默认 8888） |
| `/api/tcpserver/stop` | — | 停止网关自带 TCP 服务器 |
| `/api/report_detect` | 任意字典 | 网页上报检测结果 → 缓存为 `_last_angle` 并**广播给 TCP 客户端**（`type=angle_data`） |

未知 POST 路径 → `{"ok":false,"detail":"unknown api"}` + 404。

### 6.3 SSE 事件（`GET /api/events`）

| 事件 | 说明 |
|---|---|
| `{"type":"tcp","connected":true/false}` | 与 Qt 程序 8888 的连接状态变化 |
| `{"type":"prog","data":{...}}` | 从 8888 收到的一行（JSON 解析成功则为对象，否则 `{"type":"raw","text":...}`） |
| `: connected` / `: hb` | 注释行；连接建立与每 10s 心跳 |

---

## 7. 网关自带 TCP 控制服务器（默认 8888）

由网页按钮启停（`/api/tcpserver/start|stop`），**与 Qt 程序自己的 8888 互斥**（同一端口不能同时监听）。

协议：一行一条文本命令，回车换行结束；服务端对每条命令回一行 JSON。

**欢迎语与用法**（连接后立即下发）：

```json
{"type":"welcome","message":"SubseaImagingSystem TCP Server (gateway)",
 "timestamp":1730000000000,
 "usage":"zoom +/- | focus +/- | iris +/- | ptz up|down|left|right [ms] | stop | status"}
```

若已有角度缓存，随后会再补发一条上次的 `angle_data`。

**支持命令**：

| 命令 | 说明 |
|---|---|
| `help` / `?` | 返回用法 |
| `status` | 返回相机状态（含 `zoom_pos/zoom_x/iris_level/...`） |
| `zoom +` / `zoom -` | 变焦步进：以速度 ±40 持续 800ms 后自动回零 |
| `focus +` / `focus -` | 对焦档位 ±1（`FOCUS_DIST_TABLE`） |
| `iris +` / `iris -` | 光圈档位 ∓1（注意：`+` 使 `IRIS_TABLE` 索引 `-1`） |
| `ptz up\|down\|left\|right [ms]` | 云台点动，速度 40，`ms` 钳制 `50~5000`（默认 300） |
| `stop` | 云台立刻回零 |
| 其它 | `{"result":"unknown-cmd"}` |

应答格式：

```json
{"type":"ack","cmd":"ptz up 300","result":"ok","timestamp":1730000000000}
```

---

## 8. 与本地 Qt 程序（TCP 8888）的桥接

后台线程 `prog_tcp_worker` 常驻：

- 持续尝试连接 `127.0.0.1:8888`（`PROG_TCP` 硬编码）；连不上就每 3s 重试，**自动降级**，不影响网页其它功能。
- 连上后按行读，JSON 解析成功 → SSE `{"type":"prog","data":{...}}`；失败 → `{"type":"raw","text":...}`。
- 连接状态变化 → SSE `{"type":"tcp","connected":...}`。
- 反向：`/api/progcmd` 与 `/api/tcp` 可把文本指令发给 8888，正则校验 `^[a-z]+( [+\-a-z0-9]+)*$`（忽略大小写），返回程序应答。

> 注意：这里固定连 8888，与 `default.ini` 里的 `tcp_server_port` 无关（后者只被已被覆盖的旧逻辑使用）。

---

## 9. 红外激光直控（Pelco_D）

### 9.1 串口

`/dev/ir_laser`，原始模式，**9600 bps，8N1**（`iflag=oflag=lflag=0`，`CLOCAL|CREAD|CS8`，`VMIN=0/VTIME=5`）。

### 9.2 Pelco_D 帧格式

```text
0xFF  addr  cmd1  cmd2  data1  data2  checksum
checksum = (addr + cmd1 + cmd2 + data1 + data2) & 0xFF     # addr 默认 0x01
```

### 9.3 命令表（`c1, c2`）

| 功能 | cmd1 | cmd2 | data1 | data2 | 接口 |
|---|---|---|---|---|---|
| 关光 | `0x01` | `0x01` | `0` | `0` | `/api/laser/off` |
| 开光 | `0x01` | `0x01` | `1` | `0` | `/api/laser/on` |
| 亮度步进 | `0x01` | `0x02` | `0`(升)/`1`(降) | `0` | `/api/laser/brightness {step}` |
| 亮度直设 | `0x01` | `0x03` | `level(0~255)` | `0` | `/api/laser/brightness {level}` |
| 角度调整 | `0x01` | `0x04` | `1`(正)/`0`(负) | `step` | `/api/laser/angle` |
| 行程定位 | `0x01` | `0x05` | `pos>>8` | `pos&0xFF` | `/api/laser/position` |
| 复位 | `0x01` | `0x06` | `0` | `0` | `/api/laser/reset` |
| 光斑移动 | `0x00` | `0x20`(正)/`0x40`(负) | `0` | `speed` | `/api/laser/spot` |
| 光斑停止 | `0x00` | `0x00` | `0` | `0` | `/api/laser/spot {stop:true}` |
| 查询 开关 | `0x02` | `0x01` | `0` | `0` | `/api/laser/query` |
| 查询 亮度 | `0x02` | `0x03` | `0` | `0` | 同上 |
| 查询 位置 | `0x02` | `0x05` | `0` | `0` | 同上 |
| 查询 角度 | `0x09` | `0x01` | `0` | `0` | 同上 |

说明：

- 光斑角度范围约 **2°~65°**。
- 发送后可按 `read_ms` 尝试读回，返回十六进制字符串；**读回失败不影响下发**。
- 串口被 Qt 程序独占时，`laser_state()` 报 `exclusive=true, controllable=false`，网页只显示说明、拒绝直控。
- `LASER_MAX_ON_SEC=60` 与 `_laser_autooff_disabled()` 均为**已废弃占位**：最终版 `/api/laser/on` 不再自动关光、也无需二次确认。**请人工确认后再开光。**

---

## 10. 视频链路（go2rtc + tc.sh）

### 10.1 go2rtc 端口

| 端口 | 协议 | 说明 |
|---|---|---|
| `1984` | HTTP API | 流列表/控制；网关 `/api/streams` 即代理此处 |
| `8554` | RTSP | 录像取流出口（`GO2RTC_RTSP`） |
| `8555` | WebRTC | `candidates: 192.168.10.165:8555` |

### 10.2 `go2rtc.yaml` 流定义

| 流名 | 来源 | 编码 |
|---|---|---|
| `vis1080` | 可见光 `Channels/103` | H.265 直转（延时最低，需 HEVC 客户端；Firefox 不支持） |
| `vis4k` | 可见光 `Channels/101` | H.265 直转 |
| `vis102` | 可见光 `Channels/102` | H.265 直转 |
| `thermal` | 热像仪 `192.168.10.211` `Channels/101` | H.265 直转 |
| `vis1080h264` | `Channels/103` → Jetson NVENC | H.265→H.264 硬转（通用浏览器可直解，按需拉起） |
| `vis4kh264` | `Channels/101` → Jetson NVENC | 同上（16 Mbps） |

可见光相机：`admin:cisdi135@192.168.10.212:554`。

### 10.3 `tc.sh`

手动硬转码脚本（调试用），H.265→H.264 后走 UDP：

```bash
./tc.sh 1080     # Channels/103 -> udp://127.0.0.1:5556  4 Mbps
./tc.sh 4k       # Channels/101 -> udp://127.0.0.1:5557  16 Mbps
```

---

## 11. 文件命名与取证

| 类型 | 路径 | 命名 |
|---|---|---|
| 抓图 | `/home/lcfc/evidence/` | `snap_YYYYmmdd_HHMMSS.jpg` |
| 录像 | `/home/lcfc/evidence/` | `remote_<stream>_YYYYmmdd_HHMMSS.mp4` |

- 抓图后返回 `{"ok":true,"file":...,"bytes":...,"url":"/evidence/<file>"}`。
- 录像用 `-movflags +faststart`，停止时以 `SIGINT` 让 ffmpeg 正常收尾（保证 mp4 moov 完整）。
- 同一时刻只允许一路录像（全局 `_rec`）。

---

## 12. 快速自检清单

```bash
# 1) 服务与端口
systemctl is-active subsea-console.service
ss -ltnp | grep -E ':8080|:1984|:8554|:8555'

# 2) 页面与接口
python3 - <<'PY'
import urllib.request, json
for p in ("/api/status", "/api/detect", "/api/laser", "/api/tcp", "/api/streams"):
    try:
        print(p, urllib.request.urlopen("http://127.0.0.1:8080"+p, timeout=6).read()[:200])
    except Exception as e:
        print(p, "ERR", e)
PY

# 3) 相机连通性（ISAPI）
python3 - <<'PY'
import urllib.request
print(urllib.request.urlopen("http://192.168.10.212/ISAPI/System/deviceInfo", timeout=5).status)
PY

# 4) 串口占用
fuser -v /dev/ir_laser 2>/dev/null || echo "串口空闲(或无权查看)"
```

---

## 13. 注意事项与已知问题（重要）

1. **8080 端口冲突**：另有一个独立项目「SOP 装配检测系统」的 C++ 后端（`/home/tianheng/project/sop`）默认也想监听 `0.0.0.0:8080` 并托管 `sop/web/dist`，其日志出现过 `bind(0.0.0.0:8080) failed: Address already in use`。**两者不能同时用 8080**，部署前先 `ss -ltnp | grep :8080`。
2. **8080 现在归本网关**：当前由 `subsea-console.service`（Python）占用，而不是 SOP 后端，也不是 Qt 主程序。
3. **激光串口独占**：`/dev/ir_laser` 若被 Qt 程序打开，网关只能报告状态、不能控制（`controllable=false`）。要远程控激光，需在 Qt 程序侧加 TCP 命令（Pelco_D 命令表已具备，约 60 行工作量）。
4. **开光安全**：最终版 `/api/laser/on` **已取消二次确认与自动关光**。这是有意为之，但意味着存在误操作风险，操作前请人工确认。相关常量/函数仅作占位保留。
5. **相机凭据来源**：`default.ini` 缺失或读失败时，会**静默回退到代码内写死的默认账号**（含明文密码）。修改相机密码时务必同步该 ini。
6. **`default.ini` 写入即改 Qt 程序配置**：`/api/detect` 直接按行改写该文件（保留注释行），**需重启 Qt 程序生效**。
7. **配置读取时机**：相机 IP/账号在进程启动时读取一次（模块级 `load_camera_cfg()`），改 ini 后需 `systemctl restart subsea-console.service`。
8. **`/evidence` 与 `/api/file` 无鉴权**：仅取 `basename` 防穿越，但**没有访问控制**，任何能访问 8080 的机器都能下载取证文件。不要暴露到不可信网络。
9. **多层补丁的坑**：改接口时要注意 `_do_GET/_do_GET3/_do_GET5` 三层叠加（POST 同理），新路径若在高层拦截会「遮蔽」低层同名逻辑。
10. **无日志落盘**：`log_message` 被静默，排查问题主要靠 `journalctl` 的启动 3 行 + 前端行为，建议排障时临时打开日志。

---

## 14. 版本备份文件

部署目录内保留了多份历史备份，**回滚或比对时可直接参考**：

| 文件 | 备注 |
|---|---|
| `gateway.py.bak` / `.bak2` / `.bak3` / `.v7.bak` | 主程序历史版本 |
| `web/index.html.v1.bak` … `v14.bak` | 前端历史版本（`v8` 状态联动前 / `v9` 温度条修复前 / `v10` 鼠标探针前 / `v11` 六路码流前 / `v12` 帮助整合前 / `v13` VLC 参数前 / `v14` Jetson 看 4K 说明前；当前 `index.html` 为 v15） |
| `go2rtc.yaml.bak` … `bak6` | go2rtc 配置历史版本（`bak6` = 六路码流改造前） |
| `go2rtc.yaml.bak` … `bak4` | go2rtc 配置历史版本 |

---

## 15. 前端状态联动优化（v9）

### 15.1 优化前的问题

1. **按钮与状态不同步**：`连接热像仪` / `连接可见光` 即使已连接仍可点击（会不断新建 `RTCPeerConnection` 而不释放旧连接）；`断开` 在未连接时也可点击。
2. **状态反馈滞后/不准**：`状态` 只在 2s 轮询里按 `PC.thermal` 对象真值判断，链路掉线后仍显示「已连接」（对象还在，但连接已失败）。
3. **录像按钮逻辑错误（严重）**：`toggleRec()` 用 `document.getElementById('bRec').classList.contains('active')` 判断是否在录像，而 `bRec` 属于已移除控件、被替换为**哑对象**（`contains()` 恒返回 `false`）。于是按钮显示「结束录像」时点下去实际再次调用 `record/start`，后端回「已在录像中」，**无法停机**。
4. **重复点击无防重入**：连接/断开、TCP 启停、激光串口连接都会重复下发。
5. **断线无感知**：go2rtc 重启或码流中断时界面不会回退到「未连接」，按钮也不会重新可用。
6. **断开后残留**：热像仪/可见光断开后，画面角标、状态栏、检测结果仍显示旧值。

### 15.2 统一状态模型（唯一数据源）

```js
var LINK = { thermal:{state:'idle'}, visible:{state:'idle'} };
// state: idle | connecting | connected | failed | disconnected   （=通过 PC 对象真值推断状态）
var CAM_OK = null;                 // 相机 ISAPI 可达性
var REC = {on:false, busy:false};  // 录像真实状态 + 操作中
var TCPBUSY = false;               // TCP 启停操作中
```

所有状态变化只经 `renderThermalLink() / renderVisibleLink() / renderRec() / renderDetectRun()` 刷新界面，保证「按钮使能 + 状态文案 + 画面清理」三者一致。

### 15.3 每个连接模块统一的联动规则

| 状态 | 连接按钮 | 断开按钮 | 状态文案 | 画面/信息 |
|---|---|---|---|---|
| `idle` 未连接 | 使能（「连接XX」） | 禁用 | 未连接 | 角标“未连接”，信息清空 |
| `connecting` 连接中 | 禁用（「连接中…」） | 禁用 | 连接中… | 角标“连接中…” |
| `connected` 已连接 | 禁用（「已连接」） | 使能 | 已连接(·码流) | 正常显示 |
| `failed` 失败 | 使能（可重试） | 禁用 | 连接失败 | 角标“连接失败(点…重试)” |

- 真实状态由 `pc.onconnectionstatechange` 驱动（`attachPc`）：`failed`/`disconnected` 自动回退为可重连；`closed` 复位为 `idle`。
- 释放统一走 `dropPc(kind)`：先摘回调再 `close()`，避免残留回调与连接泄漏。
- 切换码流先 `dropPc` 再建新连接，并用 `CUR` 校验丢弃过期协商结果。

### 15.4 其他同步修复

- **录像**：`toggleRec()` 改以 `REC.on`（由 `/api/status` 每 2s 回填）判定，并用 `REC.busy` 防重入；禁用/文案/绿色高亮由 `renderRec()` 统一维护。
- **TCP 服务器**：启动/停止增加 `TCPBUSY` 防重入与「启动中…/停止中…」；`监听中`、`网关桥接` 用颜色区分（`已启动/未启动`、`已连接/未连接`）。
- **激光串口**：`LS.busy` 防重入，连接中显示「连接中…」，操作期间锁定控制按钮。
- **检测联动**：新增 `#d-run` 提示 —— 热像仪未连接显示「检测暂停」；关闭检测显示「已关闭」；运行显示「检测运行中(10Hz)」。底部状态栏「检测」同步为「热像仪未连接 / 已关闭 / N 候选」。
- **配色**：新增 `.st-ok/.st-bad/.st-wait`，状态文案随状态变色。

### 15.5 回归自检要点

- 点「连接热像仪」→ 按钮立即变「连接中…」且禁用，「断开」保持禁用；成功后连接按钮禁用、「断开」使能、状态「已连接」。
- 点「断开」→ 恢复初始态，角标与检测结果清空。
- 重启/断开 go2rtc → 状态自动变「连接失败/已断开」，连接按钮重新可用。
- 录像：点「开始录像」→「处理中…」→「结束录像」；再点能正确停止（**修复前此处失效**）。

---

## 16. 温度条误识别修复 + 鼠标探针增强（v10 / v11）

### 16.1 现象与量化定位

**现象**：热像仪画面最右侧有一条"由浅到深"的温度-亮度标定色条（相机 OSD 烧录在视频里）。Web 端把它当成一个独立热源识别出来。

**实测定位**（用 ffmpeg 从 go2rtc 抓一帧，按列统计亮度）：

```bash
ffmpeg -hide_banner -hide_banner -rtsp_transport tcp -i rtsp://127.0.0.1:8554/thermal \
       -frames:v 1 -pix_fmt gray -f rawvideo -y /tmp/th.raw     # 640x512
```

| 列范围 | > 44°C 阈值的像素数 | 结论 |
|---|---|---|
| x = 617 … 638 | 每列约 197 / 512 | **温度条**（整列高亮） |
| x = 639 | 0 | 外侧暗边 |

复刻 Qt 的算法（阈值 44°C、minArea 50、open(2)/close(1)、4 邻域）验证：

| 条件 | 识别结果 |
|---|---|
| 不屏蔽 | 真实热源 `box=(100,274,148,149)` 面积 13823；**温度条** `box=(617,73,21,198)` 面积 4146 |
| 屏蔽最右 24px | 仅剩真实热源，**温度条消失** |

> 关键结论：Qt 与 Web 的**算法完全相同**（同样的二值化 / open2 / close1 / 连通域），Qt 侧源码中也**没有**任何裁剪或屏蔽。两者共用同一路 `Channels/101` 视频，因此该色条对两者都存在。差异属于渲染/透明度层面的观感，修复办法是**在进入连通域之前把色条所在的列整体屏蔽**。

### 16.2 修复实现（Web）

- 新增 `BAR_MASK_PX`（默认 **24**）：在 [`detect()`](../../../lcfc/remote/web/index.html:530) 中**二值化后**与**形态学后**各屏蔽一次最右 `BAR_MASK_PX` 列（先屏蔽是防止与热源被开/闭运算"连成一片"，后屏蔽是修正膨胀越界回收）。
- 支持现场微调：URL 加 `?barmask=32` 调整宽度，`?barmask=0` 关闭屏蔽（便于对比验证）。
- **可视化**：叠加层在该区域画半透明橙罩 + 虚线边界，明确提示"此区不参与检测"。
- **可核查**：检测结果分组新增 `#d-mask` 文案，显示当前屏蔽宽度与开关方式。

### 16.3 右上角显示修正

- 原 `640x512 50fps` 角标与画布上的"检测到热源目标"状态条**位置重叠**，已按要求去掉：
  - `updateBars()` 不再向 `b-thermal` 写分辨率/帧率（仅缓存到 `LAST_TH_STATS`）；
  - 新增 CSS `.badge:empty{display:none}`，已连接时角标为空即不占位，右上角只保留检测状态；
  - 底部状态栏「热像仪」由分辨率改为**链路状态**（未连接/连接中/已连接），
    分辨率仍然体现在左栏标题「热源监测图像 640×512」。

### 16.4 Qt 端：鼠标探针增加灰度值

Qt 原本已有鼠标探针（`thermalInfoBox` 内一行 + 画面十字浮层），本次补充**原始灰度值**：

| 位置 | 改前 | 改后 |
|---|---|---|
| 信息栏标签 | `坐标: (x,y)   温度: t°C` | `坐标: (x,y)   灰度: g   温度: t°C` |
| 画面浮层 | `(x,y) t°C` 单行 | 两行：`(x,y)` / `灰度 g   温度 t°C` |
| 离开画面/越界 | `坐标: --   温度: --` | `坐标: --   灰度: --   温度: --` |
| 左下角提示 | 滚轮/拖拽/双击 | 追加「鼠标移动查看坐标·灰度·温度」 |

代码改动点：

- [`thermalGrayAt()`](../../../lcfc/remote/src/mainwindow.cpp:2139)：新增，返回 `qGray()`（即检测所用的 `(r*11+g*16+b*5)/32`），越界或未连接返回 `-1`。
- [`thermalTempAt()`](../../../lcfc/remote/src/mainwindow.cpp:2152)：改为复用 `thermalGrayAt()`，公式不变（`-20 + g/255*170`）。
- [`updateThermalProbe()`](../../../lcfc/remote/src/mainwindow.cpp:2165)：同时记录 `m_probeGray` / `m_probeTemp` 并刷新标签。
- [`renderThermal()`](../../../lcfc/remote/src/mainwindow.cpp:631)：探针浮层改为两行文本框（按两行最长文本自动配宽）。
- 头文件 [`mainwindow.h`](../../../lcfc/remote/src/mainwindow.h:129)：新增 `thermalGrayAt()` 声明与 `m_probeGray` 成员。

> 灰度与温度的对应关系（与检测、与 Qt 原逻辑一致）：`温度 = -20 + 灰度/255 × 170`，即灰度 0 ↔ -20°C，灰度 255 ↔ 150°C。故灰度是第一手数据，温度是线性换算值。

### 16.5 回归自检

- Web：右上角只剩一个状态条（"检测到/未检测到热源目标"），不再出现 `640x512 50fps`；最右侧有色条时有淡橙遮罩与虚线，检测结果里只有真实热源。
- Web：`?barmask=0` → 色条重新被识别为热源（可复现原问题）；`?barmask=24` → 恢复正常。
- Qt：鼠标在热像仪画面上移动 → 信息栏与浮层同时显示坐标、灰度、温度；移出画面后恢复 `--`。

---

### 16.6 Web 端鼠标探针：坐标 + 温度（v11）

与 Qt 对齐，Web 端热像仪画面也支持鼠标探针，**仅在鼠标位于画面内时显示，移出即隐藏**。

| 位置 | 内容 |
|---|---|
| 画面内浮层（跟随鼠标） | 黄色十字 + `(x, y)  t°C` 文本框 |
| 左栏「热源监测图像」信息框 | 新增一行「鼠标坐标 / 温度」，未悬停时显示 `--` |
| 鼠标指针 | `crosshair` |

实现要点（均在 [`web/index.html`](../../../lcfc/remote/web/index.html)）：

1. **黑边换算**：画面按 `object-fit: contain` 显示，可能上下或左右留黑边。`thermalContainRect()` 依据
   `videoWidth/videoHeight` 与控件 `getBoundingClientRect()` 算出实际显示矩形，再换算到**源像素坐标**；
   落在黑边内即视为"移出画面"并隐藏。
2. **取像素**：用独立 **1×1 画布** `drawImage(v, ix, iy, 1, 1, 0, 0, 1, 1)` 只取该点，
   再 `getImageData(0,0,1,1)`。好处：不依赖检测是否开启、不整帧重绘、开销恒定。
3. **数值口径与检测完全一致**：灰度 `g = (r*11 + g*16 + b*5) >> 5`，
   温度 `-20 + g/255 × 170`（与 [`detect()`](../../../lcfc/remote/web/index.html:530) 同源）。
4. **事件绑定**：`#v-thermal` 上挂 `mousemove` / `mouseleave`。
   叠加层 `pointer-events:none`，事件可正常落到 `<video>` 上。
5. **重绘节流**：探针绘制节流到 ~20fps（与 Qt 的 `m_lastProbeRenderMs` 50ms 策略一致），
   不额外增加检测负担。
6. **检测关闭时仍可用**：`runDetection()` 在 `DET.enable=false` 的早退分支里也会重绘叠加层，
   否则探针与网格在"关闭检测"后无法刷新。
7. **断开联动**：热像仪断开时（`renderThermalLink()`）同步清空探针状态与标签。

> 说明：按需求 Web 探针显示"坐标 + 温度"（灰色值口径已内含于温度换算）。
> 若希望与 Qt 一致地在浮层里同时显示原始灰度值，只需在 `ptxt` 拼接处加一个灰度字段即可。

### 16.7 回归自检（v11 增补）

- 鼠标在热像仪画面内移动 → 出现黄色十字与 `(x,y) t°C`，左栏同步刷新；移出画面 → 立即消失并回到 `--`。
- 画面带黑边（窗口比例不匹配）时，鼠标在黑边区域**不应**显示数值。
- 关闭「启用检测」后，探针仍能正常显示（叠加层仍会重绘）。
- 断开热像仪 → 探针消失，标签回到 `--`。

---

## 17. 六路码流（直转优先 + 转码兜底）与客户端 HEVC 自检（v12）

### 17.1 背景：为什么之前必须转码

实测相机（海康 iDS-2ZMN2312S）三路源**全是 H.265(HEVC)**：

| 通道 | 用途 | 编码 | 分辨率 | 帧率 |
|---|---|---|---|---|
| 101 | 主码流 | **H.265** | 3840×2160 | 25 |
| 103 | 第三码流 | **H.265** | 1920×1080 | 25 |
| 102 | 子码流 | **H.265** | 704×576 | 25 |
| — | 热像仪 | H.264 | 640×512 | 50 |

而现场客户端（Edge 153 / Windows x64）的 SDP 只声明 `video:VP8, video:VP9, video:H264, video:AV1`，
go2rtc 日志因此反复报：

```text
WRN error="streams: codecs not matched:
        video:H265, audio:PCMA => video:VP8, video:VP9, video:H264, video:AV1"
```

→ **直转 H.265 到该客户端会协商失败（黑屏）**，这正是当初不得不加 `vis1080h264` 转码流的原因。

**开销实测对比（同一台 Jetson Xavier NX）**

| 场景 | go2rtc 自身 CPU | 额外子进程 | 编解码核 |
|---|---|---|---|
| 空载基线 | 8% | 12.5%（103 转码） | NVDEC |
| **纯转发 4K H.265（vis4k）** | **22%** | **0** | **完全不占用** |
| 转码 4K（vis4kh264） | 14% | **35.8%**（4K NVDEC+4K NVENC） | NVDEC + NVENC |

→ 直转省掉约 0.5 个核与**全部编解码核**，并把切换出画时间从 1~3 秒降到近乎瞬时。

### 17.2 go2rtc 流布局（现为 6 路可见光 + 1 路热像仪）

| 分组 | 流名 | 来源 | 输出编码 | 说明 |
|---|---|---|---|---|
| ① 直转 | `vis4k` | ch101 | H.265 | 4K，服务端零编解码；**需客户端 HEVC 硬解** |
| ① 直转 | `vis1080` | ch103 | H.265 | 1080p 直转 |
| ① 直转 | `vis102` | ch102 | H.265 | 704×576 直转 |
| ② 转码 | `vis4kh264` | ch101 | H.264 16 Mbps | 最重，仅必要时候选 |
| ② 转码 | `vis1080h264` | ch103 | H.264 4 Mbps | 默认项 |
| ② 转码 | `vis102h264` | ch102 | H.264 2 Mbps | **本次新增** |
| — | `thermal` | 热像仪 ch101 | H.264 | 本身即 H.264，直转无需 HEVC |

转码流均为 `exec:gst-launch ... nvv4l2decoder ! nvvidconv ! nvv4l2h264enc ! mpegtsmux ! fdsink`，
由 go2rtc **按需拉起**（无人观看不占 NVENC/CPU）。

### 17.3 网页下拉：6 项（两组）

```text
① 直转 · 服务端零编解码(需客户端 HEVC 硬解)
   主码流 4K 3840x2160 · H.265 直转
   第三码流 1080p · H.265 直转
   子码流 704x576 · H.265 直转
② 通用 · 服务端转 H.264(兼容所有浏览器)
   主码流 4K 3840x2160 · H.264 转码
   第三码流 1080p · H.264 转码      (默认选中)
   子码流 704x576 · H.264 转码
```

### 17.4 客户端 HEVC 自检（不支持则禁用并说明原因）

页面加载时执行 `detectHevcCapability()`，**双判**：

1. **主判**：`RTCRtpReceiver.getCapabilities('video').codecs` 是否含 `video/H265`
   —— 这直接决定 **WebRTC 直转** 是否可行（Chromium 仅在本机具备 HEVC 硬解时才列出）。
2. **辅判**：`video.canPlayType('video/mp4; codecs="hvc1…"')` 与 `MediaSource.isTypeSupported(...)`
   —— 用于区分"压根没硬解"还是"有硬解但 WebRTC 未开放"。

界面表现：

| 检测结果 | `客户端 HEVC 直转` | 直转 3 项 | 提示（`#hevcHint`） |
|---|---|---|---|
| 支持 | `支持 · 可选直转(零转码)`（绿） | 可选 | 显示检测到的 WebRTC 编码列表 + 4K 直转对客户端要求最高 |
| 有 HEVC 但 WebRTC 未开放 | `不支持 · 直转项已禁用`（红） | 禁用并加 `✕` 前缀 | 说明属"浏览器未在 WebRTC 中开放 H.265"，建议换新版 Edge/Chrome 或选通用项 |
| 无 HEVC 硬解 | 同上 | 禁用并加 `✕` 前缀 | 提示可到 Microsoft Store 安装「**HEVC 视频扩展**」或换支持 HEVC 硬解的设备，否则请选「通用(H.264)」 |

### 17.5 三重防呆

1. **选项层**：不支持时把 3 个直转 `<option>` 置 `disabled`（无法选中）。
2. **调用层**：`setStream()` 入口再判一次，若目标为直转流且无 HEVC 能力 → **拒绝并自动切到同档 H.264**（`vis4k→vis4kh264`、`vis1080→vis1080h264`、`vis102→vis102h264`），日志写明原因。
3. **运行层**：即便协商通过，若直转流实际连接失败（`catch`），也会**自动回落**到同档 H.264 兜底流。

默认码流：支持 HEVC 用 `vis1080`（直转，服务端零编解码）；不支持则用 `vis1080h264`（保证一定出画）。

### 17.6 运维提示

- 让客户端支持 HEVC 直转：Edge/Chrome 安装 **Microsoft Store「HEVC 视频扩展」**，并确认显卡支持 HEVC 硬解；Firefox 不支持。
- 若希望**彻底零转码且全平台兼容**（不依赖客户端硬解），最优解是**把相机侧 102/103 的编码改成 H.264**（相机自带编码器，不占 Jetson 资源），改完即可删掉 3 条转码流。此项需现场确认后执行。
- 播放 4K 直转（`vis4k`）时，客户端需同时具备 **4K HEVC 解码 + 4K 渲染**能力，否则仍可能卡顿。

### 17.7 回归自检（v12）

- 在不支持 HEVC 的浏览器打开 → 「客户端 HEVC 直转」显示红字 `不支持 · 直转项已禁用`，3 个直转项灰掉且带 `✕`，提示里给出安装「HEVC 视频扩展」的建议；默认播放 1080p H.264。
- 在支持 HEVC 的浏览器（Chrome/Edge + 硬解）打开 → 显示绿字 `支持 · 可选直转(零转码)`，可自由选择 6 项；选 `vis1080`/`vis4k` 后**服务端不启动任何 gst-launch 转码进程**（可用 `ps -eo cmd | grep gst-launch` 验证）。
- 手动把某直转流改坏（如填错 URL）→ 页面自动回落并提示。
- 页面内「Windows 客户端如何开启 HEVC 硬解直转」折叠帮助在检测到不支持时**自动展开**。

### 17.8 Windows 客户端开启 HEVC 硬解（直转前置条件）

> 该步骤已内置到页面（`#hevcHelp` 折叠块，检测到不支持时自动展开），现场可直接照着做。

Chromium 系（Chrome/Edge）**只在"本机具备 HEVC 硬件解码 + 系统 HEVC 解码器已安装"** 时，
才会在 `RTCRtpReceiver.getCapabilities('video')` 里列出 `video/H265`；二者缺一，直转路必黑屏。
（Edge 153 / Win 实测编解码列表为 `VP8/rtx/VP9/H264/AV1/red/ulpfec/flexfec-03`，**没有 H265**。）

| 步骤 | 操作 | 检查点 |
|---|---|---|
| 1 | 确认硬件支持 | 任务管理器 → 性能 → GPU 有无 “Video Decode” 引擎；或 `dxdiag` 看显卡型号。Intel 6 代(Skylake, 2015)/NVIDIA Pascal(GTX10)/AMD Vega 及以上支持 HEVC 硬解 |
| 2 | 安装系统级 HEVC 解码器 | Microsoft Store 搜 “HEVC” → 「HEVC 视频扩展」(约 $0.99) 或「HEVC 视频扩展(来自设备制造商)」(OEM 免费)；命令行可用 `winget search HEVC` 定位 |
| 3 | 打开浏览器硬件加速 | `edge://settings/system` → 使用硬件加速 = 开 |
| 4 | 如需 | `edge://flags` 搜 HEVC，若有相关项设为 Enabled |
| 5 | **彻底重启浏览器** | 任务管理器结束所有 `msedge.exe`（仅关闭窗口不够） |
| 6 | 验证 | `edge://gpu` → “Video Acceleration Information” 应出现 `hevc main / main10`；刷新本页状态行应变绿 |
| 7 | 控制台自检 | F12：`RTCRtpReceiver.getCapabilities('video').codecs.map(c=>c.mimeType)`，含 `video/H265` 即为支持 |

**仍不支持时的三个根因与对策**

1. **远程桌面(RDP)/虚拟机会话**：浏览器拿不到物理 GPU 的解码能力 → 请在**物理机**上直接打开浏览器。
2. **显卡/驱动确实不支持 HEVC**（老核显、无独显的瘦客户机）→ 只能走②「通用(H.264)」转码流，或换设备。
3. **Chromium 不支持软件 HEVC 直转**：无法通过设置强制，别在客户端折腾。

**服务端根治（推荐，一劳永逸）**：把相机 **102/103 的 `videoCodecType` 改为 H.264**
（相机自带编码器，不占 Jetson 任何资源），改完即可删掉 3 条转码流，
则任何客户端（含不支持 HEVC 的机器）都能直转，服务端零编解码。

---

## 18. Jetson 安装 VLC + 直转码流地址 + 帮助整合（v13）

### 18.1 Jetson 上安装与配置 VLC

```bash
sudo apt-get update && sudo apt-get install -y vlc         # 已装: VLC 3.0.9.2 Vetinari
```

**必须做的一处规避（重要）**：VLC 3.0 自带的 `omxil`(OpenMAX) 解码模块在 Jetson 上会
在枚举 NVIDIA OMX 组件时 **`Bus error` 崩溃**（实测 `exit=135`，日志停在
`OMX.Nvidia.h265.decode`）。规避方式：强制使用 `avcodec` 解码，写入
`~/.config/vlc/vlcrc`：

```ini
# 规避 Jetson 上 VLC 的 omxil 模块崩溃(Bus error): 强制使用 avcodec 解码
text-codec=left
codec=avcodec
```

改动后清一次插件缓存 `rm -rf ~/.cache/vlc`。**不修改任何系统文件**（`libomxil_plugin.so` 保持原样）。

### 18.2 VLC 实测数据（Jetson Xavier NX，软解）

| 流 | 结果 | VLC 单核占用(单核=100%) |
|---|---|---|
| `vis102` (HEVC 704×576) | ✅ 正常，ffprobe 校验 `hevc,704,576` | **≈ 24%** |
| `vis1080` (HEVC 1080p) | ✅ 正常 | **≈ 62%** |
| `vis4k` (HEVC 4K) | ✅ 可解 | **≈ 307%**（约 3 个核，偏重） |
| `vis1080h264` (H.264 1080p) | ✅ 正常，ffprobe 校验 `h264,1920,1080` | ≈ 48% |
| 相机直连 `Channels/102` | ✅ 正常（绕过 go2rtc） | — |

> 结论：**Jetson 上 VLC 看 1080p 及以下很轻松；4K 软解要约 3 个核**，建议 4K 优先用网页（浏览器硬解）或 H.264 转码流。
> 另外 VLC 用 `-I dummy` 无界面播放时退出会等待超时（`exit=124`，属 dummy 接口的已知行为），GUI 使用不受影响。

### 18.3 网页新增「直转码流地址」帮助块

可见光页「连接与码流」分组下新增折叠块 **「直转码流地址 (VLC/mpv 直接打开)」**，
主机名按当前访问地址动态生成（`location.hostname`），每条地址带 **复制**按钮：

| 分组 | 地址 |
|---|---|
| ① 经 go2rtc 转发（推荐） | `rtsp://<本机IP>:8554/vis4k`、`/vis1080`、`/vis102`（H.265 直转）；`/vis4kh264`、`/vis1080h264`、`/vis102h264`（H.264 转码）；`/thermal` |
| ② 相机直连（绕过 go2rtc） | `rtsp://admin:***@192.168.10.212:554/Streaming/Channels/101\|103\|102`；热像仪 `...@192.168.10.211:554/Streaming/Channels/101` |
| ③ 用法 | Jetson：`vlc rtsp://<IP>:8554/vis1080`；Windows：VLC → 打开网络串流（**VLC 自带 HEVC 解码，无需装 HEVC 扩展**）；无界面录像：`cvlc -I dummy --run-time=60 <url> --sout '#std{access=file,mux=ts,dst=/tmp/a.ts}'` |

### 18.4 帮助内容合并为一个折叠控件（v16）

原页面上的两段可折叠内容已**合并为一个**帮助控件，**默认折叠(隐藏)**，点击标题才展开：

- **状态行**（保留在页面上，一眼可见）：`客户端 HEVC 直转` = `支持 · 可选直转(零转码)` / `不支持 · 直转项已禁用`；
- **统一帮助块 `#helpBox`**：`<summary id="helpSummary">` 文案随能力变化 —— 可用时
  `帮助: 直转说明 / 直转码流地址 ▸`，不可用时 `⚠ 直转不可用 —— 点此展开原因与解决方法 ▸`；
  **不再自动展开**（原 `help.open = !ok` 已移除，满足"默认隐藏、点开显示、再点收起"）；
- 折叠内容含：`#hevcHint`(失败原因/本机 WebRTC 编码列表) + 一/二/三/四 说明(Windows 8 步、VLC 直看、
  服务端根治、网页端 4K 实时) + `#rtspList`(直转码流地址，见 18.3)；
- **新增「4K 实时」按钮 `#btn4kLive`**：一键切到 4K 主码流 —— 客户端支持 HEVC 走 `vis4k`(直转、零转码)，
  否则走 `vis4kh264`(Jetson NVENC 转 H.264)，见第 17 节。原 `#hevcHelp`/`#rtspHelp` 两个 `details` 已删除。

### 18.5 VLC 播放 4K 卡顿：根因与参数修复（v14，★重要）

现场反馈"Windows 与 Jetson 上 VLC 播 4K 都卡"。逐层实测后定位到**两个真凶**：

#### ① go2rtc 的 RTSP 服务器不支持 UDP

```text
[rtsp @ ...] method SETUP failed: 461 Unsupported transport
rtsp://127.0.0.1:8554/vis4k: Protocol not supported
```

go2rtc **只接受 TCP interleaved**；而 VLC 的 RTSP **默认先试 UDP**，会先收到 461 再回退 TCP，
这个回退过程会造成起播慢/抖动。

#### ② VLC/Live555 默认接收缓冲 250KB 被 4K 关键帧打爆（主因）

```text
MultiFramedRTPSource::doGetNextFrame1(): The total received frame size
exceeds the client's buffer size (250000).  302469 bytes of trailing data will be dropped!
```

实测 4K 流的包大小分布（12 s 采样 724 包）：

| 指标 | 值 |
|---|---|
| 平均 | 59,474 B |
| 中位 | 47,837 B |
| **最大** | **801,269 B（≈780 KB！）** |
| **> 250000 B 的包数** | **16** |

→ **4K 的 I 帧远超 VLC 默认 250KB 缓冲，关键帧数据被丢弃 → 花屏/卡顿**。
**这与客户端是 Windows 还是 Jetson 无关，两边都会中招** —— 完美解释了"两台都卡"。

#### 修复：三个 VLC 参数（已写入 Jetson 的 `~/.config/vlc/vlcrc`）

```ini
codec=avcodec                    # 规避 omxil 崩溃(见 18.1)
rtsp-tcp=1                       # 强制 TCP(go2rtc 不支持 UDP)
rtsp-frame-buffer-size=2000000   # 接收缓冲 250KB -> 2MB(容纳 800KB I 帧)
network-caching=1500
```

命令行等价写法：`vlc --rtsp-tcp --rtsp-frame-buffer-size=2000000 --network-caching=1500 rtsp://...`
Windows GUI：工具 → 首选项 → **显示全部** → 输入/编解码器 → RTP/RTSP → 勾选
「使用 RTP over RTSP (TCP)」，并把「RTSP 帧缓冲大小」改为 2000000。

#### 修复效果（实测告警计数）

| 场景 | buffer 被打爆 | 461 协议 | 画面迟到 |
|---|---|---|---|
| 4K · 修复前 | **有（持续）** | **有** | **181** |
| 4K · 修复后 | **0** | **0** | **41** |
| 1080p · 修复后 | 0 | 0 | **0** |

#### ② 之外：Jetson 播 4K 还有硬限制（算力不足，无法靠参数解决）

```text
ffmpeg 纯软件解码 4K H.265:  speed=0.819x,  fps=20   (需要 25fps)
```
Jetson Xavier NX 上 4K H.265 **软解只有约 20fps（0.82× 实时）**，VLC 单核占用约 **307%（≈3 核）**。
所以 **Jetson 本机看 4K 必然掉帧**（迟到告警 41 条就是残留证据）——这不是 VLC 参数能解决的。

> **结论（现场指导）**：
> - **Windows PC**：装 VLC 并加上述三参数；只要显卡能硬解 HEVC（Intel 7 代+/NVIDIA Pascal+/AMD Vega+），**4K 应顺畅**。
> - **Jetson 本机**：用 **1080p(`vis1080`) 或子码流(`vis102`)** —— 实测告警 0、CPU 62%/24%；**不要在 Jetson 上看 4K**。
> - 4K 流参数（实测）：`HEVC Main, 8bit(yuv420p), level 5.1, refs=1, 无 B 帧, 25fps` —— 对硬解器其实很轻，卡的根源是缓冲与算力，不是编码难度。

### 18.6 回归自检（v14）

- Jetson：`vlc rtsp://127.0.0.1:8554/vis1080` 能出画面且无 `Bus error`；`ps` 查 VLC CPU 与上表一致。
- 页面：不支持 HEVC 的浏览器打开 → 状态行红字、3 个直转项灰掉带 `✕`、
  **帮助默认展开**且 summary 显示 `⚠ 直转不可用 …`；点开「直转码流地址」能看到 11 条地址与复制按钮。
- 复制按钮在 http 环境下走 `execCommand` 回退（`navigator.clipboard` 需要 https/localhost）。

---

## 19. Jetson 本机如何看 4K（v15）

### 19.1 先明确：Jetson 上哪些播放器**不行**

| 播放器 | 结论 | 原因 |
|---|---|---|
| **VLC** | ❌ 4K 卡 | 无可用硬解后端：唯一的 `omxil` 模块会 `Bus error`（见 18.1）；只能软解，而软解 4K H.265 实测 `speed=0.819x`（≈20fps，需 25fps） |
| **mpv / ffplay** | ❌ 不可用 | 它们会用 `*_v4l2m2m` 硬解，但本机**没有 `/dev/video*` 节点** → `Could not find a valid device` |

即 **不是"Jetson 看不了 4K"，而是"用错了播放器"**。

### 19.2 正确通路：GStreamer + NVDEC + 零拷贝输出

本机具备的硬解条件（已实测确认）：

```text
/dev/nvhost-nvdec  +  /dev/nvhost-nvdec1      → NVDEC 硬件解码器存在
gst-inspect: nvv4l2decoder / nvvidconv / nveglglessink / nv3dsink  → 全部可用
运行时日志: NvMMLiteOpen : BlockType = 279     → 确认走了 NVDEC(H.265)
```

**实测效果**

| 指标 | 结果 |
|---|---|
| NVDEC 解 4K 的进程 CPU | **约 13%**（单核=100%）—— 是硬解，不是软解 |
| 4K 解码帧率 | **满 25fps**（依据：Qt 程序长期日志 `采集 24.9~26.2fps`，其管线同为 `nvv4l2decoder`） |
| 显示管线 | `nveglglessink` / `nv3dsink` 均正常进入 `PLAYING`，**0 错误 / 0 not-negotiated** |
| 显示路径 | `nveglglessink` 为**零拷贝**（NVMM 直出，不经过 CPU） |

### 19.3 一键脚本 `watch.sh`（已创建并逐流验证）

```bash
/home/lcfc/remote/watch.sh            # 默认 vis1080
/home/lcfc/remote/watch.sh vis4k      # 4K 主码流(H.265)
/home/lcfc/remote/watch.sh vis1080    # 1080p 直转
/home/lcfc/remote/watch.sh vis102     # 704x576 直转
/home/lcfc/remote/watch.sh thermal    # 热像仪(H.264, 自动识别)
/home/lcfc/remote/watch.sh rtsp://admin:***@192.168.10.212:554/Streaming/Channels/101  # 相机直连
```

- 自动按流名/地址判断解复用器（H.265 ↔ H.264），可用 `CODEC=h264|h265` 强制；
- 默认 `DISPLAY=:0` + `nveglglessink`，可用 `SINK=nv3dsink` 或 `SINK=xvimagesink` 换后端；
- 测试结果：`vis4k`(h265)、`thermal`(h264)、`vis1080h264`(h264) 三例均 **0 错误**、进入 `PLAYING`。

### 19.4 等价的手工命令

```bash
# H.265 直转流(4K/1080p/子码流)
DISPLAY=:0 gst-launch-1.0 -e \
  rtspsrc location=rtsp://127.0.0.1:8554/vis4k latency=0 protocols=tcp \
          drop-on-latency=true buffer-mode=0 ntp-sync=false \
  ! rtph265depay ! h265parse config-interval=-1 \
  ! nvv4l2decoder enable-max-performance=1 disable-dpb=true \
  ! nvvidconv ! nveglglessink sync=false

# H.264 流(热像仪/转码流)：把 rtph265depay ! h265parse 换成 rtph264depay ! h264parse
```

> 关键点：`protocols=tcp`（go2rtc 的 RTSP 不支持 UDP，见 18.5）、
> `nvv4l2decoder`（走 NVDEC）、`nveglglessink`（零拷贝上屏）。
> 若画面显示异常，优先把 sink 换成 `nv3dsink`，其次 `xvimagesink`（会经 CPU，性能差些）。

---

## 20. Qt 程序：选主码流(4K)自动改走外部 GStreamer 窗口（2026-09-23）

**需求**：Qt 里选「主码流 4K (101)」时，程序内**不再显示**可见光画面，自动拉起
`/home/lcfc/remote/watch.sh`（GStreamer / NVDEC 零拷贝窗口）显示 4K 并弹出提示；
切到其它码流则关闭该窗口、恢复程序内显示。

### 20.1 改动清单

| 文件 | 内容 |
|---|---|
| `src/mainwindow.h` | `#include <QProcess>`；新增 `updateExternal4kState()` / `startExternal4kPlayer()` / `stopExternal4kPlayer()`；新增成员 `m_label4kHint`、`m_ext4kProc`、`m_ext4kActive`、`m_lastStreamChannel4k` |
| `src/mainwindow.cpp` | ① `renderVisible()` 增加守卫：外部 4K 模式下直接 `return`，**不渲染 4K 画面**；② 可见光面板「拉取码流」下新增黄色提示标签；③ 实现上述三个方法；④ `applyStreamProfile()` 末尾调用 `updateExternal4kState(channel)`；⑤ 析构函数调用 `stopExternal4kPlayer()`，避免退出后残留播放窗口 |
| `/home/lcfc/remote/watch.sh` | 由 `exec` 改为「后台 `gst-launch` + `trap` + `wait`」：Qt 发 `SIGTERM` 时 trap 会一并结束 `gst-launch`，**不会残留播放窗口** |

### 20.2 行为

| 所选码流 | Qt 内画面 | 外部窗口 | 提示 |
|---|---|---|---|
| **主码流 4K (101)** | **不显示**（清空并显示"画面已交由外部窗口显示"） | 自动启动 `watch.sh <当前RTSP地址>` | 弹出对话框 + 面板常驻黄色提示（仅在**切换到**主码流时弹一次，避免启动/其它操作重复弹窗） |
| 第三码流 1080p (103) | 正常显示 | 自动关闭 | 隐藏 |
| 子码流 704x576 (102) | 正常显示 | 自动关闭 | 隐藏 |

提示文案（按需求原文）：

> 在 Jetson 上看 4K，别用 VLC（它在这台机器上没有硬解后端，软解只有 20fps），改用 GStreamer：nvv4l2decoder 走 NVDEC + nveglglessink 零拷贝上屏4K 满 25fps。

### 20.3 实现要点

- **不渲染**：`renderVisible()` 在 `m_ext4kActive` 时直接返回，界面线程完全不碰 4K 贴图；
  切换回其它码流时清空占位文字并立即 `renderVisible()` 恢复画面。
- **进程管理**：用 `QProcess` 启动 `/bin/bash /home/lcfc/remote/watch.sh <url>`，
  继承/兜底 `DISPLAY=:0`；关闭时先 `terminate()`（SIGTERM）并等待 2.5s，超时才 `kill()`。
- **幂等**：已在运行时不会重复启动；`applyStreamProfile()` 被多处调用（下拉切换、初始加载、
  预览策略复选框）也不会重复拉起进程。
- **URL 来源**：使用 `ConfigManager::visibleRtspUrl()`（即当前所选主码流地址），
  watch.sh 会自动识别为 H.265 并用 `rtph265depay ! h265parse`。

### 20.4 验证

| 项 | 结果 |
|---|---|
| `g++ -fsyntax-only`（Qt5Widgets/Network/SerialPort + opencv4） | **通过** |
| `make -j6` 完整构建 | **成功**，`build/SubseaImagingSystem` 已更新 |
| `bash -n watch.sh` | 语法 **OK** |
| 实测向 watch.sh 发 SIGTERM | 脚本退出且其 `gst-launch` **一同退出（无残留）**；go2rtc 的转码子进程未受影响 |
| 4K 流复核 | `ffprobe rtsp://127.0.0.1:8554/vis4k` → `hevc,3840,2160` |

> **必须重启 Qt 程序才生效**：构建产物已更新，但当前正在运行的仍是旧进程。


_最后更新：Qt(2026-09-23) 选主码流 4K 时程序内不显示画面、自动拉起外部 GStreamer(NVDEC+零拷贝)窗口并提示（见 §20）；v15 —— 定位 "Jetson 上看 4K" 的正确通路：VLC/mpv 在 Jetson 上均不可行（无硬解后端），改用 GStreamer + NVDEC(`nvv4l2decoder`) + `nveglglessink` 零拷贝，实测 CPU 仅 13%、4K 满 25fps，并提供一键脚本 `/home/lcfc/remote/watch.sh`；v14 —— 修复 VLC 播 4K 卡顿（go2rtc 不支持 UDP → `--rtsp-tcp`；4K I 帧 801KB 打爆默认 250KB 缓冲 → `--rtsp-frame-buffer-size=2000000`）；v13 —— Jetson 安装配置 VLC + 码流地址帮助块 + HEVC 帮助整合；v12 —— 六路码流与 HEVC 自检；v11 —— 热像仪鼠标探针；v10 —— 温度条误识别修复、右上角去掉 640×512 50fps、Qt 探针加灰度。_
