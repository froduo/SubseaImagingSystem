#!/bin/bash
# ============================================================
# 在 Jetson 桌面上观看视频流 —— 使用 NVDEC 硬件解码 + 零拷贝输出
# ------------------------------------------------------------
# 为什么需要它：Jetson 上 CPU 软解 4K H.265 只有 ~20fps(0.82x 实时)，必然卡；
#   VLC 3.0.9 在这台机器上没有可用硬解后端(omxil 会 Bus error)，
#   mpv/ffplay 的 v4l2m2m 因缺少 /dev/video* 也不可用。
#   而 GStreamer 的 nvv4l2decoder 走 /dev/nvhost-nvdec(NVDEC)，
#   4K 硬解 CPU 仅约 13%，配合 nveglglessink 零拷贝直接上屏。
#
# 用法:
#   ./watch.sh                 # 默认看第三码流 1080p(vis1080)
#   ./watch.sh vis4k           # 4K 主码流(H.265 直转)
#   ./watch.sh vis102          # 子码流 704x576
#   ./watch.sh vis1080h264     # 1080p H.264 转码流
#   ./watch.sh thermal         # 热像仪
#   ./watch.sh rtsp://admin:cisdi135@192.168.10.212:554/Streaming/Channels/101   # 相机直连
#
# 可选环境变量:
#   SINK=nv3dsink      # 换显示后端(nveglglessink 有问题时改用 nv3dsink / xvimagesink)
#   CODEC=h264|h265    # 强制解复用器(默认按流名/地址自动判断)
#   DISPLAY=:0         # 默认 :0
#
# 注意: 本脚本用 "后台启动 + trap + wait" 而不是 exec，
#       这样父进程(如 Qt 程序)向本脚本发 SIGTERM 时，trap 会一并结束 gst-launch，
#       不会残留播放窗口。
# ============================================================
set -u

ARG="${1:-vis1080}"
RTSP_BASE="${RTSP_BASE:-rtsp://127.0.0.1:8554}"

case "$ARG" in
  rtsp://*|rtsp:*) SRC="$ARG" ;;          # 已是完整地址
  *)               SRC="$RTSP_BASE/$ARG" ;;
esac

# ---- 编码判定(H.265 还是 H.264) ----
if [ -n "${CODEC:-}" ]; then
  C="$CODEC"
else
  C="h265"                                                # 默认: 可见光直转流为 H.265
  case "$ARG" in
    *h264*|*thermal*) C="h264" ;;                         # 转码流 / 热像仪 = H.264
  esac
  case "$ARG" in
    *192.168.10.211*) C="h264" ;;                         # 热像仪相机直连
  esac
fi

if [ "$C" = "h264" ]; then
  DEPAY="rtph264depay ! h264parse config-interval=-1"
else
  DEPAY="rtph265depay ! h265parse config-interval=-1"
fi

export DISPLAY="${DISPLAY:-:0}"
SINK="${SINK:-nveglglessink}"

echo "[watch] 源      : $SRC"
echo "[watch] 编码    : $C"
echo "[watch] 显示    : $SINK   DISPLAY=$DISPLAY"
echo "[watch] 退出    : 关闭窗口 / Ctrl+C"
echo

# ---- 后台启动 + trap: 收到 SIGTERM/INT 时一并结束 gst-launch(避免残留窗口) ----
gst-launch-1.0 -e \
  rtspsrc location="$SRC" latency=0 timeout=10000000000 \
          protocols=tcp drop-on-latency=true buffer-mode=0 \
          ntp-sync=false do-retransmission=false \
  ! $DEPAY \
  ! nvv4l2decoder enable-max-performance=1 disable-dpb=true \
  ! nvvidconv \
  ! "$SINK" sync=false &
GPID=$!

cleanup() {
  kill "$GPID" 2>/dev/null
  wait "$GPID" 2>/dev/null
}
trap cleanup EXIT INT TERM

wait "$GPID"
