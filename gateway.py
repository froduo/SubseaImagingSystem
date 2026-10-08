#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
水下成像系统 - 远程控制网关 (纯标准库, 无第三方依赖)

职责:
  1. 提供静态操作台页面 (web/index.html)
  2. 以 ISAPI 直接控制可见光相机 (云台/变焦/光圈/增益/曝光/对焦/日夜/抓图)
  3. 可选桥接现有程序的 TCP 8888 (角度推送 angle_data -> SSE), 程序未启动该服务时自动降级
  4. 录像: 调用 ffmpeg 从 go2rtc 的 RTSP 出口 -c copy 直存 (不转码)

设计要点:
  - 与现有 Qt 程序解耦: 只用 ISAPI(HTTP 无状态) 与本地 RTSP, 不改程序
  - 云台/变焦为"连续量", 下发后按 ms 自动回零, 避免"跑飞"
  - 所有写操作都在全局锁内串行, 防止并发下发互相打断
"""
import json
import os
import re
import signal
import socket
import subprocess
import threading
import time
import urllib.request as urlreq
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------- 配置
CONFIG_INI = "/home/tianheng/project/SubseaImagingSystem/config/default.ini"
WEB_DIR = os.path.dirname(os.path.abspath(__file__)) + "/web"
GO2RTC_RTSP = "rtsp://127.0.0.1:8554"          # go2rtc 的 RTSP 出口
EVIDENCE_DIR = "/home/lcfc/evidence"
PORT = 8080
PROG_TCP = ("127.0.0.1", 8888)                  # 现有程序的控制面(可能未开启)

IRIS_TABLE = [160, 200, 240, 280, 340, 400, 480, 560,
              680, 960, 1100, 1400, 1600, 1900, 2200]
FOCUS_DIST_TABLE = [10, 30, 100, 150, 300, 600, 1000, 2000, 65535]
ZOOM_POS_MIN, ZOOM_POS_MAX = 10, 230


def load_camera_cfg():
    """从现有程序配置里读相机地址与账号, 保持单一数据源"""
    ip, port, user, pw = "192.168.10.212", 80, "admin", "cisdi135"
    try:
        txt = open(CONFIG_INI, encoding="utf-8", errors="replace").read()
        def g(key, cur):
            m = re.search(rf"^{key}\s*=\s*(.+)$", txt, re.M)
            return m.group(1).strip() if m else cur
        ip = g("visible_camera_ip", ip)
        port = int(g("visible_camera_port", port))
        user = g("visible_camera_user", user)
        pw = g("visible_camera_pass", pw)
    except Exception as e:  # noqa: BLE001
        print(f"[WARN] 读配置失败, 使用默认: {e}", flush=True)
    return ip, port, user, pw


CAM_IP, CAM_PORT, CAM_USER, CAM_PW = load_camera_cfg()
BASE = f"http://{CAM_IP}:{CAM_PORT}"

_opener = urlreq.build_opener(
    urlreq.HTTPDigestAuthHandler(urlreq.HTTPPasswordMgrWithDefaultRealm()))
_opener.addheaders = [("User-Agent", "subsea-gw")]


def isapi(method, path, body=None, timeout=8):
    """调用相机 ISAPI, 返回 (ok, status, text_or_bytes)"""
    pm = urlreq.HTTPPasswordMgrWithDefaultRealm()
    pm.add_password(None, f"{BASE}/", CAM_USER, CAM_PW)
    opener = urlreq.build_opener(urlreq.HTTPDigestAuthHandler(pm))
    url = BASE + path
    data = body.encode("utf-8") if isinstance(body, str) else body
    req = urlreq.Request(url, data=data, method=method)
    if data:
        req.add_header("Content-Type", "application/xml")
    try:
        with opener.open(req, timeout=timeout) as r:
            return True, r.status, r.read()
    except urlreq.HTTPError as e:
        return False, e.code, e.read()[:300]
    except Exception as e:  # noqa: BLE001
        return False, 0, str(e)[:200]


def xml_text(raw):
    return raw.decode("utf-8", "replace") if isinstance(raw, bytes) else str(raw)


_lock = threading.RLock()
_ptz_gen = 0


_zoom_af_timer = None


def focus_once_after_zoom(hold_s=1.2):
    """变焦结束后在当前倍率下**强制**执行一次自动对焦。

    2026-09-24 修正: 该机芯没有 /ISAPI/Image/channels/1/focus 一键聚焦端点(404);
    而"把 focusStyle 写成同一个值"(例如本来就是 SEMIAUTOMATIC)不会产生任何镜头动作,
    于是出现"点了变焦但没对焦"。这里改为: 先切 AUTO 让镜头在当前倍率下完成一次自动对焦,
    再切回原模式锁住焦点 —— 无论原模式是什么, 每次都会真正对焦一次。
    """
    try:
        cur = (camera_status().get("focus_style") or "").upper()
        ok, st, _ = read_modify_write(
            "/ISAPI/Image/channels/1", "FocusConfiguration", "focusStyle", "AUTO")
        if not ok:
            print(f"[WARN] 变焦后自动对焦失败(切AUTO): http={st}", flush=True)
            return
        time.sleep(max(0.3, hold_s))          # 等连续自动对焦在当前倍率下完成聚焦
        back = cur if cur in ("MANUAL", "SEMIAUTOMATIC") else "SEMIAUTOMATIC"
        ok2, st2, _ = read_modify_write(
            "/ISAPI/Image/channels/1", "FocusConfiguration", "focusStyle", back)
        print("[INFO] 变焦后自动对焦完成, focusStyle 恢复 "
              + back + ("" if ok2 else f"(HTTP {st2})"), flush=True)
    except Exception as e:  # noqa: BLE001
        print(f"[WARN] 变焦后自动对焦异常: {e}", flush=True)


def schedule_focus_after_zoom(delay_s=1.0):
    """去抖调度: 连续变焦/连点按钮只在最后一次动作结束后触发**一次**对焦"""
    global _zoom_af_timer
    with _lock:
        if _zoom_af_timer is not None:
            _zoom_af_timer.cancel()
        _zoom_af_timer = threading.Timer(max(0.2, delay_s), focus_once_after_zoom)
        _zoom_af_timer.daemon = True
        _zoom_af_timer.start()


def ptz_move(pan=0, tilt=0, zoom=0, ms=0):
    """下发连续云台/变焦, ms>0 时到时自动回零(带代数号防止旧定时器误停新的动作)"""
    global _ptz_gen
    with _lock:
        _ptz_gen += 1
        gen = _ptz_gen
        body = ('<PTZData xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                f'<pan>{max(-100, min(100, pan))}</pan>'
                f'<tilt>{max(-100, min(100, tilt))}</tilt>'
                f'<zoom>{max(-100, min(100, zoom))}</zoom></PTZData>')
        ok, st, resp = isapi("PUT", "/ISAPI/PTZCtrl/channels/1/continuous", body)
    if ok and ms > 0:
        def _stop():
            time.sleep(max(0.05, ms / 1000.0))
            with _lock:
                if gen != _ptz_gen:
                    return
            isapi("PUT", "/ISAPI/PTZCtrl/channels/1/continuous",
                  '<PTZData xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                  '<pan>0</pan><tilt>0</tilt><zoom>0</zoom></PTZData>')
        threading.Thread(target=_stop, daemon=True).start()
    return ok, st, xml_text(resp)


def get_doc(path):
    ok, st, raw = isapi("GET", path)
    if not ok:
        return None, st
    doc = xml_text(raw)
    # Hikvision 带命名空间, 统一去掉前缀便于正则匹配
    doc = re.sub(r"<([a-zA-Z0-9]+):", "<", doc)
    doc = re.sub(r"</([a-zA-Z0-9]+):", "</", doc)
    return doc, st


def read_modify_write(path, block, tag, value):
    """整篇 read-modify-write: /ISAPI/Image/channels/1 不接受片段"""
    doc, st = get_doc(path)
    if doc is None:
        return False, st, "读取图像文档失败"
    m = re.search(rf"<{block}[^>]*>.*?</{block}>", doc, re.S)
    if m:
        blk = m.group(0)
        if re.search(rf"<{tag}>[^<]*</{tag}>", blk):
            newblk = re.sub(rf"<{tag}>[^<]*</{tag}>", f"<{tag}>{value}</{tag}>", blk)
        else:
            newblk = blk.replace(f"</{block}>", f"<{tag}>{value}</{tag}></{block}>")
        doc = doc.replace(blk, newblk)
    else:
        doc = doc.replace("</ImageChannel>",
                          f"<{block}><{tag}>{value}</{tag}></{block}></ImageChannel>")
    ok, st2, raw = isapi("PUT", path, doc)
    return ok, st2, xml_text(raw)[:200]


def camera_status():
    out = {"ok": True}
    doc, st = get_doc("/ISAPI/PTZCtrl/channels/1/status")
    if doc:
        m = re.search(r"<absoluteZoom>(\d+)</absoluteZoom>", doc)
        if m:
            z = int(m.group(1))
            out["zoom_pos"] = z
            out["zoom_x"] = round(z / 10.0, 2)
    doc2, st2 = get_doc("/ISAPI/Image/channels/1")
    if doc2:
        for k, tag in (("iris_level", "IrisLevel"), ("gain", "GainLevel"),
                       ("ircut", "IrcutFilterType"), ("focus_style", "focusStyle"),
                       ("focus_limited", "focusLimited")):
            m = re.search(rf"<{tag}>([^<]*)</{tag}>", doc2)
            if m:
                out[k] = m.group(1)
    return out


# ---------------------------------------------------------------- 录像
_rec = {"proc": None, "path": None, "t0": 0}


REC_LOG = "ffmpeg_record.log"


def _tail_file(path, lines=3):
    try:
        with open(path, errors="replace") as f:
            return " | ".join([x.strip() for x in f.readlines()[-lines:] if x.strip()])
    except OSError:
        return ""


def record_start(stream="vis1080"):
    """开始录像(直拷所选码流, 不转码)。

    2026-09-24 修复: go2rtc 的直转流(如 vis4k/vis1080/thermal)同时带**音轨 G.711 PCM A-Law**,
    而 MP4 容器不支持该编码, ffmpeg 会报
        "Could not find tag for codec pcm_alaw in stream #1, codec not currently supported in container"
    并拒绝写文件头 -> 生成 0 字节 mp4(点击播放时黑屏/无内容)。
    录像只需要画面, 因此这里显式只取视频轨(-map 0:v:0)并丢弃音频(-an), 仍然 -c:v copy 不转码。
    """
    with _lock:
        if _rec["proc"] and _rec["proc"].poll() is None:
            return False, "已在录像中"
        os.makedirs(EVIDENCE_DIR, exist_ok=True)
        name = f"remote_{stream}_{time.strftime('%Y%m%d_%H%M%S')}.mp4"
        path = os.path.join(EVIDENCE_DIR, name)
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "warning",
               "-rtsp_transport", "tcp", "-i", f"{GO2RTC_RTSP}/{stream}",
               "-map", "0:v:0", "-c:v", "copy", "-an",
               "-movflags", "+faststart", "-fflags", "+genpts", "-y", path]
        errlog = os.path.join(EVIDENCE_DIR, REC_LOG)
        try:
            ferr = open(errlog, "ab")
        except OSError:
            ferr = subprocess.DEVNULL
        try:
            p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=ferr)
        except Exception as e:  # noqa: BLE001
            return False, f"启动 ffmpeg 失败: {e}"
        _rec.update(proc=p, path=path, t0=time.time())
        # 存活检查: ffmpeg 立刻退出(流名不存在/容器不支持等)时给出真实原因, 并清掉 0 字节文件
        time.sleep(1.0)
        if p.poll() is not None:
            tail = _tail_file(errlog, 3)
            try:
                if os.path.exists(path) and os.path.getsize(path) == 0:
                    os.remove(path)
            except OSError:
                pass
            _rec.update(proc=None)
            print(f"[WARN] 录像启动失败 stream={stream}: {tail}", flush=True)
            return False, (f"ffmpeg 启动即失败(流 {stream} 不可用或编码不被 MP4 支持): "
                           f"{tail or ('见 ' + errlog)}")
        return True, name


def record_stop():
    with _lock:
        p = _rec.get("proc")
        if not p or p.poll() is not None:
            return False, "当前未在录像"
        try:
            p.send_signal(signal.SIGINT)     # 优雅收尾 mp4
            p.wait(timeout=8)
        except Exception:  # noqa: BLE001
            try:
                p.kill()
            except Exception:  # noqa: BLE001
                pass
        path = _rec.get("path") or ""
        name = os.path.basename(path)
        try:
            size = os.path.getsize(path)
        except OSError:
            size = 0
        _rec.update(proc=None)
        if size <= 0:
            # 0 字节 = 没录上: 删除残file 并明确报错, 避免界面显示"看起来成功了"
            try:
                os.remove(path)
            except OSError:
                pass
            return False, ("录像失败(输出为空): "
                           + (_tail_file(os.path.join(EVIDENCE_DIR, REC_LOG), 3)
                              or "见 " + REC_LOG))
        return True, f"{name} ({size / 1048576.0:.1f} MB)"


# ---------------------------------------------------------------- 8888 桥接 (SSE)
_sse_clients = []
_sse_lock = threading.Lock()
_tcp_state = {"connected": False, "last": "", "conflict": False}


def sse_broadcast(obj):
    data = f"data: {json.dumps(obj, ensure_ascii=False)}\n\n".encode("utf-8")
    with _sse_lock:
        dead = []
        for q in _sse_clients:
            try:
                q.append(data)
            except Exception:  # noqa: BLE001
                dead.append(q)
        for d in dead:
            _sse_clients.remove(d)


def prog_tcp_port_conflict():
    """网关自带的 TCP 服务器是否已占用程序侧端口(8888)。

    此时再去连 127.0.0.1:8888 只会连到网关自己(loopback 自连接):
      - 会被自己的 accept 记成 1 个"客户端"(假客户端);
      - 会让 prog_tcp 误报"已连接"。
    因此端口被自己占用时必须跳过桥接。
    """
    try:
        with _tcpsrv_lock:
            return bool(_tcpsrv["running"] and int(_tcpsrv["port"]) == int(PROG_TCP[1]))
    except Exception:  # noqa: BLE001
        return False


def prog_tcp_state():
    """桥接状态: connected | port_conflict | disconnected"""
    if _tcp_state.get("conflict"):
        return "port_conflict"
    return "connected" if _tcp_state["connected"] else "disconnected"


def prog_tcp_worker():
    """持续尝试连接现有程序的 TCP 8888, 把 angle_data 等推送转给 SSE"""
    while True:
        # 端口被网关自带 TCP 占用时连 8888 只会连到自己(自连接): 跳过并如实上报冲突
        conflict = prog_tcp_port_conflict()
        if conflict != _tcp_state.get("conflict"):
            _tcp_state["conflict"] = conflict
            if conflict:
                _tcp_state["connected"] = False
                sse_broadcast({"type": "tcp", "connected": False, "reason": "port_conflict"})
        if conflict:
            time.sleep(2)
            continue
        try:
            s = socket.create_connection(PROG_TCP, timeout=5)
            s.settimeout(1.0)
            _tcp_state["connected"] = True
            sse_broadcast({"type": "tcp", "connected": True})
            buf = b""
            while True:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    txt = line.decode("utf-8", "replace").strip()
                    if not txt:
                        continue
                    _tcp_state["last"] = txt
                    obj = None
                    try:
                        obj = json.loads(txt)
                    except Exception:  # noqa: BLE001
                        obj = {"type": "raw", "text": txt}
                    sse_broadcast({"type": "prog", "data": obj})
            s.close()
        except Exception:  # noqa: BLE001
            pass
        if _tcp_state["connected"]:
            _tcp_state["connected"] = False
            sse_broadcast({"type": "tcp", "connected": False})
        time.sleep(3)


def prog_tcp_send(line):
    """把控制指令转发给现有程序的 8888 (可选通道, 例如 zoom/-focus/iris/ptz)"""
    if prog_tcp_port_conflict():
        return False, ("端口 8888 被网关自带 TCP 占用, 未转发到本地程序 "
                       "(请先在程序中停止 TCP, 或停用网关自带 TCP)")
    try:
        s = socket.create_connection(PROG_TCP, timeout=3)
        s.sendall((line.strip() + "\n").encode())
        s.settimeout(1.5)
        try:
            resp = s.recv(2048).decode("utf-8", "replace").strip()
        except Exception:  # noqa: BLE001
            resp = ""
        s.close()
        return True, resp
    except Exception as e:  # noqa: BLE001
        return False, f"程序 TCP 未连接: {e}"


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    server_version = "SubseaGateway/1.0"

    def log_message(self, fmt, *args):
        pass

    # ---- 工具
    def _json(self, obj, code=200):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def _body_json(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0:
                return {}
            return json.loads(self.rfile.read(n).decode("utf-8", "replace") or "{}")
        except Exception:  # noqa: BLE001
            return {}

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    # ---- GET
    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/api/events":
            return self._sse()
        if path == "/api/status":
            st = camera_status()
            st["prog_tcp"] = _tcp_state["connected"]
            st["prog_tcp_state"] = prog_tcp_state()
            st["recording"] = bool(_rec["proc"] and _rec["proc"].poll() is None)
            st["record_file"] = os.path.basename(_rec["path"]) if _rec["path"] else None
            return self._json(st)
        if path.startswith("/evidence/"):
            return self._file(os.path.join(EVIDENCE_DIR, os.path.basename(path)))
        if path.startswith("/api/file/"):
            return self._file(os.path.join(EVIDENCE_DIR, os.path.basename(path)))
        return self._static(path)

    # ---- POST
    def do_POST(self):
        path = self.path.split("?")[0]
        p = self._body_json()

        if path == "/api/ptz":
            ms = int(p.get("ms") or 300)
            ms = max(50, min(5000, ms))
            zoom_speed = int(p.get("zoom") or 0)
            ok, st, resp = ptz_move(int(p.get("pan") or 0), int(p.get("tilt") or 0),
                                    zoom_speed, ms)
            # 2026-09-24: 只要本次含变焦(单击或按住都一样), 在该动作结束后于**当前焦距**
            # 强制自动对焦一次; schedule 内部去抖, 连续操作只触发最后一次。
            if zoom_speed != 0:
                schedule_focus_after_zoom(ms / 1000.0 + 0.4)
            return self._json({"ok": ok, "http": st, "ms": ms, "detail": resp[:120]})

        if path == "/api/ptzstop":
            ok, st, resp = ptz_move(0, 0, 0, 0)
            # 2026-09-24: 变焦(按住连续)松开后, 在当前倍率下自动对焦一次(去抖)
            if p.get("focus_once"):
                schedule_focus_after_zoom(0.4)
            return self._json({"ok": ok, "http": st})

        if path == "/api/iris":
            v = int(p.get("level") or 400)
            v = max(IRIS_TABLE[0], min(IRIS_TABLE[-1], v))
            isapi("PUT", "/ISAPI/Image/channels/1/exposure",
                  '<Exposure xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                  '<ExposureType>manual</ExposureType></Exposure>')
            ok, st, resp = isapi("PUT", "/ISAPI/Image/channels/1/iris",
                                 '<Iris xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                                 f'<IrisLevel>{v}</IrisLevel></Iris>')
            return self._json({"ok": ok, "http": st, "level": v})

        if path == "/api/gain":
            v = max(0, min(100, int(p.get("level") or 50)))
            isapi("PUT", "/ISAPI/Image/channels/1/exposure",
                  '<Exposure xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                  '<ExposureType>manual</ExposureType></Exposure>')
            ok, st, resp = isapi("PUT", "/ISAPI/Image/channels/1/gain",
                                 '<Gain xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                                 f'<GainLevel>{v}</GainLevel><GainLimit>100</GainLimit></Gain>')
            return self._json({"ok": ok, "http": st, "level": v})

        if path == "/api/exposure":
            mode = "auto" if str(p.get("mode")) == "auto" else "manual"
            ok, st, resp = isapi("PUT", "/ISAPI/Image/channels/1/exposure",
                                 '<Exposure xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                                 f'<ExposureType>{mode}</ExposureType></Exposure>')
            return self._json({"ok": ok, "http": st, "mode": mode})

        if path == "/api/focus":
            cur = camera_status().get("focus_limited")
            try:
                cur = int(cur)
            except Exception:  # noqa: BLE001
                cur = 600
            idx = min(range(len(FOCUS_DIST_TABLE)),
                      key=lambda i: abs(FOCUS_DIST_TABLE[i] - cur))
            d = 1 if int(p.get("dir") or 1) > 0 else -1
            idx = max(0, min(len(FOCUS_DIST_TABLE) - 1, idx + d))
            val = FOCUS_DIST_TABLE[idx]
            ok, st, resp = read_modify_write(
                "/ISAPI/Image/channels/1", "FocusConfiguration", "focusLimited", val)
            if not ok or "focusStyle" not in (camera_status().get("focus_style") or ""):
                read_modify_write("/ISAPI/Image/channels/1",
                                  "FocusConfiguration", "focusStyle", "MANUAL")
            return self._json({"ok": ok, "http": st, "focus_limited": val})

        if path == "/api/ircut":
            mode = str(p.get("mode") or "auto")
            if mode not in ("day", "night", "auto"):
                mode = "auto"
            ok, st, resp = read_modify_write(
                "/ISAPI/Image/channels/1", "IrcutFilter", "IrcutFilterType", mode)
            return self._json({"ok": ok, "http": st, "mode": mode})

        if path == "/api/snapshot":
            ok, st, raw = isapi("GET", "/ISAPI/Streaming/channels/101/picture")
            if not ok:
                return self._json({"ok": False, "http": st, "detail": xml_text(raw)[:120]})
            os.makedirs(EVIDENCE_DIR, exist_ok=True)
            name = f"snap_{time.strftime('%Y%m%d_%H%M%S')}.jpg"
            path_p = os.path.join(EVIDENCE_DIR, name)
            with open(path_p, "wb") as f:
                f.write(raw)
            return self._json({"ok": True, "file": name, "bytes": len(raw),
                               "url": f"/evidence/{name}"})

        if path == "/api/record/start":
            ok, msg = record_start(str(p.get("stream") or "vis1080"))
            return self._json({"ok": ok, "detail": msg})

        if path == "/api/record/stop":
            ok, msg = record_stop()
            return self._json({"ok": ok, "detail": msg})

        if path == "/api/progcmd":
            line = str(p.get("line") or "")
            if not re.match(r"^[a-z]+( [+\-a-z0-9]+)*$", line, re.I):
                return self._json({"ok": False, "detail": "指令格式不合法"})
            ok, resp = prog_tcp_send(line)
            return self._json({"ok": ok, "resp": resp})

        return self._json({"ok": False, "detail": "unknown api"}, 404)

    # ---- SSE
    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        q = []
        with _sse_lock:
            _sse_clients.append(q)
        try:
            self.wfile.write(b": connected\n\n")
            self.wfile.flush()
            last_hb = time.time()
            while True:
                if q:
                    while q:
                        self.wfile.write(q.pop(0))
                    self.wfile.flush()
                elif time.time() - last_hb > 10:
                    last_hb = time.time()
                    self.wfile.write(b": hb\n\n")
                    self.wfile.flush()
                time.sleep(0.2)
        except Exception:  # noqa: BLE001
            pass
        finally:
            with _sse_lock:
                if q in _sse_clients:
                    _sse_clients.remove(q)

    # ---- 静态文件
    def _file(self, path):
        """发送证据文件(抓图/录像)。

        2026-09-24: 增加 HTTP Range(206) 支持并改为分块发送。
          · 浏览器 <video> 播放/拖动进度依赖 206 + Accept-Ranges;
          · 原来一次性 f.read() 整个文件, 4K 长录像(可达上 GB)会把整文件读进内存;
          · 现在按 256KB 分块输出, 并正确处理 bytes=a-b / bytes=-N / 416。
        """
        if not os.path.isfile(path):
            return self._json({"ok": False, "detail": "not found"}, 404)
        size = os.path.getsize(path)
        low = path.lower()
        if low.endswith((".jpg", ".jpeg")):
            ctype = "image/jpeg"
        elif low.endswith(".png"):
            ctype = "image/png"
        elif low.endswith(".mp4"):
            ctype = "video/mp4"
        else:
            ctype = "application/octet-stream"

        start, end = 0, max(0, size - 1)
        partial = False
        rng = self.headers.get("Range") or ""
        if rng.startswith("bytes="):
            try:
                first, _, last = rng[6:].split(",")[0].partition("-")
                if first == "":                        # bytes=-N: 末尾 N 字节
                    start = max(0, size - int(last))
                else:
                    start = int(first)
                    if last:
                        end = min(size - 1, int(last))
                partial = True
            except Exception:  # noqa: BLE001
                start, end, partial = 0, max(0, size - 1), False
        if size == 0 or start > end or start >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return

        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            with open(path, "rb") as f:
                f.seek(start)
                remain = length
                while remain > 0:
                    chunk = f.read(min(262144, remain))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remain -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass                                   # 客户端拖动进度会主动断开, 属正常

    def _static(self, path):
        rel = "index.html" if path in ("/", "") else path.lstrip("/")
        full = os.path.normpath(os.path.join(WEB_DIR, rel))
        if not full.startswith(WEB_DIR) or not os.path.isfile(full):
            self.send_response(404)
            self.end_headers()
            return
        ctype = {"html": "text/html; charset=utf-8", "js": "application/javascript",
                 "css": "text/css"}.get(full.rsplit(".", 1)[-1], "application/octet-stream")
        with open(full, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        # 2026-09-24: 前端迭代期禁用浏览器缓存, 否则页面继续用旧版 -> "新加的东西看不到"
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        self.end_headers()
        self.wfile.write(data)




# ============================================================ [追加] 热源检测参数 / TCP服务器 / 激光状态
INI_PATH = "/home/tianheng/project/SubseaImagingSystem/config/default.ini"

DETECT_FIELDS = [
    ("enable_detection", "bool"),
    ("manual_mode", "bool"),
    ("max_sources", "int"),
    ("min_area_pixels", "int"),
    ("min_temp", "float"),
    ("temp_threshold_delta", "float"),
    ("grid", "bool"),
]


def ini_read_detect():
    out = {}
    try:
        txt = open(INI_PATH, encoding="utf-8", errors="replace").read()
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "detail": str(e)}
    for key, _ in DETECT_FIELDS:
        m = re.search(rf"(?m)^{key}\s*=\s*(\S+)", txt)
        out[key] = m.group(1) if m else None
    m = re.search(r"(?m)^tcp_server_port\s*=\s*(\d+)", txt)
    out["tcp_server_port"] = m.group(1) if m else "8888"
    out["ok"] = True
    return out


def ini_write_detect(vals):
    """按行精确替换/插入键值; 不破坏原文件的非标准注释行"""
    try:
        lines = open(INI_PATH, encoding="utf-8", errors="replace").read().splitlines()
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "detail": str(e)}

    def set_key(section, key, value):
        cur = None
        for i, ln in enumerate(lines):
            if ln.strip().startswith(f"[{section}]"):
                cur = i
                continue
            if cur is not None and ln.strip().startswith("["):
                # 进入下一段: 若未找到 key 则回插到该段末尾
                for j in range(i - 1, cur, -1):
                    if lines[j].strip():
                        lines.insert(j + 1, f"{key}={value}")
                        return
                lines.insert(cur + 1, f"{key}={value}")
                return
            if cur is not None and re.match(rf"^{key}\s*=", ln):
                lines[i] = f"{key}={value}"
                return
        for j in range(len(lines) - 1, -1, -1):
            if lines[j].strip().startswith(f"[{section}]"):
                lines.insert(j + 1, f"{key}={value}")
                return
        lines.append(f"[{section}]")
        lines.append(f"{key}={value}")

    for k, _ in DETECT_FIELDS:
        if k not in vals or vals[k] in (None, ""):
            continue
        v = str(vals[k]).lower() if isinstance(vals[k], bool) else str(vals[k])
        if k == "max_sources":
            v = str(max(1, min(50, int(float(v)))))
        elif k == "min_area_pixels":
            v = str(max(1, min(100000, int(float(v)))))
        elif k in ("min_temp", "temp_threshold_delta"):
            v = f"{float(v):.1f}"
        elif k in ("enable_detection", "manual_mode", "grid"):
            v = "true" if str(vals[k]).lower() in ("1", "true", "yes", "on") else "false"
        set_key("display" if k == "grid" else "detection", k, v)

    try:
        with open(INI_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "detail": str(e)}
    return {"ok": True, "note": "已写入配置文件; 需重启本地程序后生效"}


def tcp_port():
    try:
        return int(ini_read_detect().get("tcp_server_port") or 8888)
    except Exception:  # noqa: BLE001
        return 8888


def tcp_listening():
    """通过 /proc/net/tcp 判断 8888 是否在监听, 并数出已建立连接数"""
    port = tcp_port()
    hexport = f"{port:04X}"
    listen = 0
    estab = 0
    try:
        for fn, want in (("/proc/net/tcp", 2), ("/proc/net/tcp6", 2)):
            for ln in open(fn).read().splitlines()[1:]:
                f = ln.split()
                if len(f) < 4:
                    continue
                local, state = f[1], f[3]
                if local.split(":")[-1].upper() != hexport:
                    continue
                if state == "0A":
                    listen += 1
                elif state == "01":
                    estab += 1
    except Exception:  # noqa: BLE001
        pass
    return {"port": port, "listening": listen > 0, "clients": estab}


def laser_state():
    """激光状态: 串口被本地程序独占, 这里只报告占用情况与可行方案"""
    dev = "/dev/ir_laser"
    busy = False
    try:
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        os.close(fd)
    except OSError as e:
        busy = getattr(e, "errno", 0) == 16
    return {
        "device": dev,
        "exclusive": busy,
        "controllable": not busy,
        "rs485_rts": LASER_RS485_RTS,
        "rs485_active_low": LASER_RS485_ACTIVE_LOW,
        "note": ("串口被本地 Qt 程序独占, 远程激光控制需在程序中新增 TCP 命令"
                 "(Pelco_D 命令表已具备, 约 60 行可完成)") if busy
                else "串口空闲, 网关可直接下发 Pelco_D 控制",
    }


_orig_do_GET = Handler.do_GET
_orig_do_POST = Handler.do_POST


def _do_GET(self) -> None:
    path = self.path.split("?")[0]
    if path == "/api/detect":
        return self._json(ini_read_detect())
    if path == "/api/tcp":
        st = tcp_listening()
        st["prog_tcp"] = _tcp_state["connected"]
        return self._json(st)
    if path == "/api/laser":
        return self._json(laser_state())
    if path == "/api/streams":
        # 代理 go2rtc 的流列表, 便于前端判断哪些流可用
        try:
            with urlreq.urlopen("http://127.0.0.1:1984/api/streams", timeout=6) as r:
                return self._json(json.loads(r.read().decode()))
        except Exception as e:  # noqa: BLE001
            return self._json({"ok": False, "detail": str(e)})
    return _orig_do_GET(self)


def _do_POST(self) -> None:
    path = self.path.split("?")[0]
    if path == "/api/detect":
        vals = self._body_json()
        return self._json(ini_write_detect(vals))
    if path == "/api/tcp":
        line = str(self._body_json().get("line") or "")
        if not re.match(r"^[a-z]+( [+\-a-z0-9]+)*$", line, re.I):
            return self._json({"ok": False, "detail": "指令格式不合法"})
        ok, resp = prog_tcp_send(line)
        return self._json({"ok": ok, "resp": resp})
    if path == "/api/laser":
        st = laser_state()
        if not st["controllable"]:
            return self._json({"ok": False, "detail": st["note"]})
        return self._json({"ok": False, "detail": "网关直控激光尚未启用(需程序侧配合)"})
    return _orig_do_POST(self)


Handler.do_GET = _do_GET
Handler.do_POST = _do_POST




# ============================================================ [追加] 红外激光直控 (Pelco_D)
import array
import fcntl
import termios

LASER_MAX_ON_SEC = 60          # 最长连续出光时间(秒), 到点自动关光
LASER_DEV_DEFAULT = "/dev/ir_laser"
_laser = {"fd": None, "dev": None, "on": False, "on_since": 0, "last": {},
          "replied": None}   # replied: 最近一次下发是否读到设备应答(None=尚无记录)


# ---- RS-485 收发方向控制 (与主程序 IRLaser::sendCommand 保持一致) ----
# CH347T 是半双工 RS-485: 必须由 RTS 驱动收发器方向, 否则数据发不上总线且收不到应答。
# 实测 ch343 驱动打开串口后 RTS 默认"置位"(发送态), 不做切换会长期占用总线。
# 参数取自 config/default.ini [serial], 与本地 Qt 程序同一数据源。
LASER_RS485_RTS = True           # 是否启用 RTS 方向控制
LASER_RS485_ACTIVE_LOW = False   # 反相电路(部分适配器 RTS 低电平为发送)


def _load_laser_serial_cfg():
    """从 config/default.ini 的 [serial] 读取 RS-485 方向控制参数"""
    global LASER_RS485_RTS, LASER_RS485_ACTIVE_LOW
    try:
        txt = open(CONFIG_INI, encoding="utf-8", errors="replace").read()

        def g(k, cur):
            m = re.search(rf"^{k}\s*=\s*(.+)$", txt, re.M)
            return m.group(1).strip() if m else cur
        LASER_RS485_RTS = str(g("rs485_rts", "true")).lower() in ("1", "true", "yes", "on")
        LASER_RS485_ACTIVE_LOW = str(
            g("rs485_rts_active_low", "false")).lower() in ("1", "true", "yes", "on")
    except Exception as e:  # noqa: BLE001
        print(f"[WARN] 读取 RS-485 串口配置失败, 使用默认: {e}", flush=True)


def _laser_set_rts(fd, tx):
    """切换 RS-485 收发方向: tx=True 发送态(驱动总线), False 接收态"""
    if not LASER_RS485_RTS:
        return
    level = (not tx) if LASER_RS485_ACTIVE_LOW else tx
    try:
        buf = array.array("i", [termios.TIOCM_RTS])
        fcntl.ioctl(fd, termios.TIOCMBIS if level else termios.TIOCMBIC, buf, True)
    except OSError as e:
        print(f"[WARN] 设置 RS-485 RTS 方向失败: {e}", flush=True)


def _laser_write(fd, frame):
    """发送一帧: RTS 拉到发送态 -> 写 -> tcdrain 等字节移出 -> 切回接收态

    与主程序 IRLaser::sendCommand() 的 setRequestToSend/ flush 流程等价。
    """
    _laser_set_rts(fd, True)
    try:
        os.write(fd, frame)
        try:
            termios.tcdrain(fd)
        except OSError:
            pass
    finally:
        _laser_set_rts(fd, False)


_load_laser_serial_cfg()      # 启动即读取一次, 供界面显示方向控制状态


def _pelco(cmd1, cmd2, d1=0, d2=0, addr=1):
    """Pelco_D 帧: FF addr cmd1 cmd2 d1 d2 checksum"""
    cs = (addr + cmd1 + cmd2 + d1 + d2) & 0xFF
    return bytes([0xFF, addr, cmd1, cmd2, d1, d2, cs])


def laser_open(dev=LASER_DEV_DEFAULT):
    with _lock:
        if _laser["fd"] is not None:
            return True, "串口已打开"
        try:
            fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(fd)
            a[0] = 0                                  # iflag: 原始模式
            a[1] = 0                                  # oflag
            a[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            a[3] = 0                                  # lflag
            a[4] = termios.B9600
            a[5] = termios.B9600
            a[6][termios.VMIN] = 0
            a[6][termios.VTIME] = 5
            termios.tcsetattr(fd, termios.TCSANOW, a)
            _laser["fd"] = fd
            _laser["dev"] = dev
            _load_laser_serial_cfg()
            _laser_set_rts(fd, False)   # 打开即置接收态, 避免长期占用 RS-485 总线
            return True, (f"已打开 {dev} @9600 8N1"
                          + (" + RS-485 RTS 方向控制" if LASER_RS485_RTS else ""))
        except OSError as e:
            return False, f"打开串口失败: {e}"


def laser_send(cmd1, cmd2, d1=0, d2=0, read_ms=300):
    """发送一帧并**闭环读回应答**。

    返回 (ok, msg, resp_hex):
      - 串口未打开 / 写入失败            -> ok=False;
      - 写入成功但 read_ms 内无任何回读数据 -> ok=False, msg="无应答(...)", resp=""
        (用于前端提示: 激光器可能断电/未接线, 指令实际未生效);
      - 收到任意回读数据                  -> ok=True。
    说明: 串口能被打开并不代表设备在线 —— 设备断电时串口仍在, 但不会有任何回读,
    因此必须用"是否有返回数据"来闭环判定, 避免显示"看起来执行成功"。
    """
    with _lock:
        fd = _laser["fd"]
        if fd is None:
            return False, "串口未打开", ""
        try:
            _laser_write(fd, _pelco(cmd1, cmd2, d1, d2))
        except OSError as e:
            return False, f"写入失败: {e}", ""
        try:
            time.sleep(read_ms / 1000.0)
            resp = os.read(fd, 64)
        except (BlockingIOError, OSError):
            resp = b""
        # 2026-09-24: 该型号**只对查询指令应答**, 控制指令(开/关光、亮度、角度、变焦等)
        # 不会回读任何数据; 因此不能用"控制指令无回读"判定设备离线(会在界面误报"无应答")。
        is_query = (cmd1, cmd2) in ((0x02, 0x01), (0x02, 0x03), (0x02, 0x05),
                                    (0x02, 0x0F), (0x09, 0x01), (0x05, 0x10))
        if resp or is_query:
            _laser["replied"] = bool(resp)
            _laser["replied_ms"] = time.time() * 1000
        if resp:
            return True, "ok", resp.hex(" ")
        if is_query:
            return False, "无应答(激光器可能断电/未接线/串口异常)", ""
        return True, "已下发(该型号控制指令不应答, 可用\"查询\"确认设备在线)", ""


def laser_off_internal():
    """内部关光(不校验 confirm)"""
    fd = _laser.get("fd")
    if fd is None:
        return
    try:
        _laser_write(fd, _pelco(0x01, 0x01, 0, 0))
    except OSError:
        pass
    _laser["on"] = False
    _laser["on_since"] = 0


def _laser_autooff():
    """最长出光时间到点自动关光"""
    time.sleep(LASER_MAX_ON_SEC)
    with _lock:
        if _laser["on"]:
            laser_off_internal()


def laser_close():
    with _lock:
        if _laser["fd"] is None:
            return True, "串口未打开"
        laser_off_internal()          # 先确保关光
        try:
            os.close(_laser["fd"])
        except OSError:
            pass
        _laser["fd"] = None
        return True, "已关闭串口(并已关光)"


def laser_status(probe=True, ttl_ms=3000):
    """状态; probe=True 时用一条**查询指令**探测设备是否在线(带 TTL, 避免频繁占串口)"""
    with _lock:
        now_ms = time.time() * 1000
        if probe and _laser["fd"] is not None \
                and now_ms - _laser.get("probe_ms", 0) >= ttl_ms:
            laser_send(0x02, 0x03, 0, 0, read_ms=250)   # 查询亮度: 有回读即为在线
            _laser["probe_ms"] = time.time() * 1000
        opened = _laser["fd"] is not None
        st = {
            "device": _laser.get("dev") or LASER_DEV_DEFAULT,
            "opened": opened,
            "on": _laser["on"],
            "on_elapsed": (round(time.time() - _laser["on_since"], 1)
                           if _laser["on"] and _laser["on_since"] else 0),
            "max_on_sec": LASER_MAX_ON_SEC,
            "exclusive": laser_state()["exclusive"],
            "controllable": laser_state()["controllable"],
            "last": _laser.get("last", {}),
            "replied": _laser.get("replied"),
            "replied_ms": _laser.get("replied_ms"),
            "rs485_rts": LASER_RS485_RTS,
            "rs485_active_low": LASER_RS485_ACTIVE_LOW,
        }
    return st


_orig_do_GET2 = Handler.do_GET
_orig_do_POST2 = Handler.do_POST


def _do_GET3(self):
    path = self.path.split("?")[0]
    if path == "/api/laser/status":
        return self._json(laser_status())
    if path == "/api/laser/query":
        # 依次查询: 开关 / 亮度 / 行程位置 / 出光角度 (闭环: 统计是否收到任何应答)
        out = {}
        replied = False
        for name, (c1, c2) in (("switch", (0x02, 0x01)), ("brightness", (0x02, 0x03)),
                               ("position", (0x02, 0x05)), ("angle", (0x09, 0x01))):
            ok, _, resp = laser_send(c1, c2, 0, 0, read_ms=300)
            out[name] = resp
            if resp:
                replied = True
        _laser["last"] = out
        return self._json({"ok": True, "queries": out, "replied": replied,
                           "detail": "" if replied
                                     else "无应答(激光器可能断电/未接线)"})
    if path == "/api/laser":
        st = laser_status()
        st["note"] = ("串口空闲, 网关可直控(开光无需二次确认, 不做自动关光; 光斑角度 2°~65°)"
                      ) if st["controllable"] else \
                     "串口被其它进程占用, 无法直控"
        return self._json(st)
    return _orig_do_GET2(self)


def _do_POST3(self):
    path = self.path.split("?")[0]
    if not path.startswith("/api/laser/"):
        return _orig_do_POST2(self)
    p = self._body_json()

    if path == "/api/laser/open":
        ok, msg = laser_open(str(p.get("dev") or LASER_DEV_DEFAULT))
        replied = None
        if ok:
            # 闭环探测: 串口能打开 ≠ 设备在线, 主动查询一次看是否有回读数据
            _, _, resp = laser_send(0x02, 0x01, 0, 0, read_ms=300)
            replied = bool(resp)
            msg += ("; 设备在线(有应答)" if replied
                    else "; 串口已开但设备无应答(请检查激光器电源/接线)")
        return self._json({"ok": ok, "detail": msg, "device_replied": replied})

    if path == "/api/laser/close":
        ok, msg = laser_close()
        return self._json({"ok": ok, "detail": msg})

    if path == "/api/laser/on":
        if not bool(p.get("confirm")):
            return self._json({"ok": False, "detail": "需要二次确认(confirm=true)"})
        if _laser["fd"] is None:
            return self._json({"ok": False, "detail": "串口未打开"})
        ok, msg, resp = laser_send(0x01, 0x01, 1, 0)
        if ok:
            _laser["on"] = True
            _laser["on_since"] = time.time()
            threading.Thread(target=_laser_autooff, daemon=True).start()
        _laser["last"] = {"cmd": "on", "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp,
                           "auto_off_sec": LASER_MAX_ON_SEC})

    if path == "/api/laser/off":
        with _lock:
            laser_off_internal()
        ok, msg, resp = laser_send(0x01, 0x01, 0, 0)
        _laser["last"] = {"cmd": "off", "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp})

    if path == "/api/laser/brightness":
        if "level" in p:
            v = max(0, min(255, int(float(p["level"]))))
            ok, msg, resp = laser_send(0x01, 0x03, v, 0)
            _laser["last"] = {"cmd": "set_brightness", "level": v, "resp": resp}
        else:
            up = int(p.get("step") or 1) > 0
            ok, msg, resp = laser_send(0x01, 0x02, 0 if up else 1, 0, read_ms=400)
            _laser["last"] = {"cmd": "brightness_step", "up": up, "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp})

    if path == "/api/laser/spot":
        if bool(p.get("stop")):
            ok, msg, resp = laser_send(0x00, 0x00, 0, 0)
            _laser["last"] = {"cmd": "stop", "resp": resp}
        else:
            d = int(p.get("dir") or 1)
            sp = max(0, min(255, int(p.get("speed") or 0)))
            ok, msg, resp = laser_send(0x00, 0x20 if d > 0 else 0x40, 0, sp)
            _laser["last"] = {"cmd": "spot", "dir": d, "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp})

    if path == "/api/laser/angle":
        d = int(p.get("dir") or 1)
        stp = max(1, min(255, int(p.get("step") or 8)))
        ok, msg, resp = laser_send(0x01, 0x04, 1 if d > 0 else 0, stp, read_ms=400)
        _laser["last"] = {"cmd": "angle", "dir": d, "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp})

    if path == "/api/laser/position":
        pos = max(0, min(0x4000, int(p.get("pos") or 0)))
        ok, msg, resp = laser_send(0x01, 0x05, (pos >> 8) & 0xFF, pos & 0xFF, read_ms=400)
        _laser["last"] = {"cmd": "position", "pos": pos, "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp})

    if path == "/api/laser/reset":
        ok, msg, resp = laser_send(0x01, 0x06, 0, 0, read_ms=400)
        _laser["last"] = {"cmd": "reset", "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp})

    if path == "/api/laser/query":
        # 依次查询: 开关 / 亮度 / 行程位置 / 出光角度 (闭环: 统计是否收到任何应答)
        out = {}
        replied = False
        for name, (c1, c2) in (("switch", (0x02, 0x01)), ("brightness", (0x02, 0x03)),
                               ("position", (0x02, 0x05)), ("angle", (0x09, 0x01))):
            ok, _, resp = laser_send(c1, c2, 0, 0, read_ms=300)
            out[name] = resp
            if resp:
                replied = True
        _laser["last"] = out
        return self._json({"ok": True, "queries": out, "replied": replied,
                           "detail": "" if replied
                                     else "无应答(激光器可能断电/未接线)"})

    return self._json({"ok": False, "detail": "unknown laser api"}, 404)


Handler.do_GET = _do_GET3
Handler.do_POST = _do_POST3




# ============================================================ [追加] 网关自带 TCP 控制服务器
_tcpsrv = {"sock": None, "running": False, "port": 8888, "clients": []}
_tcpsrv_lock = threading.Lock()
_last_angle = {}


def _tcpsrv_send(c, obj):
    try:
        c.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
    except Exception:  # noqa: BLE001
        pass


def tcpsrv_broadcast(obj):
    with _tcpsrv_lock:
        for c in list(_tcpsrv["clients"]):
            _tcpsrv_send(c, obj)


def tcpsrv_status():
    with _tcpsrv_lock:
        return {"running": _tcpsrv["running"], "port": _tcpsrv["port"],
                "clients": len(_tcpsrv["clients"])}


def tcpsrv_start(port=8888):
    with _tcpsrv_lock:
        if _tcpsrv["running"]:
            return True, "已在运行 (端口 %d)" % _tcpsrv["port"]
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("0.0.0.0", port))
            s.listen(8)
        except OSError as e:
            s.close()
            return False, ("端口 %d 无法监听: %s (若本地程序已启动 TCP, 请先在程序中停止)"
                           % (port, e))
        _tcpsrv.update(sock=s, running=True, port=port)
    threading.Thread(target=_tcpsrv_accept, daemon=True).start()
    return True, "已启动, 监听 0.0.0.0:%d" % port


def tcpsrv_stop():
    with _tcpsrv_lock:
        if not _tcpsrv["running"]:
            return True, "未在运行"
        _tcpsrv["running"] = False
        try:
            _tcpsrv["sock"].close()
        except Exception:  # noqa: BLE001
            pass
        for c in list(_tcpsrv["clients"]):
            try:
                c.close()
            except Exception:  # noqa: BLE001
                pass
        _tcpsrv["clients"] = []
    return True, "已停止"


def _tcpsrv_accept():
    while True:
        with _tcpsrv_lock:
            if not _tcpsrv["running"]:
                return
            s = _tcpsrv["sock"]
        try:
            c, addr = s.accept()
        except OSError:
            return
        with _tcpsrv_lock:
            _tcpsrv["clients"].append(c)
        threading.Thread(target=_tcpsrv_client, args=(c, addr), daemon=True).start()


def _tcpsrv_client(c, addr):
    c.settimeout(1.0)
    _tcpsrv_send(c, {"type": "welcome",
                     "message": "SubseaImagingSystem TCP Server (gateway)",
                     "timestamp": int(time.time() * 1000),
                     "usage": "zoom +/- | focus +/- | iris +/- | "
                              "ptz up|down|left|right [ms] | stop | status"})
    if _last_angle:
        _tcpsrv_send(c, _last_angle)
    buf = b""
    while True:
        with _tcpsrv_lock:
            if not _tcpsrv["running"]:
                break
        try:
            data = c.recv(1024)
        except socket.timeout:
            continue
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            cmd = line.decode("utf-8", "replace").strip()
            if cmd:
                _tcpsrv_handle(c, cmd)
    with _tcpsrv_lock:
        if c in _tcpsrv["clients"]:
            _tcpsrv["clients"].remove(c)
    try:
        c.close()
    except Exception:  # noqa: BLE001
        pass


def _tcpsrv_handle(c, line):
    p = line.split()
    cmd = p[0].lower()
    ts = int(time.time() * 1000)
    result, extra = "ok", ""
    if cmd in ("help", "?"):
        _tcpsrv_send(c, {"type": "ack", "cmd": line, "result": "help", "timestamp": ts,
                         "usage": "zoom +/- | focus +/- | iris +/- | "
                                  "ptz up|down|left|right [ms] | stop | status"})
        return
    if cmd == "status":
        st = camera_status()
        st.update({"type": "status", "timestamp": ts})
        _tcpsrv_send(c, st)
        return
    if cmd == "zoom" and len(p) > 1:
        if p[1] in ("+", "-"):
            spd = 40 if p[1] == "+" else -40
            ptz_move(0, 0, spd, 800)
        else:
            result = "bad-arg"
    elif cmd == "focus" and len(p) > 1:
        if p[1] in ("+", "-"):
            cur = 600
            try:
                cur = int(camera_status().get("focus_limited"))
            except Exception:  # noqa: BLE001
                pass
            idx = min(range(len(FOCUS_DIST_TABLE)),
                      key=lambda i: abs(FOCUS_DIST_TABLE[i] - cur))
            idx = max(0, min(len(FOCUS_DIST_TABLE) - 1, idx + (1 if p[1] == "+" else -1)))
            read_modify_write("/ISAPI/Image/channels/1", "FocusConfiguration",
                              "focusLimited", FOCUS_DIST_TABLE[idx])
    elif cmd == "iris" and len(p) > 1:
        if p[1] in ("+", "-"):
            cur = 400
            try:
                cur = int(camera_status().get("iris_level"))
            except Exception:  # noqa: BLE001
                pass
            idx = min(range(len(IRIS_TABLE)), key=lambda i: abs(IRIS_TABLE[i] - cur))
            idx = max(0, min(len(IRIS_TABLE) - 1, idx + (-1 if p[1] == "+" else 1)))
            isapi("PUT", "/ISAPI/Image/channels/1/exposure",
                  '<Exposure xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                  '<ExposureType>manual</ExposureType></Exposure>')
            isapi("PUT", "/ISAPI/Image/channels/1/iris",
                  '<Iris xmlns="http://www.hikvision.com/ver20/XMLSchema">'
                  '<IrisLevel>%d</IrisLevel></Iris>' % IRIS_TABLE[idx])
    elif cmd == "ptz" and len(p) > 1:
        d = p[1].lower()
        ms = 300
        if len(p) > 2:
            try:
                ms = int(p[2])
            except ValueError:
                ms = 300
        ms = max(50, min(5000, ms))
        sp = 40
        if d == "up":       ptz_move(0, sp, 0, ms)
        elif d == "down":   ptz_move(0, -sp, 0, ms)
        elif d == "left":   ptz_move(-sp, 0, 0, ms)
        elif d == "right":  ptz_move(sp, 0, 0, ms)
        else:               result = "bad-arg"
        if result == "ok":
            extra = ", \"duration_ms\": %d" % ms
    elif cmd == "stop":
        ptz_move(0, 0, 0, 0)
    else:
        result = "unknown-cmd"
    _tcpsrv_send(c, {"type": "ack", "cmd": line, "result": result, "timestamp": ts})


_orig_do_GET4 = Handler.do_GET
_orig_do_POST4 = Handler.do_POST


def _do_GET5(self):
    path = self.path.split("?")[0]
    if path == "/api/tcpserver":
        st = tcpsrv_status()
        st["prog_tcp"] = _tcp_state["connected"]     # 程序侧 8888(参考)
        st["prog_tcp_state"] = prog_tcp_state()      # connected | port_conflict | disconnected
        st["clients_external"] = st.get("clients", 0)  # 自连接已禁止, 即为真实外部客户端数
        return self._json(st)
    if path == "/api/tcp":
        st = tcpsrv_status()                          # 网页可启停的是网关自己的服务
        st["prog_tcp"] = _tcp_state["connected"]
        st["prog_tcp_state"] = prog_tcp_state()
        st["clients_external"] = st.get("clients", 0)
        return self._json(st)
    return _orig_do_GET4(self)


def _do_POST5(self):
    path = self.path.split("?")[0]
    if path == "/api/tcpserver/start":
        p = self._body_json()
        ok, msg = tcpsrv_start(int(p.get("port") or 8888))
        return self._json({"ok": ok, "detail": msg, **tcpsrv_status()})
    if path == "/api/tcpserver/stop":
        ok, msg = tcpsrv_stop()
        return self._json({"ok": ok, "detail": msg, **tcpsrv_status()})
    if path == "/api/report_detect":
        # 网页端把检测结果上报, 网关缓存并广播给 TCP 客户端(角度闭环)
        global _last_angle
        p = self._body_json()
        if p:
            _last_angle = dict(p)
            _last_angle["type"] = "angle_data"
            _last_angle.setdefault("timestamp", int(time.time() * 1000))
            tcpsrv_broadcast(_last_angle)
        return self._json({"ok": True})
    if path == "/api/laser/on":
        # 已按要求取消"二次确认"校验
        p = self._body_json()
        if _laser["fd"] is None:
            return self._json({"ok": False, "detail": "串口未打开"})
        ok, msg, resp = laser_send(0x01, 0x01, 1, 0)
        if ok:
            _laser["on"] = True
            _laser["on_since"] = time.time()      # 仅记录, 不再自动关断
        _laser["last"] = {"cmd": "on", "resp": resp}
        return self._json({"ok": ok, "detail": msg, "resp": resp})
    return _orig_do_POST4(self)


Handler.do_GET = _do_GET5
Handler.do_POST = _do_POST5


def _laser_autooff_disabled():
    """按要求: 取消"最长 60s 自动关光"。保留函数占位, 不再被调用。"""
    return



# =====================================================================
# 证据文件管理: 列表 / 磁盘容量 / 保留天数清理  (2026-09-24)
#   GET  /api/storage        保存路径 + 磁盘容量 + 保留策略 + 文件统计
#   GET  /api/evidence       证据文件列表(视频+抓图, 按时间倒序, 带 /api/file/xx 播放地址)
#   POST /api/retention      {days:N} 设置最长保存天数(0=不自动清理)并立即清理一次
#   POST /api/cleanup        立即按当前保留天数清理
#   POST /api/evidence/delete {name:"xxx.mp4"} 删除单个文件
# =====================================================================
import shutil

SETTINGS_FILE = "/home/lcfc/remote/gateway_settings.json"
_retention = {"days": 7}          # 最长保存天数; 0 = 不自动清理


def _load_settings():
    try:
        with open(SETTINGS_FILE, encoding="utf-8") as f:
            d = json.load(f)
        _retention["days"] = max(0, min(3650, int(d.get("retention_days", _retention["days"]))))
    except Exception:
        pass


def _save_settings():
    try:
        with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
            json.dump({"retention_days": _retention["days"]}, f, ensure_ascii=False, indent=1)
    except Exception as e:
        print(f"[WARN] 保存保留设置失败: {e}", flush=True)


def _fmt_size(n):
    n = float(n)
    for u in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or u == "TB":
            return f"{n:.0f}{u}" if u == "B" else f"{n:.1f}{u}"
        n /= 1024.0


def evidence_list(limit=500):
    """证据目录文件列表(视频/抓图), 按修改时间倒序"""
    out = []
    try:
        names = os.listdir(EVIDENCE_DIR)
    except OSError:
        return out
    for name in names:
        low = name.lower()
        if not low.endswith((".mp4", ".jpg", ".jpeg", ".png")):
            continue
        p = os.path.join(EVIDENCE_DIR, name)
        try:
            st = os.stat(p)
        except OSError:
            continue
        out.append({"name": name, "bytes": st.st_size, "mtime": int(st.st_mtime),
                    "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st.st_mtime)),
                    "kind": "video" if low.endswith(".mp4") else "image",
                    "url": "/api/file/" + name})
    out.sort(key=lambda x: x["mtime"], reverse=True)
    return out[:limit]


def storage_info():
    base = EVIDENCE_DIR if os.path.isdir(EVIDENCE_DIR) else "/"
    du = shutil.disk_usage(base)
    files = evidence_list(limit=100000)
    videos = [f for f in files if f["kind"] == "video"]
    return {"ok": True, "path": EVIDENCE_DIR,
            "disk_total": du.total, "disk_used": du.used, "disk_free": du.free,
            "disk_total_text": _fmt_size(du.total), "disk_free_text": _fmt_size(du.free),
            "disk_free_pct": round(du.free * 100.0 / du.total, 1) if du.total else 0,
            "file_count": len(files), "video_count": len(videos),
            "evidence_text": _fmt_size(sum(f["bytes"] for f in files)),
            "newest": files[0]["time"] if files else "", "oldest": files[-1]["time"] if files else "",
            "retention_days": _retention["days"],
            "recording": bool(_rec.get("proc") and _rec["proc"].poll() is None),
            "rec_file": os.path.basename(_rec.get("path") or "")}


def cleanup_evidence(days=None):
    """删除超过保留天数的证据文件; days=0 表示不清理。
    10 分钟内修改过的文件跳过(可能正在录制/写入)。"""
    d = _retention["days"] if days is None else max(0, int(days))
    if d <= 0:
        return {"ok": True, "deleted": 0, "freed": 0, "detail": "未设置最长保存天数(0=不自动清理)"}
    cutoff = time.time() - d * 86400
    now = time.time()
    deleted = freed = 0
    for f in evidence_list(limit=100000):
        if f["mtime"] >= cutoff or (now - f["mtime"]) < 600:
            continue
        try:
            os.remove(os.path.join(EVIDENCE_DIR, f["name"]))
            deleted += 1
            freed += f["bytes"]
        except OSError as e:
            print(f"[WARN] 清理失败 {f['name']}: {e}", flush=True)
    detail = f"按保留 {d} 天删除 {deleted} 个文件, 释放 {_fmt_size(freed)}"
    if deleted:
        print("[cleanup] " + detail, flush=True)
    return {"ok": True, "deleted": deleted, "freed": freed, "detail": detail}


def _cleanup_worker():
    """启动后清理一次, 之后每小时一次"""
    time.sleep(20)
    while True:
        try:
            cleanup_evidence()
        except Exception as e:
            print(f"[WARN] 定期清理异常: {e}", flush=True)
        time.sleep(3600)


_load_settings()


_orig_do_GET6 = Handler.do_GET
_orig_do_POST6 = Handler.do_POST


def _do_GET6(self):
    path = self.path.split("?")[0]
    if path == "/api/storage":
        return self._json(storage_info())
    if path == "/api/evidence":
        return self._json({"ok": True, "files": evidence_list(),
                           "path": EVIDENCE_DIR, "retention_days": _retention["days"]})
    return _orig_do_GET6(self)


def _do_POST6(self):
    path = self.path.split("?")[0]
    if path == "/api/retention":
        p = self._body_json()
        try:
            days = max(0, min(3650, int(p.get("days") or 0)))
        except Exception:
            return self._json({"ok": False, "detail": "天数无效"})
        _retention["days"] = days
        _save_settings()
        res = cleanup_evidence(days)
        print(f"[gateway] 最长保存天数 = {days} 天; 立即清理: {res['detail']}", flush=True)
        return self._json({"ok": True, "retention_days": days, "cleanup": res})
    if path == "/api/cleanup":
        return self._json(cleanup_evidence())
    if path == "/api/evidence/delete":
        p = self._body_json()
        name = os.path.basename(str(p.get("name") or ""))
        if not name:
            return self._json({"ok": False, "detail": "缺少文件名"})
        try:
            os.remove(os.path.join(EVIDENCE_DIR, name))
            print(f"[gateway] 删除证据文件 {name}", flush=True)
            return self._json({"ok": True, "detail": "已删除 " + name})
        except OSError as e:
            return self._json({"ok": False, "detail": f"删除失败: {e}"})
    return _orig_do_POST6(self)


Handler.do_GET = _do_GET6
Handler.do_POST = _do_POST6


# =====================================================================
# 热像仪真实温度 (ISAPI 全屏测温) —— 供 Web 鼠标探针使用 (2026-09-28)
#   GET /api/thermal/temp?x=<ix>&y=<iy>
#     -> {"ok":true,"x":..,"y":..,"temp":35.7,"w":640,"h":512,"ts":..}
#   数据来源: GET /ISAPI/Thermal/channels/1/thermometry/jpegPicWithAppendData?format=json
#             -> multipart: [application/json] + [image/pjpeg] + [application/octet-stream]
#             第三段为 640x512 个 float32(小端, °C) 的全屏温度矩阵(与热像视频帧同尺寸)。
#   说明: 单次抓取约 1.3MB, 这里做 ~150ms 缓存, 避免鼠标高频移动时反复抓取。
# =====================================================================
import array as _array


def load_thermal_cfg():
    """从程序配置读热像仪(测温相机)地址与账号, 与可见光相机相互独立"""
    ip, port, user, pw = "192.168.10.211", 80, "admin", "cisdi135"
    try:
        txt = open(CONFIG_INI, encoding="utf-8", errors="replace").read()

        def g(key, cur):
            m = re.search(rf"^{key}\s*=\s*(.+)$", txt, re.M)
            return m.group(1).strip() if m else cur

        ip = g("thermal_camera_ip", ip)
        port = int(g("thermal_camera_port", port))
        user = g("thermal_camera_user", user)
        pw = g("thermal_camera_pass", pw)
    except Exception as e:  # noqa: BLE001
        print(f"[WARN] 读热像仪配置失败, 使用默认: {e}", flush=True)
    return ip, port, user, pw


TH_IP, TH_PORT, TH_USER, TH_PW = load_thermal_cfg()
TH_BASE = f"http://{TH_IP}:{TH_PORT}"
_TH_URL = ("/ISAPI/Thermal/channels/1/thermometry/"
           "jpegPicWithAppendData?format=json")

_th_lock = threading.Lock()
_th_cache = {"ts": 0.0, "w": 0, "h": 0, "data": None}


def _thermal_fetch_matrix(timeout=6):
    """抓取并解析全屏温度矩阵; 返回 (w, h, array('f')) 或 None"""
    pm = urlreq.HTTPPasswordMgrWithDefaultRealm()
    pm.add_password(None, TH_BASE + "/", TH_USER, TH_PW)
    opener = urlreq.build_opener(urlreq.HTTPDigestAuthHandler(pm))
    req = urlreq.Request(TH_BASE + _TH_URL)
    with opener.open(req, timeout=timeout) as r:
        body = r.read()
    octet = None
    for pt in body.split(b"--boundary"):
        he = pt.find(b"\r\n\r\n")
        if he < 0:
            continue
        if b"application/octet-stream" in pt[:he]:
            data = pt[he + 4:]
            if data.endswith(b"\r\n"):
                data = data[:-2]
            octet = data
    if not octet or len(octet) % 4 != 0:
        return None
    w = 640
    h = len(octet) // 4 // w
    if h <= 0:
        return None
    arr = _array.array("f")
    arr.frombytes(octet)
    return w, h, arr


def thermal_matrix(cache_s=0.15):
    """带缓存的全屏温度矩阵; 返回 (w, h, array('f')) 或 None"""
    now = time.time()
    with _th_lock:
        if _th_cache["data"] is not None and now - _th_cache["ts"] < cache_s:
            return _th_cache["w"], _th_cache["h"], _th_cache["data"]
    try:
        got = _thermal_fetch_matrix()
    except Exception as e:  # noqa: BLE001
        print(f"[WARN] 热像仪测温抓取失败: {e}", flush=True)
        got = None
    with _th_lock:
        if got is not None:
            _th_cache.update({"ts": time.time(), "w": got[0], "h": got[1], "data": got[2]})
        if _th_cache["data"] is None:
            return None
        return _th_cache["w"], _th_cache["h"], _th_cache["data"]


# =====================================================================
# 相机系统时间校准 (ISAPI /ISAPI/System/time) —— 网页端每次连接时自动调用
#   GET /api/camera/time/sync?which=both|thermal|visible  (默认 both)
#   策略: GET 读时间文档(保留 timeZone/timeMode/命名空间) -> 仅替换 <localTime>
#         -> 原样 PUT 回写 -> GET 读回校验, 返回设备时间与本机偏差(ms)。
#   说明: 对齐 Qt 程序 documents/相机自动校时说明.md 的"最小影响原则"。
# =====================================================================
_time_sync_lock = threading.Lock()


def _local_time_iso(offset_ms=0):
    t = time.time() + (offset_ms / 1000.0)
    lt = time.localtime(t)
    off = -(time.altzone if (lt.tm_isdst and time.daylight) else time.timezone)
    sign = "+" if off >= 0 else "-"
    a = abs(int(off))
    return time.strftime("%Y-%m-%dT%H:%M:%S", lt) + "%s%02d:%02d" % (sign, a // 3600, (a % 3600) // 60)


def _isapi_time_sync_one(which):
    import datetime as _dt
    if which == "visible":
        ip, port, user, pw = CAM_IP, CAM_PORT, CAM_USER, CAM_PW
    else:
        ip, port, user, pw = TH_IP, TH_PORT, TH_USER, TH_PW
    base = f"http://{ip}:{port}"
    pm = urlreq.HTTPPasswordMgrWithDefaultRealm()
    pm.add_password(None, base + "/", user, pw)
    opener = urlreq.build_opener(urlreq.HTTPDigestAuthHandler(pm))
    url = base + "/ISAPI/System/time"

    # 1) 读文档作为模板(保留时区/对时模式/命名空间, 只改 localTime)
    with opener.open(urlreq.Request(url), timeout=8) as r:
        doc = r.read().decode("utf-8", "replace")

    # 2) 闭环迭代: 写入 -> 读回 -> 修正补偿(补偿量=HTTP 往返 + 设备应用延迟), 目标偏差 < 500ms
    comp_ms = 0
    wrote = ""
    dev = ""
    delta_ms = None
    for _ in range(3):
        wrote = _local_time_iso(comp_ms)
        body = re.sub(r"<localTime>.*?</localTime>",
                      f"<localTime>{wrote}</localTime>", doc, count=1)
        req = urlreq.Request(url, data=body.encode("utf-8"), method="PUT")
        req.add_header("Content-Type", "application/xml")
        with opener.open(req, timeout=8) as r:
            r.read()
        with opener.open(urlreq.Request(url), timeout=8) as r:
            back = r.read().decode("utf-8", "replace")
        m = re.search(r"<localTime>(.*?)</localTime>", back)
        dev = m.group(1) if m else ""
        delta_ms = None
        try:
            dv = _dt.datetime.strptime(dev[:19], "%Y-%m-%dT%H:%M:%S")
            delta_ms = int((dv - _dt.datetime.now()).total_seconds() * 1000)
        except Exception:  # noqa: BLE001
            break
        if abs(delta_ms) <= 500:
            break
        comp_ms -= delta_ms
    return {"ok": True, "which": which, "device": dev, "wrote": wrote, "delta_ms": delta_ms}


def _isapi_time_sync(which="both"):
    """校准相机时间; 返回逐台结果。加锁串行, 避免并发重复写入。"""
    targets = ["thermal", "visible"] if which in ("both", "", None) else [which]
    out = {}
    with _time_sync_lock:
        for t in targets:
            try:
                out[t] = _isapi_time_sync_one(t)
            except Exception as e:  # noqa: BLE001
                out[t] = {"ok": False, "which": t, "detail": str(e)}
    return {"ok": any(v.get("ok") for v in out.values()), "results": out}


_orig_do_GET7 = Handler.do_GET


def _do_GET7(self):
    path = self.path.split("?")[0]
    # ---- 相机校时: 网页端每次连接相机后调用 ----
    if path == "/api/camera/time/sync":
        qs = {}
        if "?" in self.path:
            for kv in self.path.split("?", 1)[1].split("&"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    qs[k] = v
        res = _isapi_time_sync(qs.get("which", "both"))
        try:
            for k, v in res.get("results", {}).items():
                if v.get("ok"):
                    print(f"[gateway] {k} 校时成功: 设备 {v.get('device')} 偏差 {v.get('delta_ms')} ms", flush=True)
                else:
                    print(f"[gateway] {k} 校时失败: {v.get('detail')}", flush=True)
        except Exception:  # noqa: BLE001
            pass
        return self._json(res)
    if path in ("/api/thermal/temp", "/api/thermal/matrix"):
        qs = {}
        if "?" in self.path:
            for kv in self.path.split("?", 1)[1].split("&"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    qs[k] = v

        m = thermal_matrix()
        if not m:
            return self._json({"ok": False, "detail": "未获取到测温数据"})
        w, h, arr = m

        # ---- 整幅温度矩阵(降采样, 二进制) ----
        #   GET /api/thermal/matrix?step=4 -> body: ow*oh 个 float32(小端, °C)
        #   尺寸/步长由响应头 X-Width / X-Height / X-Step 给出。
        #   用途: 前端"本地查表"取任意像素/目标框的温度, 避免鼠标高频移动时逐点请求,
        #         且数值即相机实测(不再用"灰度->温度"近似)。
        if path == "/api/thermal/matrix":
            try:
                step = int(qs.get("step", "4") or 4)
            except Exception:  # noqa: BLE001
                step = 4
            step = max(1, min(16, step))
            ow, oh = w // step, h // step
            if ow <= 0 or oh <= 0:
                return self._json({"ok": False, "detail": "降采样参数无效"})
            out = _array.array("f")
            for oy in range(oh):                     # 最近邻抽样: 计算量=输出像素数
                base = (oy * step) * w
                out.extend(arr[base:base + ow * step:step])
            body = out.tobytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("X-Width", str(ow))
            self.send_header("X-Height", str(oh))
            self.send_header("X-Step", str(step))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # ---- 单点接口(兼容保留) ----
        try:
            ix = int(qs.get("x", "-1"))
            iy = int(qs.get("y", "-1"))
        except Exception:  # noqa: BLE001
            return self._json({"ok": False, "detail": "坐标无效"})
        if ix < 0 or iy < 0 or ix >= w or iy >= h:
            return self._json({"ok": False, "detail": "坐标越界"})
        return self._json({"ok": True, "x": ix, "y": iy,
                           "temp": round(float(arr[iy * w + ix]), 2),
                           "w": w, "h": h, "ts": int(time.time() * 1000)})
    return _orig_do_GET7(self)


Handler.do_GET = _do_GET7
print(f"[gateway] 热像仪温度接口已启用: {TH_BASE}{_TH_URL.split('?')[0]}", flush=True)


if __name__ == "__main__":
    os.makedirs(EVIDENCE_DIR, exist_ok=True)
    threading.Thread(target=prog_tcp_worker, daemon=True).start()
    threading.Thread(target=_cleanup_worker, daemon=True).start()   # 超期证据定期清理
    print(f"[gateway] 相机 {CAM_IP}:{CAM_PORT} 用户 {CAM_USER}", flush=True)
    print(f"[gateway] RTSP 出口 {GO2RTC_RTSP}  证据目录 {EVIDENCE_DIR}", flush=True)
    print(f"[gateway] 最长保存天数 {_retention['days']} 天(0=不自动清理), 设置文件 {SETTINGS_FILE}", flush=True)
    print(f"[gateway] 监听 :{PORT}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()