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


def record_start(stream="vis1080"):
    with _lock:
        if _rec["proc"] and _rec["proc"].poll() is None:
            return False, "已在录像中"
        os.makedirs(EVIDENCE_DIR, exist_ok=True)
        name = f"remote_{stream}_{time.strftime('%Y%m%d_%H%M%S')}.mp4"
        path = os.path.join(EVIDENCE_DIR, name)
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp",
               "-i", f"{GO2RTC_RTSP}/{stream}", "-c", "copy", "-movflags", "+faststart",
               path]
        try:
            p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as e:  # noqa: BLE001
            return False, f"启动 ffmpeg 失败: {e}"
        _rec.update(proc=p, path=path, t0=time.time())
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
        name = os.path.basename(_rec.get("path") or "")
        _rec.update(proc=None)
        return True, name


# ---------------------------------------------------------------- 8888 桥接 (SSE)
_sse_clients = []
_sse_lock = threading.Lock()
_tcp_state = {"connected": False, "last": ""}


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


def prog_tcp_worker():
    """持续尝试连接现有程序的 TCP 8888, 把 angle_data 等推送转给 SSE"""
    while True:
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
            ok, st, resp = ptz_move(int(p.get("pan") or 0), int(p.get("tilt") or 0),
                                    int(p.get("zoom") or 0), ms)
            return self._json({"ok": ok, "http": st, "ms": ms, "detail": resp[:120]})

        if path == "/api/ptzstop":
            ok, st, resp = ptz_move(0, 0, 0, 0)
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
        if not os.path.isfile(path):
            return self._json({"ok": False, "detail": "not found"}, 404)
        with open(path, "rb") as f:
            data = f.read()
        ctype = "image/jpeg" if path.endswith(".jpg") else "video/mp4"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

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
import termios

LASER_MAX_ON_SEC = 60          # 最长连续出光时间(秒), 到点自动关光
LASER_DEV_DEFAULT = "/dev/ir_laser"
_laser = {"fd": None, "dev": None, "on": False, "on_since": 0, "last": {}}


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
            return True, f"已打开 {dev} @9600 8N1"
        except OSError as e:
            return False, f"打开串口失败: {e}"


def laser_send(cmd1, cmd2, d1=0, d2=0, read_ms=250):
    """发送一帧并尝试读回应答(读回失败不影响下发)"""
    with _lock:
        fd = _laser["fd"]
        if fd is None:
            return False, "串口未打开", ""
        try:
            os.write(fd, _pelco(cmd1, cmd2, d1, d2))
        except OSError as e:
            return False, f"写入失败: {e}", ""
        try:
            time.sleep(read_ms / 1000.0)
            resp = os.read(fd, 64)
        except (BlockingIOError, OSError):
            resp = b""
        return True, "ok", resp.hex(" ")


def laser_off_internal():
    """内部关光(不校验 confirm)"""
    fd = _laser.get("fd")
    if fd is None:
        return
    try:
        os.write(fd, _pelco(0x01, 0x01, 0, 0))
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


def laser_status():
    with _lock:
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
        }
    return st


_orig_do_GET2 = Handler.do_GET
_orig_do_POST2 = Handler.do_POST


def _do_GET3(self):
    path = self.path.split("?")[0]
    if path == "/api/laser/status":
        return self._json(laser_status())
    if path == "/api/laser/query":
        out = {}
        for name, (c1, c2) in (("switch", (0x02, 0x01)), ("brightness", (0x02, 0x03)),
                               ("position", (0x02, 0x05)), ("angle", (0x09, 0x01))):
            ok, _, resp = laser_send(c1, c2, 0, 0, read_ms=300)
            out[name] = resp
        _laser["last"] = out
        return self._json({"ok": True, "queries": out})
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
        return self._json({"ok": ok, "detail": msg})

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
        # 依次查询: 开关 / 亮度 / 行程位置 / 出光角度
        out = {}
        for name, (c1, c2) in (("switch", (0x02, 0x01)), ("brightness", (0x02, 0x03)),
                               ("position", (0x02, 0x05)), ("angle", (0x09, 0x01))):
            ok, _, resp = laser_send(c1, c2, 0, 0, read_ms=300)
            out[name] = resp
        _laser["last"] = out
        return self._json({"ok": True, "queries": out})

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
        return self._json(st)
    if path == "/api/tcp":
        st = tcpsrv_status()                          # 网页可启停的是网关自己的服务
        st["prog_tcp"] = _tcp_state["connected"]
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


if __name__ == "__main__":
    os.makedirs(EVIDENCE_DIR, exist_ok=True)
    threading.Thread(target=prog_tcp_worker, daemon=True).start()
    print(f"[gateway] 相机 {CAM_IP}:{CAM_PORT} 用户 {CAM_USER}", flush=True)
    print(f"[gateway] RTSP 出口 {GO2RTC_RTSP}  证据目录 {EVIDENCE_DIR}", flush=True)
    print(f"[gateway] 监听 :{PORT}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()