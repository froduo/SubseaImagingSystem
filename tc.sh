#!/bin/bash
# 水下成像 - 可见光硬转码 (H.265 -> H.264), 供任意浏览器播放
# 用法: tc.sh 1080 | tc.sh 4k
set -u
MODE="${1:-1080}"
RTSP_BASE="rtsp://admin:cisdi135@192.168.10.212:554/Streaming/Channels"

case "$MODE" in
  1080)
    SRC="$RTSP_BASE/103"          # 1080p H.265 第三码流
    PORT=5556
    BITRATE=4000000
    PEAK=5000000
    ;;
  4k)
    SRC="$RTSP_BASE/101"          # 4K H.265 主码流
    PORT=5557
    BITRATE=16000000
    PEAK=18000000
    ;;
  *)
    echo "用法: $0 1080|4k" >&2
    exit 2
    ;;
esac

echo "[tc] mode=$MODE src=$SRC -> udp://127.0.0.1:$PORT ${BITRATE}bps"
exec gst-launch-1.0 -e \
  rtspsrc location="$SRC" latency=0 protocols=tcp drop-on-latency=true \
    buffer-mode=0 ntp-sync=false do-retransmission=false ! \
  rtph265depay ! h265parse config-interval=-1 ! \
  nvv4l2decoder enable-max-performance=1 disable-dpb=true ! \
  nvvidconv ! \
  nvv4l2h264enc bitrate=$BITRATE peak-bitrate=$PEAK control-rate=1 \
    preset-level=1 profile=2 insert-sps-pps=true idrinterval=25 iframeinterval=25 ! \
  h264parse config-interval=-1 ! \
  mpegtsmux ! udpsink host=127.0.0.1 port=$PORT sync=false
