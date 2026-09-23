#include "visiblecamera.h"
#include "logger.h"
#include <QBuffer>
#include <QUrl>
#include <QNetworkRequest>
#include <QAuthenticator>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QDateTime>
#include <QFileInfo>
#include <csignal>   // 录像结束时向 ffmpeg 发 SIGINT 以优雅收尾 mp4

// ============================================================
// iDS-2ZMN2312S 实测可用的 ISAPI 控制端点 (2026-09)
//   PUT /ISAPI/PTZCtrl/channels/1/continuous   变焦: zoom -100..100 (0=停止)
//   GET /ISAPI/PTZCtrl/channels/1/status       回读: absoluteZoom 10..230
//   PUT /ISAPI/Image/channels/1/exposure       ExposureType: auto/manual/IrisFirst/ShutterFirst
//   PUT /ISAPI/Image/channels/1/iris           IrisLevel: 160..2200 (数值越大画面越暗)
//   PUT /ISAPI/Image/channels/1/gain           GainLevel/GainLimit: 0..100
//   PUT /ISAPI/Image/channels/1 (整篇文档)      含 FocusConfiguration/focusStyle 对焦模式
// 不可用(返回 404): /ISAPI/Image/channels/1/{focus, zoom, settings}
// 实测结论: 快门(ShutterLevel)对该机芯无实际效果, 亮度须由 光圈 + 增益 控制
// ============================================================
static const int ZOOM_SPEED = 60;          // PTZ 连续变焦速率 (1..100)
static const int ZOOM_STOP_TIMEOUT_MS = 3000;  // 手动变焦安全停止超时
// 闭环变焦总超时: 原为 15s, 过长的闭环会让镜头在反馈异常时一路冲到最大倍率,
// 这里收紧到 6s 并配合"二次确认停止", 从根本上消除"变焦几秒后自行回到最大焦距"的现象。
static const int ZOOM_CLOSED_LOOP_TIMEOUT_MS = 6000;   // 闭环变焦总超时
static const int ZOOM_CLOSED_LOOP_STEP_MS = 300;       // 闭环轮询间隔
static const int EXPOSURE_DEBOUNCE_MS = 150;   // 曝光/光圈/增益滑条去抖

// 变焦位置(absoluteZoom)空间与本机倍率对应: 10..230 对应 1.0×..23.0×
static const int ZOOM_POS_MIN = 10;
static const int ZOOM_POS_MAX = 230;
static const int ZOOM_STEP_UNITS = 10;      // 单击一次变焦按钮的步进量 (=1.0×)
static const int ZOOM_AROUND_TOLERANCE = 2; // 步进到达判定容差
static const int ZOOM_STEP_SPEED = 35;      // 步进变焦速度(低于连续变焦, 减小过冲)
static const int ZOOM_STEP_POLL_MS = 120;   // 步进闭环轮询间隔(越密过冲越小)

// 该机芯可用的 IrisLevel 档位 (实测: 数值越大画面越暗)
static const int IRIS_TABLE[] = {160, 200, 240, 280, 340, 400, 480, 560,
                                 680, 960, 1100, 1400, 1600, 1900, 2200};
static const int IRIS_TABLE_N = static_cast<int>(sizeof(IRIS_TABLE) / sizeof(IRIS_TABLE[0]));

// 该机芯"最小聚焦距离"档位: 取自设备 capabilities 中 FocusConfiguration/focusLimited 的 opt 列表
// (10/30cm, 1.0/1.5/3.0/6.0/10/20m, 65535=无限远); 该机芯没有"聚焦±步进"接口,
// 官方网页的做法就是"对焦模式 + 最小聚焦距离档位", 此处按档位步进实现 聚焦±
static const int FOCUS_DIST_TABLE[] = {10, 30, 100, 150, 300, 600, 1000, 2000, 65535};
static const int FOCUS_DIST_N = static_cast<int>(sizeof(FOCUS_DIST_TABLE) / sizeof(FOCUS_DIST_TABLE[0]));

// 找到最接近当前值的 IrisLevel 档位下标
static int irisTableIndexOf(int level)
{
    int best = 0, bestDiff = qAbs(level - IRIS_TABLE[0]);
    for (int i = 1; i < IRIS_TABLE_N; ++i) {
        const int d = qAbs(level - IRIS_TABLE[i]);
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    return best;
}

// 找到最接近当前值的聚焦距离档位下标
static int focusTableIndexOf(int v)
{
    int best = 0, bestDiff = qAbs(v - FOCUS_DIST_TABLE[0]);
    for (int i = 1; i < FOCUS_DIST_N; ++i) {
        const int d = qAbs(v - FOCUS_DIST_TABLE[i]);
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    return best;
}

// 滑条 0(最暗) -> IrisLevel 2200 ; 滑条 100(最亮) -> IrisLevel 160
int VisibleCamera::irisLevelFromSlider(int v)
{
    v = qBound(0, v, 100);
    const int idx = (IRIS_TABLE_N - 1) - qRound(v * (IRIS_TABLE_N - 1) / 100.0);
    return IRIS_TABLE[qBound(0, idx, IRIS_TABLE_N - 1)];
}

int VisibleCamera::irisSliderFromLevel(int level)
{
    const int idx = irisTableIndexOf(level);
    return qBound(0, qRound((IRIS_TABLE_N - 1 - idx) * 100.0 / (IRIS_TABLE_N - 1)), 100);
}

QString VisibleCamera::focusLimitedLabel(int v)
{
    if (v == 65535) return QStringLiteral("无限远");
    if (v == 0)     return QStringLiteral("自动");
    if (v >= 100)   return QString("%1m").arg(v / 100.0, 0, 'f', 1);
    return QString("%1cm").arg(v);
}

// OpenCV timeout property fallback for older versions
#ifndef CAP_PROP_OPEN_TIMEOUT_MSEC
#define CAP_PROP_OPEN_TIMEOUT_MSEC 53
#endif
#ifndef CAP_PROP_READ_TIMEOUT_MSEC
#define CAP_PROP_READ_TIMEOUT_MSEC 54
#endif

static const int MAX_RECONNECT_ATTEMPTS = 10;
static const int CONNECTION_TIMEOUT_MS = 10000;   // 10s connect timeout
static const int READ_TIMEOUT_MS = 10000;          // 10s read timeout
static const int RECONNECT_DELAY_MS = 3000;        // 3s between reconnects

// ---- 低延时/健壮性参数 (2026-09-22 "4K 零延时"改造) ----
// 实测: 原实现"一帧空即重连"导致每约 2 分钟断一次流, 而相机 GOP=50 @25fps,
// 重连后最长要等 2 秒才收到 I 帧 -> 表现为周期性卡顿/延时骤增。
// 改为容错: 偶发空帧只跳过, 连续多次或长时间无帧才判定链路断开。
static const int EMPTY_FRAME_TOLERANCE = 20;      // 连续空帧达到该次数才重连(约 0.5s)
static const int STALL_RECONNECT_MS   = 2500;     // 完全无新帧超过该时长才重连
// 界面确认超时: 若界面线程被 4K 渲染占满/卡死, 背压会永久置位使采集停摆,
// 这里加超时自愈, 保证采集端不会因消费端异常而彻底冻结。
static const int BACKPRESSURE_TIMEOUT_MS = 2000;

// ============================================================
// VisibleCameraWorker
// ============================================================
VisibleCameraWorker::VisibleCameraWorker(QObject *parent)
    : QObject(parent)
{
}

VisibleCameraWorker::~VisibleCameraWorker()
{
    if (m_capture.isOpened()) {
        m_capture.release();
    }
}

void VisibleCameraWorker::setRtspUrl(const QString &url)
{
    m_rtspUrl = url;
}

void VisibleCameraWorker::setPreviewSize(int w, int h)
{
    m_previewW = w;
    m_previewH = h;
}

void VisibleCameraWorker::requestStop()
{
    m_stopRequested = true;
}

// 由界面线程在消费完一帧后调用, 解除背压让采集循环可以投递下一帧
void VisibleCameraWorker::acknowledgeFrame()
{
    m_frameInFlight.store(false, std::memory_order_release);
}

// ============================================================
// 硬解链路: GStreamer + nvv4l2decoder (Jetson Xavier NX 内置 NVDEC)
//   实测(2026-09, iDS-2ZMN2312S 主码流 4K H.265):
//     FFMPEG 软解 全分辨率           : 约  5.8 fps  (界面卡顿, 原实现默认走此路)
//     nvv4l2decoder 硬解 全分辨率     : 约 16.6 fps
//     nvv4l2decoder + GPU 缩放 1080p : 约 26.9 fps  (若需要可经 setPreviewSize 启用)
//   当前默认全分辨率硬解输出(满足"选择哪路码流就显示哪路码流"的一致性要求),
//   如需更高帧率可调用 setPreviewSize(1920,1080) 启用 GPU 缩放预览。
//
// ---- 延迟优化(2026-09-20, 修复"4K 预览延迟 20 秒以上") ----
//   根因: 1) appsink 未限制队列 -> 默认 max-buffers=200 且 drop=false,
//            解码(=约16fps) 追不上相机 25fps 时缓冲持续积压,
//            积压 160 帧即约 20 秒延迟, 且延迟随时间越来越大;
//         2) videoconvert 在 CPU 上做 4K BGRx->BGR 转换(每帧读写各 24MB),
//            是掉帧的主要开销; nvvidconv 已能在 GPU 内输出 BGRx;
//         3) rtspsrc 默认 buffer-mode=auto 会 Slave 到相机时钟, 引入同步等待。
//   处理: max-buffers=1 drop=true(永远只取最新帧) + 去掉 videoconvert(BGRx 直达)
//         + buffer-mode=0/ntp-sync=false/do-retransmission=false(不等时钟、不重传)
//         + disable-dpb=true(减少解码器参考帧缓存)。
// ============================================================
bool VisibleCameraWorker::openHardwareStream()
{
    const long long timeoutNs = static_cast<long long>(CONNECTION_TIMEOUT_MS) * 1000000; // ms -> ns
    const bool scaleOnGpu = (m_previewW > 0 && m_previewH > 0);
    const QString scaleCaps = scaleOnGpu
        ? QString("video/x-raw,width=%1,height=%2,format=BGRx").arg(m_previewW).arg(m_previewH)
        : QStringLiteral("video/x-raw,format=BGRx");

    // 该机芯各码流均为 H.265; 保留 H.264 分支以兼容其他机型
    const QStringList codecs = {QStringLiteral("h265"), QStringLiteral("h264")};
    for (const QString &codec : codecs) {
        const bool isH265 = (codec == QStringLiteral("h265"));
        // 说明:
        //   - buffer-mode=0        : 不做接收端时钟 Slave, 避免按相机时钟缓存而累积延迟
        //   - ntp-sync=false       : 不做 NTP 时钟对齐
        //   - do-retransmission=false: TCP 传输本身可靠, 关闭 RTP 重传可避免重传造成的抖动延迟
        //   - disable-dpb=true     : 解码器不保留额外的参考帧缓存(低延迟优先)
        //   - appsink max-buffers=1 drop=true: 队列深度恒为 1, 永远只取最新帧
        //     (这是消除"延迟随时间增长"的关键: 原来默认缓冲 200 帧)
        const QString pipeline = QString(
            "rtspsrc location=%1 latency=0 timeout=%2 protocols=tcp drop-on-latency=true "
            "buffer-mode=0 ntp-sync=false do-retransmission=false ! "
            "%3 ! %4 ! nvv4l2decoder enable-max-performance=1 disable-dpb=true ! nvvidconv ! %5 ! "
            // enable-last-sample=false: 不保留上一帧引用, 少占用一个 4K 缓冲, 也让
            // appsink 始终只持有最新帧(配合 max-buffers=1 drop=true 保证"只取最新")
            "appsink max-buffers=1 drop=true sync=false enable-last-sample=false")
            .arg(m_rtspUrl, QString::number(timeoutNs),
                 isH265 ? QStringLiteral("rtph265depay") : QStringLiteral("rtph264depay"),
                 isH265 ? QStringLiteral("h265parse")    : QStringLiteral("h264parse"),
                 scaleCaps);
        try {
            LOG_INFO(QString("Visible camera: 尝试硬解(%1) pipeline: %2")
                         .arg(codec.toUpper(), pipeline));
            m_capture.open(pipeline.toStdString(), cv::CAP_GSTREAMER);
        } catch (const std::exception &e) {
            LOG_ERROR(QString("Visible camera: 硬解(%1)异常: %2")
                          .arg(codec.toUpper(), QString::fromUtf8(e.what())));
            m_capture.release();
        } catch (...) {
            LOG_ERROR(QString("Visible camera: 硬解(%1)未知异常").arg(codec.toUpper()));
            m_capture.release();
        }
        if (m_capture.isOpened()) {
            m_decoderDesc = QString("硬解 nvv4l2decoder(%1)%2")
                                .arg(codec.toUpper(),
                                     scaleOnGpu ? QString(", GPU缩放 %1x%2").arg(m_previewW).arg(m_previewH)
                                                : QStringLiteral(", 原分辨率"));
            LOG_INFO(QString("Visible camera: 硬解链路已启用 -> %1").arg(m_decoderDesc));
            emit decoderChanged(m_decoderDesc);
            return true;
        }
    }
    return false;
}

// 软解后备链路: FFMPEG(CPU), 仅在硬解不可用时使用(4K 时约 6fps, 预览会卡顿)
bool VisibleCameraWorker::openSoftStream()
{
    try {
        LOG_WARN("Visible camera: 尝试回退 FFMPEG 软解...");
        m_capture.set(CAP_PROP_OPEN_TIMEOUT_MSEC, CONNECTION_TIMEOUT_MS);
        m_capture.set(CAP_PROP_READ_TIMEOUT_MSEC, READ_TIMEOUT_MS);
        m_capture.open(m_rtspUrl.toStdString(), cv::CAP_FFMPEG);
    } catch (const std::exception &e) {
        LOG_ERROR(QString("Visible camera: FFMPEG 软解异常: %1").arg(QString::fromUtf8(e.what())));
        m_capture.release();
    } catch (...) {
        LOG_ERROR("Visible camera: FFMPEG 软解未知异常");
        m_capture.release();
    }
    if (!m_capture.isOpened()) {
        try {
            LOG_WARN("Visible camera: FFMPEG 失败, 尝试 GStreamer auto-plug...");
            m_capture.open(m_rtspUrl.toStdString(), cv::CAP_GSTREAMER);
        } catch (const std::exception &e) {
            LOG_ERROR(QString("Visible camera: GStreamer auto-plug 异常: %1").arg(QString::fromUtf8(e.what())));
            m_capture.release();
        } catch (...) {
            LOG_ERROR("Visible camera: GStreamer auto-plug 未知异常");
            m_capture.release();
        }
    }
    if (m_capture.isOpened()) {
        m_decoderDesc = QStringLiteral("软解 FFMPEG(CPU)");
        LOG_WARN(QString("Visible camera: 软解链路已启用 -> %1 (4K 预览可能卡顿)").arg(m_decoderDesc));
        emit decoderChanged(m_decoderDesc);
        return true;
    }
    return false;
}

void VisibleCameraWorker::startCapture()
{
    LOG_INFO(QString("=== Visible Camera: Starting capture ==="));
    LOG_INFO(QString("  RTSP URL: %1").arg(m_rtspUrl));
    LOG_INFO(QString("  预览目标分辨率: %1")
                 .arg((m_previewW > 0 && m_previewH > 0)
                          ? QString("%1x%2 (GPU缩放)").arg(m_previewW).arg(m_previewH)
                          : QStringLiteral("原分辨率")));

    // 解码优先级(2026-09 优化): 硬解优先 —— nvv4l2decoder 支持 4K 且可 GPU 缩放,
    // 实测 4K 预览可达约 27fps(满帧); 软解仅在硬解不可用时作为后备。
    if (!openHardwareStream()) {
        LOG_WARN("Visible camera: 硬解链路均失败, 回退软解");
        if (!openSoftStream()) {
            LOG_ERROR("Visible camera: All backends failed to open RTSP stream");
            emit errorOccurred("无法连接可见光RTSP流(所有后端均失败): " + m_rtspUrl);
            emit connectionStatusChanged(false);
            return;
        }
    }

    // 仅对非 GStreamer 后端设置内部缓冲。
    // 重要: GStreamer 后端不支持 CAP_PROP_BUFFERSIZE, 强设会返回 false 并使 appsink 失效,
    // 之后 read() 一帧都读不到, 进而触发反复重连(实测: 设置后 0 帧/6 次读失败,
    // 不设置则 160 帧/0 失败)。这是此前"可见光频繁重连"的根因。
    const int backendForBuf = static_cast<int>(m_capture.get(cv::CAP_PROP_BACKEND));
    if (backendForBuf != cv::CAP_GSTREAMER) {
        m_capture.set(cv::CAP_PROP_BUFFERSIZE, 3);
    }

    // Log connection success and stream properties
    double frameW = m_capture.get(cv::CAP_PROP_FRAME_WIDTH);
    double frameH = m_capture.get(cv::CAP_PROP_FRAME_HEIGHT);
    double fps = m_capture.get(cv::CAP_PROP_FPS);
    double backendId = m_capture.get(cv::CAP_PROP_BACKEND);
    LOG_INFO(QString("Visible camera: RTSP stream opened successfully"));
    LOG_INFO(QString("  Backend ID: %1").arg(static_cast<int>(backendId)));
    LOG_INFO(QString("  Frame size: %1x%2").arg(static_cast<int>(frameW)).arg(static_cast<int>(frameH)));
    LOG_INFO(QString("  Reported FPS: %1").arg(fps, 0, 'f', 1));

    emit connectionStatusChanged(true);

    cv::Mat frame;
    QElapsedTimer fpsTimer;
    fpsTimer.start();
    int frameCount = 0;
    int reconnectCount = 0;
    m_emptyFrameStreak = 0;
    m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();

    while (!m_stopRequested) {
        // ---- 背压超时自愈 (2026-09-22) ----
        // 若界面线程被渲染占满/卡死而长期不确认, 背压会永久置位使采集彻底停摆,
        // 这里超时后强制解除, 保证"消费端异常"不会拖死"采集端"。
        if (m_frameInFlight.load(std::memory_order_acquire) &&
            QDateTime::currentMSecsSinceEpoch() - m_frameInFlightMs > BACKPRESSURE_TIMEOUT_MS) {
            m_frameInFlight.store(false, std::memory_order_release);
            LOG_WARN("Visible camera: 界面确认超时, 强制解除背压以恢复采集");
        }

        if (!m_capture.read(frame) || frame.empty()) {
            // ---- 重连容错 (2026-09-22 延时优化) ----
            // 原实现"一帧空即重连": 实测每约 2 分钟断流一次, 而相机 GOP=50 @25fps,
            // 重连后最长要等 2 秒才收到 I 帧 -> 周期性卡顿, 观感上就是"延时很大"。
            // 现在偶发空帧只跳过(最多 20 次 / 2.5 秒), 只有真正断链才重连。
            ++m_emptyFrameStreak;
            const qint64 silentMs = QDateTime::currentMSecsSinceEpoch() - m_lastFrameMs;
            if (m_emptyFrameStreak < EMPTY_FRAME_TOLERANCE && silentMs < STALL_RECONNECT_MS) {
                QThread::msleep(5);
                continue;
            }

            reconnectCount++;
            LOG_WARN(QString("Visible camera: 链路无帧(连续空帧 %1 / 静默 %2ms), 重连第 %3/%4 次")
                         .arg(m_emptyFrameStreak).arg(silentMs)
                         .arg(reconnectCount).arg(MAX_RECONNECT_ATTEMPTS));

            if (reconnectCount > MAX_RECONNECT_ATTEMPTS) {
                LOG_ERROR(QString("Visible camera: Max reconnect attempts (%1) exceeded, giving up")
                              .arg(MAX_RECONNECT_ATTEMPTS));
                emit errorOccurred(QString("可见光重连失败，已尝试 %1 次").arg(MAX_RECONNECT_ATTEMPTS));
                emit connectionStatusChanged(false);
                break;
            }

            emit connectionStatusChanged(false);
            m_capture.release();
            QThread::msleep(RECONNECT_DELAY_MS);

            // 重连同样保持"硬解优先", 避免回退到软解导致 4K 卡顿
            LOG_INFO(QString("Visible camera: Reconnect #%1 - 重新打开硬解链路...").arg(reconnectCount));
            if (!openHardwareStream()) {
                LOG_WARN(QString("Visible camera: Reconnect #%1 硬解失败, 回退软解...").arg(reconnectCount));
                openSoftStream();
            }

            if (m_capture.isOpened()) {
                double rw = m_capture.get(cv::CAP_PROP_FRAME_WIDTH);
                double rh = m_capture.get(cv::CAP_PROP_FRAME_HEIGHT);
                LOG_INFO(QString("Visible camera: Reconnected successfully (attempt #%1), frame: %2x%3")
                             .arg(reconnectCount).arg(static_cast<int>(rw)).arg(static_cast<int>(rh)));
                emit connectionStatusChanged(true);
                reconnectCount = 0;  // Reset counter on success
                m_emptyFrameStreak = 0;
                m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
            } else {
                LOG_ERROR(QString("Visible camera: Reconnect attempt #%1 failed").arg(reconnectCount));
            }
            continue;
        }

        // 取到有效帧: 重置空帧容错计数与静默计时
        m_emptyFrameStreak = 0;
        m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();

        // ---- 背压(关键): 上一帧还未被界面线程消费时, 直接丢弃本帧 ----
        // 1920x1080 RGB888 每帧约 6MB, 若界面渲染(缩放/上传/绘制)追不上采集速度,
        // 排队的事件会无限堆积(实测 RSS 涨到 1.45GB 后被 OOM killer 杀死)。
        if (m_frameInFlight.load(std::memory_order_acquire)) {
            m_droppedFrames++;
            if (m_lastDropLogMs == 0 ||
                QDateTime::currentMSecsSinceEpoch() - m_lastDropLogMs > 5000) {
                m_lastDropLogMs = QDateTime::currentMSecsSinceEpoch();
                LOG_DEBUG(QString("Visible camera: 界面未消费, 已丢弃 %1 帧 (背压丢帧)")
                              .arg(m_droppedFrames));
            }
        } else {
            // 硬解链路已由 nvvidconv 在 GPU 内输出 BGRx(4 通道),
            // 其内存布局(B,G,R,X)与 QImage::Format_RGB32(0xffRRGGBB, 小端)完全一致,
            // 因此可零转换直接映射 —— 省掉 4K 全帧的 CPU 格式转换(每帧 24MB 读写)。
            // 软解后备链路(FFMPEG)为 3 通道 BGR, 才需要转成 BGRA。
            cv::Mat bgrx;
            if (frame.channels() == 4) {
                bgrx = frame;
            } else {
                cv::cvtColor(frame, bgrx, cv::COLOR_BGR2BGRA);
            }

            QImage qimg(bgrx.data, bgrx.cols, bgrx.rows,
                        static_cast<int>(bgrx.step), QImage::Format_RGB32);
            m_frameInFlight.store(true, std::memory_order_release);
            m_frameInFlightMs = QDateTime::currentMSecsSinceEpoch();
            emit frameReady(qimg.copy());
        }

        frameCount++;
        qint64 elapsed = fpsTimer.elapsed();
        if (elapsed >= 1000) {
            double actualFps = frameCount * 1000.0 / elapsed;
            emit fpsUpdated(actualFps);
            // 每秒记录一次采集/丢帧情况, 便于评估延迟优化效果
            LOG_DEBUG(QString("Visible camera: 采集 %1fps (界面消费不足丢弃 %2 帧)")
                          .arg(actualFps, 0, 'f', 1)
                          .arg(m_droppedFrames));
            frameCount = 0;
            fpsTimer.restart();
        }
    }

    m_capture.release();
    LOG_INFO("Visible camera: Capture stopped");
}

// ============================================================
// VisibleCamera (controller)
// ============================================================
VisibleCamera::VisibleCamera(QObject *parent)
    : QObject(parent)
{
    m_networkManager = new QNetworkAccessManager(this);
    connect(m_networkManager, &QNetworkAccessManager::authenticationRequired,
            [this](QNetworkReply *, QAuthenticator *authenticator) {
                authenticator->setUser(m_user);
                authenticator->setPassword(m_pass);
            });

    // 变焦安全定时器: 连续变焦指令后若超时无新指令则自动停止, 防止滑条停在中位以外导致变焦失控
    m_zoomStopTimer = new QTimer(this);
    m_zoomStopTimer->setSingleShot(true);
    m_zoomStopTimer->setInterval(ZOOM_STOP_TIMEOUT_MS);
    connect(m_zoomStopTimer, &QTimer::timeout, this, [this]() {
        LOG_INFO("Visible camera: Zoom auto-stop (safety timeout)");
        sendPtzZoom(0);
    });

    // 曝光滑条去抖: 拖动过程中只下发最后一次取值, 避免刷爆相机
    m_exposureTimer = new QTimer(this);
    m_exposureTimer->setSingleShot(true);
    m_exposureTimer->setInterval(EXPOSURE_DEBOUNCE_MS);
    connect(m_exposureTimer, &QTimer::timeout, this, &VisibleCamera::applyExposure);

    // 光圈滑条去抖
    m_irisTimer = new QTimer(this);
    m_irisTimer->setSingleShot(true);
    m_irisTimer->setInterval(EXPOSURE_DEBOUNCE_MS);
    connect(m_irisTimer, &QTimer::timeout, this, &VisibleCamera::applyIris);

    // 增益滑条去抖
    m_gainTimer = new QTimer(this);
    m_gainTimer->setSingleShot(true);
    m_gainTimer->setInterval(EXPOSURE_DEBOUNCE_MS);
    connect(m_gainTimer, &QTimer::timeout, this, &VisibleCamera::applyGain);
}

void VisibleCamera::setCredentials(const QString &ip, int port, const QString &user, const QString &pass)
{
    m_ip = ip;
    m_port = port;
    m_user = user;
    m_pass = pass;
    LOG_INFO(QString("Visible camera credentials set: %1:%2 (user: %3)")
                 .arg(m_ip).arg(m_port).arg(m_user));
}

void VisibleCamera::setPreviewSize(int w, int h)
{
    m_previewW = w;
    m_previewH = h;
    if (w > 0 && h > 0) {
        LOG_INFO(QString("Visible camera: 预览目标分辨率 -> %1x%2 (GPU 缩放, 降低 CPU 负担)")
                     .arg(w).arg(h));
    } else {
        // 当前策略: 预览按所选码流原生分辨率硬解显示(选 4K 就显示 4K),
        // 与录像保存分辨率保持"选择=显示=录像"三者一致。
        LOG_INFO("Visible camera: 预览保持码流原分辨率(GPU 不缩放)");
    }
}

void VisibleCamera::setStreamChannel(int channel)
{
    if (channel <= 0) return;
    m_streamChannel = channel;
    LOG_INFO(QString("Visible camera: 当前码流通道 -> %1 (原生 %2)")
                 .arg(channel).arg(streamResolutionText(channel)));
}

// 码流通道 -> 原生分辨率(该机芯规格): 101 主码流 4K / 103 第三码流 1080p / 102 子码流 704x576
QString VisibleCamera::streamResolutionText(int channel)
{
    switch (channel) {
    case 101: return QStringLiteral("3840x2160 (4K)");
    case 102: return QStringLiteral("704x576");
    case 103: return QStringLiteral("1920x1080");
    default:  return QStringLiteral("未知");
    }
}

VisibleCamera::~VisibleCamera()
{
    stop();
}

void VisibleCamera::start(const QString &rtspUrl)
{
    if (m_thread.isRunning() || m_worker) {
        stop();
    }
    if (m_worker) {
        // 上一次采集线程仍未退出(见 stop() 中的保护), 此时不能再新建 worker
        LOG_ERROR("Visible camera: 上一次采集线程尚未退出, 忽略本次启动请求");
        return;
    }

    m_rtspUrl = rtspUrl;   // 录像 ffmpeg 进程复用该地址
    m_worker = new VisibleCameraWorker();
    m_worker->setRtspUrl(rtspUrl);
    m_worker->setPreviewSize(m_previewW, m_previewH);   // 下发 GPU 缩放目标(0,0=不缩放)
    m_worker->moveToThread(&m_thread);

    // 注意: 这里在消费完一帧后必须 acknowledgeFrame() 解除背压, 否则采集端会持续丢帧
    connect(m_worker, &VisibleCameraWorker::frameReady,
            this, [this](const QImage &frame) {
                m_lastFrame = frame;
                if (m_worker) m_worker->acknowledgeFrame();
                emit frameReady(frame);
            }, Qt::QueuedConnection);
    connect(m_worker, &VisibleCameraWorker::connectionStatusChanged,
            this, &VisibleCamera::connectionStatusChanged, Qt::QueuedConnection);
    connect(m_worker, &VisibleCameraWorker::fpsUpdated,
            this, &VisibleCamera::fpsUpdated, Qt::QueuedConnection);
    connect(m_worker, &VisibleCameraWorker::errorOccurred,
            this, &VisibleCamera::errorOccurred, Qt::QueuedConnection);
    connect(m_worker, &VisibleCameraWorker::decoderChanged,
            this, &VisibleCamera::decoderChanged, Qt::QueuedConnection);

    connect(this, &VisibleCamera::startRequested, m_worker, &VisibleCameraWorker::startCapture);

    m_thread.start();
    emit startRequested();
}

void VisibleCamera::stop()
{
    stopRecording();   // 断开前先结束录像, 保证视频文件正常收尾
    if (m_worker) {
        m_worker->requestStop();   // 采集循环每轮开头检查该标志
    }
    if (m_thread.isRunning()) {
        m_thread.quit();
        // 采集循环可能正阻塞在 m_capture.read()(FFMPEG 读超时 10s), 故等待时间需大于该超时。
        // 严禁 terminate(): 硬杀线程会破坏 OpenCV/GStreamer 内部状态, 退出时导致 SIGSEGV/SIGABRT。
        if (!m_thread.wait(12000)) {
            LOG_WARN("Visible camera: 采集线程未在 12s 内退出, 暂不销毁 worker");
            return;   // 保留 m_worker, 避免线程仍在使用该对象时被销毁
        }
    }
    // 线程已确认退出, 此处销毁 worker 才是安全的。
    // 原实现用 QThread::finished -> deleteLater, 但线程事件循环已停止, 该槽永远不会执行,
    // 每次 连接/断开 都会泄漏一个 worker 及其解码器资源。
    if (m_worker) {
        delete m_worker;
        m_worker = nullptr;
    }
}

bool VisibleCamera::isRunning() const
{
    return m_thread.isRunning();
}

void VisibleCamera::sendIsapiCommand(const QString &url, const QString &body)
{
    LOG_INFO(QString("ISAPI command -> URL: %1").arg(url));
    if (!body.isEmpty()) {
        LOG_DEBUG(QString("ISAPI command body: %1").arg(body));
    }

    QNetworkRequest request;
    request.setUrl(QUrl(url));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml");

    QNetworkReply *reply = nullptr;
    if (body.isEmpty()) {
        reply = m_networkManager->get(request);
    } else {
        reply = m_networkManager->put(request, body.toUtf8());
    }

    connect(reply, &QNetworkReply::finished, this, [reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            LOG_ERROR(QString("ISAPI command failed: %1 (HTTP %2)")
                          .arg(reply->errorString())
                          .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
        } else {
            LOG_INFO(QString("ISAPI command success: %1").arg(QString(reply->readAll())));
        }
        reply->deleteLater();
    });
}

// 生成与海康 ISAPI 一致的时间串: yyyy-MM-ddTHH:mm:ss+08:00
// 注意1: Qt 的格式符 'z' 表示"毫秒"而不是时区偏移, 若写成 "...sszzz" 会得到
//        形如 2026-09-20T18:01:47476 的非法时间, 设备直接返回 HTTP 400。
//        时区偏移必须按 offsetFromUtc() 自行拼接。
// 注意2: ISAPI 时间串只精确到秒, 直接截断毫秒会固定慢 0~1 秒,
//        因此取整到"最近的秒"(毫秒 >= 500 进位), 使误差不超过 ±0.5 秒。
static QString isapiTimeString(QDateTime t)
{
    const int msec = t.time().msec();
    if (msec >= 500) t = t.addMSecs(1000 - msec);
    const int offSec = t.offsetFromUtc();
    const int absSec = qAbs(offSec);
    return t.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"))
         + QStringLiteral("%1%2:%3")
               .arg(offSec < 0 ? QLatin1Char('-') : QLatin1Char('+'))
               .arg(absSec / 3600, 2, 10, QLatin1Char('0'))
               .arg((absSec % 3600) / 60, 2, 10, QLatin1Char('0'));
}

// ============================================================
// 校时: 把相机时间校准为本机系统时间(误差目标 < 1 秒)
//
//   流程: GET 读设备时间文档 -> 写入 -> 按实测 PUT 耗时补偿重写 -> 读回校验
//
//   为什么写两次:
//     实测可见光相机一次 PUT 全程耗时约 2.7 秒(热像仪约 0.3 秒), 而设备是把
//     收到的 localTime 直接当作当前时间。若只写一次, 设备时间会固定偏慢
//     约一个 PUT 耗时(实测偏差 2.2 秒)。因此第一次写入用于测量耗时,
//     第二次用该耗时补偿, 之后读回校验并把偏差写入日志。
//
//   只改 localTime, 设备时区/对时模式/命名空间原样保留, 影响最小。
// ============================================================
void VisibleCamera::syncSystemTime()
{
    if (m_ip.isEmpty()) {
        emit timeSyncFinished(false, QStringLiteral("未配置可见光相机 IP"));
        return;
    }

    m_timeSyncUrl = QString("http://%1:%2/ISAPI/System/time").arg(m_ip).arg(m_port);
    LOG_INFO(QString("可见光相机校时: 读取设备时间 -> %1").arg(m_timeSyncUrl));

    QNetworkRequest getReq{QUrl(m_timeSyncUrl)};
    QNetworkReply *getReply = m_networkManager->get(getReq);
    connect(getReply, &QNetworkReply::finished, this, [this, getReply]() {
        getReply->deleteLater();

        const int httpCode =
            getReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (getReply->error() != QNetworkReply::NoError) {
            const QString info = QStringLiteral("读取设备时间失败(HTTP %1: %2)")
                                     .arg(httpCode).arg(getReply->errorString());
            LOG_ERROR(QString("可见光相机校时失败: %1").arg(info));
            emit timeSyncFinished(false, info);
            return;
        }

        // 保存设备时间文档作为模板: 命名空间/时区/对时模式都保持设备原样
        m_timeDocTemplate = QString::fromUtf8(getReply->readAll());
        writeTimePass(1);
    });
}

// 写入一轮设备时间。
//   写入值 = 本机当前时间 + m_syncCompensationMs
//   补偿量的物理含义: 从"取时间戳"到"设备把该值当作当前时间生效"之间的延迟,
//   它包含 HTTP 往返 + 设备内部应用时间, 且每次不固定(实测可见光 1.6~2.7 秒),
//   因此不能只靠一次耗时测量, 而是每轮写入后读回校验并迭代修正补偿量。
void VisibleCamera::writeTimePass(int pass)
{
    const QString stamp = isapiTimeString(
        QDateTime::currentDateTime().addMSecs(m_syncCompensationMs));

    QString body = m_timeDocTemplate;
    const QRegularExpression re(QStringLiteral("<localTime>[^<]*</localTime>"));
    if (re.match(body).hasMatch()) {
        body.replace(re, QStringLiteral("<localTime>%1</localTime>").arg(stamp));
    } else {
        // 设备未返回 localTime 字段时, 按海康标准结构构造(时区保持东八区)
        body = QStringLiteral(
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                   "<Time xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                   "<timeMode>manual</timeMode><localTime>%1</localTime>"
                   "<timeZone>CST-8:00:00</timeZone></Time>").arg(stamp);
    }

    QNetworkRequest putReq{QUrl(m_timeSyncUrl)};
    putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml"));
    QNetworkReply *putReply = m_networkManager->put(putReq, body.toUtf8());
    connect(putReply, &QNetworkReply::finished, this, [this, putReply, pass, stamp]() {
        putReply->deleteLater();
        const int code =
            putReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (putReply->error() != QNetworkReply::NoError) {
            const QString info = QStringLiteral("写入失败(HTTP %1: %2)")
                                     .arg(code).arg(putReply->errorString());
            LOG_ERROR(QString("可见光相机校时失败: %1").arg(info));
            emit timeSyncFinished(false, info);
            return;
        }
        LOG_INFO(QString("可见光相机校时: 第 %1 轮写入 %2 (补偿 %3ms)")
                     .arg(pass).arg(stamp).arg(m_syncCompensationMs));
        verifySyncedTime(pass);
    });
}

// 读回设备时间, 计算与本机的偏差, 并按偏差迭代修正补偿量
//   Δ = 设备时间 - 本机时间 = 补偿量 - 实际延迟  =>  新补偿量 = 旧补偿量 - Δ
//   偏差收敛到 500ms 以内或达到 5 轮上限后上报结果
//   (可见光相机的 PUT 生效延迟在 1.5~3 秒间波动, 因此上限取 5 轮以保证收敛)
void VisibleCamera::verifySyncedTime(int pass)
{
    if (m_timeSyncUrl.isEmpty()) return;

    const QDateTime t1 = QDateTime::currentDateTime();
    QNetworkRequest req{QUrl(m_timeSyncUrl)};
    QNetworkReply *reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, t1, pass]() {
        reply->deleteLater();
        const QDateTime t2 = QDateTime::currentDateTime();

        if (reply->error() != QNetworkReply::NoError) {
            emit timeSyncFinished(true, QStringLiteral("已写入(读回校验失败: %1)")
                                            .arg(reply->errorString()));
            return;
        }

        const QString xml = QString::fromUtf8(reply->readAll());
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("<localTime>([^<]*)</localTime>")).match(xml);

        QString info;
        if (m.hasMatch()) {
            const QString devStr = m.captured(1).trimmed();
            const QDateTime dev = QDateTime::fromString(devStr, Qt::ISODate);
            // 用 GET 往返的中间时刻与本机比较, 消除往返延迟带来的测量误差
            const QDateTime mid = t1.addMSecs(t1.msecsTo(t2) / 2);
            if (dev.isValid()) {
                const qint64 diffMs = dev.toMSecsSinceEpoch() - mid.toMSecsSinceEpoch();
                // 迭代修正: 把补偿量调整到使"设备时间 == 本机时间"
                m_syncCompensationMs =
                    qBound<qint64>(-5000, m_syncCompensationMs - diffMs, 5000);
                if (qAbs(diffMs) > 500 && pass < 5) {
                    LOG_INFO(QString("可见光相机校时: 本轮偏差 %1ms, 进行第 %2 轮修正")
                                 .arg(diffMs).arg(pass + 1));
                    writeTimePass(pass + 1);
                    return;
                }
                info = QStringLiteral("已校准为 %1 (与本机偏差 %2 ms)").arg(devStr).arg(diffMs);
                if (qAbs(diffMs) > 1000) {
                    LOG_WARN(QString("可见光相机校时偏差仍偏大: %1 ms").arg(diffMs));
                }
            } else {
                info = QStringLiteral("已校准为 %1").arg(devStr);
            }
        } else {
            info = QStringLiteral("已校准(未取到回读值)");
        }

        LOG_INFO(QString("可见光相机校时成功: %1").arg(info));
        emit timeSyncFinished(true, info);
    });
}

void VisibleCamera::setAutoFocus(bool enable)
{
    setFocusMode(enable ? "AUTO" : "MANUAL");
    LOG_INFO(QString("Visible camera: Auto focus %1").arg(enable ? "ON" : "OFF"));
}

void VisibleCamera::setFocusMode(const QString &focusStyle)
{
    // 该机芯无 /ISAPI/Image/channels/1/focus 端点(404), 必须读取整篇图像文档后改写
    // FocusConfiguration/focusStyle 再整体 PUT 回 /ISAPI/Image/channels/1
    const QString url = QString("http://%1:%2/ISAPI/Image/channels/1").arg(m_ip).arg(m_port);
    LOG_INFO(QString("Visible camera: 设置对焦模式 -> %1").arg(focusStyle));

    QNetworkRequest getReq{QUrl(url)};
    QNetworkReply *getReply = m_networkManager->get(getReq);
    connect(getReply, &QNetworkReply::finished, this, [this, getReply, url, focusStyle]() {
        if (getReply->error() != QNetworkReply::NoError) {
            LOG_ERROR(QString("Visible camera: 读取图像参数失败: %1").arg(getReply->errorString()));
            getReply->deleteLater();
            return;
        }
        QString xml = QString::fromUtf8(getReply->readAll());
        getReply->deleteLater();

        QRegularExpression re("<focusStyle>[^<]*</focusStyle>");
        if (re.match(xml).hasMatch()) {
            xml.replace(re, QString("<focusStyle>%1</focusStyle>").arg(focusStyle));
        } else {
            xml.replace("</ImageChannel>",
                        QString("<FocusConfiguration><focusStyle>%1</focusStyle>"
                                "<focusLimited>600</focusLimited></FocusConfiguration>"
                                "</ImageChannel>").arg(focusStyle));
        }

        QNetworkRequest putReq{QUrl(url)};
        putReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml");
        QNetworkReply *putReply = m_networkManager->put(putReq, xml.toUtf8());
        connect(putReply, &QNetworkReply::finished, this, [putReply, focusStyle]() {
            if (putReply->error() != QNetworkReply::NoError) {
                LOG_ERROR(QString("Visible camera: 设置对焦模式(%1)失败: %2 (HTTP %3)")
                              .arg(focusStyle).arg(putReply->errorString())
                              .arg(putReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
            } else {
                LOG_INFO(QString("Visible camera: 对焦模式已设为 %1").arg(focusStyle));
            }
            putReply->deleteLater();
        });
    });
}

// 云台连续控制: pan/tilt/zoom 均为 -100..100, 0 = 停止(实测 200 OK)
void VisibleCamera::sendPtzContinuous(int pan, int tilt, int zoom)
{
    const QString url = QString("http://%1:%2/ISAPI/PTZCtrl/channels/1/continuous")
                            .arg(m_ip).arg(m_port);
    const QString body = QString(
        "<PTZData xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<pan>%1</pan><tilt>%2</tilt><zoom>%3</zoom></PTZData>")
        .arg(qBound(-100, pan, 100)).arg(qBound(-100, tilt, 100)).arg(qBound(-100, zoom, 100));
    sendIsapiCommand(url, body);
}

void VisibleCamera::sendPtzZoom(int speed)
{
    sendPtzContinuous(0, 0, speed);
}

void VisibleCamera::zoomIn()
{
    LOG_INFO(QString("Visible camera: Zoom In (PTZ zoom=+%1)").arg(ZOOM_SPEED));
    sendPtzZoom(+ZOOM_SPEED);
    if (m_zoomStopTimer) m_zoomStopTimer->start();
}

void VisibleCamera::zoomOut()
{
    LOG_INFO(QString("Visible camera: Zoom Out (PTZ zoom=-%1)").arg(ZOOM_SPEED));
    sendPtzZoom(-ZOOM_SPEED);
    if (m_zoomStopTimer) m_zoomStopTimer->start();
}

void VisibleCamera::zoomStop()
{
    if (m_zoomStopTimer) m_zoomStopTimer->stop();
    m_zoomTarget = -1;
    m_zoomStepDir = 0;
    sendPtzZoom(0);
    stopZoomConfirm();   // 再补发一次停止, 避免镜头继续冲到最大倍率
    LOG_INFO("Visible camera: Zoom Stop (PTZ zoom=0)");
}

// 二次确认停止: 部分机芯对单次 zoom=0 响应不稳定, 会继续向原方向冲到极限倍率。
// 这里在 250ms 后补发一次 zoom=0, 确保镜头真正停下。
void VisibleCamera::stopZoomConfirm()
{
    QTimer::singleShot(250, this, [this]() {
        sendPtzZoom(0);
        LOG_DEBUG("Visible camera: 变焦停止二次确认 (zoom=0)");
    });
}

// 单击步进变焦: 先回读设备当前 absoluteZoom, 再沿固定方向闭环移动一档。
// 关键: 整个过程方向恒定、绝不反向, 一旦到达/越过目标立即停止,
//       因此不会出现"数值先变低又变高"的来回抖动。
void VisibleCamera::zoomStep(int dir)
{
    if (dir == 0) return;
    if (m_zoomTarget > 0) {
        LOG_INFO("Visible camera: 上一档变焦仍在进行, 忽略本次点击");
        return;
    }
    const QString url = QString("http://%1:%2/ISAPI/PTZCtrl/channels/1/status")
                            .arg(m_ip).arg(m_port);
    QNetworkRequest req{QUrl(url)};
    QNetworkReply *reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, dir]() {
        if (reply->error() != QNetworkReply::NoError) {
            LOG_ERROR(QString("Visible camera: 读取变焦位置失败(步进): %1").arg(reply->errorString()));
            reply->deleteLater();
            return;
        }
        const QString xml = QString::fromUtf8(reply->readAll());
        reply->deleteLater();

        const QRegularExpressionMatch m =
            QRegularExpression("<absoluteZoom>(\\d+)</absoluteZoom>").match(xml);
        if (!m.hasMatch()) {
            LOG_WARN("Visible camera: 步进变焦无法解析 absoluteZoom");
            return;
        }
        const int cur = m.captured(1).toInt();
        emit zoomPositionChanged(cur);

        const int target = qBound(ZOOM_POS_MIN, cur + dir * ZOOM_STEP_UNITS, ZOOM_POS_MAX);
        if (target == cur) {
            LOG_INFO(QString("Visible camera: 变焦已到极限 (当前 %1)").arg(cur));
            return;
        }
        m_zoomStepDir = (target > cur) ? +1 : -1;   // 整个步进期间方向固定
        m_zoomTarget = target;
        m_zoomDeadlineMs = QDateTime::currentMSecsSinceEpoch() + ZOOM_CLOSED_LOOP_TIMEOUT_MS;
        if (m_zoomStopTimer) m_zoomStopTimer->stop();
        LOG_INFO(QString("Visible camera: 变焦步进 %1 (当前 %2 -> 目标 %3)")
                     .arg(m_zoomStepDir > 0 ? QStringLiteral("+") : QStringLiteral("-"))
                     .arg(cur).arg(target));
        pollZoomStep();
    });
}

// 闭环移动至指定绝对位置(保留接口; 同样采用单向不反向策略)
void VisibleCamera::setZoomPosition(int pos)
{
    if (pos <= 0) {
        zoomStop();
        return;
    }
    // 该机芯不支持绝对变焦(PUT /absolute 返回 400), 因此采用闭环:
    // 读取 /status 的 absoluteZoom, 单向逼近目标, 到达后发送 zoom=0
    m_zoomTarget = qBound(ZOOM_POS_MIN, pos, ZOOM_POS_MAX);
    m_zoomStepDir = 0;   // 方向由首次回读决定, 之后不再改变
    m_zoomDeadlineMs = QDateTime::currentMSecsSinceEpoch() + ZOOM_CLOSED_LOOP_TIMEOUT_MS;
    if (m_zoomStopTimer) m_zoomStopTimer->stop();   // 闭环期间不使用手动安全停止
    LOG_INFO(QString("Visible camera: 变焦闭环 -> 目标位置 %1 (%.1f×)")
                 .arg(m_zoomTarget).arg(m_zoomTarget / 10.0, 0, 'f', 1));
    pollZoomStep();
}

void VisibleCamera::pollZoomStep()
{
    if (m_zoomTarget <= 0) return;

    const QString url = QString("http://%1:%2/ISAPI/PTZCtrl/channels/1/status")
                            .arg(m_ip).arg(m_port);
    QNetworkRequest req{QUrl(url)};
    QNetworkReply *reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            LOG_ERROR(QString("Visible camera: 读取变焦位置失败: %1").arg(reply->errorString()));
            reply->deleteLater();
            m_zoomTarget = -1;
            m_zoomStepDir = 0;
            sendPtzZoom(0);
            stopZoomConfirm();
            return;
        }
        const QString xml = QString::fromUtf8(reply->readAll());
        reply->deleteLater();

        const QRegularExpressionMatch m =
            QRegularExpression("<absoluteZoom>(\\d+)</absoluteZoom>").match(xml);
        if (!m.hasMatch()) {
            m_zoomTarget = -1;
            m_zoomStepDir = 0;
            sendPtzZoom(0);
            stopZoomConfirm();
            return;
        }

        const int cur = m.captured(1).toInt();
        emit zoomPositionChanged(cur);
        if (m_zoomTarget <= 0) return;

        // 首次回读确定运动方向, 之后保持不变
        if (m_zoomStepDir == 0) {
            m_zoomStepDir = (m_zoomTarget > cur) ? +1 : -1;
        }

        // 单向到达判定: 沿固定方向到达或越过目标即停止 —— 绝不反向, 故不会来回抖动
        const int remaining = (m_zoomTarget - cur) * m_zoomStepDir;   // >0 表示还需继续
        if (remaining <= ZOOM_AROUND_TOLERANCE) {
            sendPtzZoom(0);
            stopZoomConfirm();
            LOG_INFO(QString("Visible camera: 变焦步进完成 (目标 %1, 实测 %2)")
                         .arg(m_zoomTarget).arg(cur));
            m_zoomTarget = -1;
            m_zoomStepDir = 0;
            return;
        }
        if (QDateTime::currentMSecsSinceEpoch() > m_zoomDeadlineMs) {
            sendPtzZoom(0);
            stopZoomConfirm();
            LOG_WARN(QString("Visible camera: 变焦闭环超时(当前 %1, 目标 %2), 已停止")
                         .arg(cur).arg(m_zoomTarget));
            m_zoomTarget = -1;
            m_zoomStepDir = 0;
            return;
        }

        // 始终沿固定方向运动
        sendPtzZoom(m_zoomStepDir * ZOOM_STEP_SPEED);
        QTimer::singleShot(ZOOM_STEP_POLL_MS, this, &VisibleCamera::pollZoomStep);
    });
}

void VisibleCamera::captureImage(const QString &savePath)
{
    // GET /ISAPI/Streaming/channels/1/picture
    // 注: iDS-2ZMN2312S 同时支持 /101/picture 与 /1/picture (均返回 3840x2160 JPEG);
    //     声波成像仪 HM-TD2069N-18D 仅支持 /1/picture (101 返回 403), 故统一使用 /1/picture
    QString url = QString("http://%1:%2/ISAPI/Streaming/channels/1/picture")
                      .arg(m_ip).arg(m_port);

    LOG_INFO(QString("Visible camera: Capture requested -> %1").arg(savePath));

    QNetworkRequest request;
    request.setUrl(QUrl(url));

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, savePath]() {
        if (reply->error() == QNetworkReply::NoError) {
            QByteArray data = reply->readAll();
            if (m_lastFrame.isNull()) {
                // Save raw JPEG data from ISAPI
                QFile file(savePath);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(data);
                    file.close();
                    LOG_INFO(QString("Visible camera: Image captured from ISAPI: %1 (%2 bytes)")
                                 .arg(savePath).arg(data.size()));
                    emit imageCaptured(savePath);
                }
            } else {
                m_lastFrame.save(savePath);
                LOG_INFO(QString("Visible camera: Image captured from frame: %1").arg(savePath));
                emit imageCaptured(savePath);
            }
        } else {
            // Fallback: save last frame
            if (!m_lastFrame.isNull()) {
                m_lastFrame.save(savePath);
                LOG_INFO(QString("Visible camera: Image captured from last frame (ISAPI failed): %1").arg(savePath));
                emit imageCaptured(savePath);
            } else {
                LOG_ERROR(QString("Visible camera: Capture failed: %1").arg(reply->errorString()));
            }
        }
        reply->deleteLater();
    });
}

void VisibleCamera::setExposure(int value)
{
    // 滑条拖动会连续触发, 此处仅记录并去抖, 由 applyExposure() 统一下发
    m_pendingExposure = qBound(0, value, 100);
    if (m_exposureTimer) {
        m_exposureTimer->start();
    } else {
        applyExposure();
    }
    LOG_INFO(QString("Visible camera: 曝光滑条 = %1").arg(m_pendingExposure));
}

void VisibleCamera::forceManualExposure()
{
    // 必须切手动曝光, 否则自动曝光会立即把亮度纠正回去(表现为"点了没反应")
    sendIsapiCommand(QString("http://%1:%2/ISAPI/Image/channels/1/exposure").arg(m_ip).arg(m_port),
                     "<Exposure xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                     "<ExposureType>manual</ExposureType></Exposure>");
}

void VisibleCamera::applyExposure()
{
    // 实测该机芯: 快门无效, 亮度由 光圈(Iris) 与 增益(Gain) 决定
    //   滑条 0 (最暗) -> IrisLevel 2200,  Gain 0
    //   滑条 100(最亮) -> IrisLevel 160,   Gain 100
    const int v = qBound(0, m_pendingExposure, 100);
    const int irisLevel = irisLevelFromSlider(v);
    m_irisLevelRaw = irisLevel;          // 记录设备光圈档位, 供 光圈± 步进

    m_pendingIris = v;   // 与独立光圈滑条保持同步
    m_pendingGain = v;   // 与独立增益滑条保持同步

    forceManualExposure();
    sendIsapiCommand(QString("http://%1:%2/ISAPI/Image/channels/1/iris").arg(m_ip).arg(m_port),
                     QString("<Iris xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                             "<IrisLevel>%1</IrisLevel></Iris>").arg(irisLevel));
    sendIsapiCommand(QString("http://%1:%2/ISAPI/Image/channels/1/gain").arg(m_ip).arg(m_port),
                     QString("<Gain xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                             "<GainLevel>%1</GainLevel><GainLimit>100</GainLimit></Gain>").arg(v));

    LOG_INFO(QString("Visible camera: 应用曝光 滑条=%1 -> 手动曝光 IrisLevel=%2 GainLevel=%3")
                 .arg(v).arg(irisLevel).arg(v));
}

void VisibleCamera::setIris(int value)
{
    m_pendingIris = qBound(0, value, 100);
    if (m_irisTimer) {
        m_irisTimer->start();
    } else {
        applyIris();
    }
    LOG_INFO(QString("Visible camera: 光圈滑条 = %1").arg(m_pendingIris));
}

void VisibleCamera::applyIris()
{
    const int v = qBound(0, m_pendingIris, 100);
    const int irisLevel = irisLevelFromSlider(v);
    m_irisLevelRaw = irisLevel;          // 记录设备光圈档位, 供 光圈± 步进
    forceManualExposure();
    sendIsapiCommand(QString("http://%1:%2/ISAPI/Image/channels/1/iris").arg(m_ip).arg(m_port),
                     QString("<Iris xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                             "<IrisLevel>%1</IrisLevel></Iris>").arg(irisLevel));
    LOG_INFO(QString("Visible camera: 应用光圈 滑条=%1 -> IrisLevel=%2").arg(v).arg(irisLevel));
}

void VisibleCamera::setGain(int value)
{
    m_pendingGain = qBound(0, value, 100);
    if (m_gainTimer) {
        m_gainTimer->start();
    } else {
        applyGain();
    }
    LOG_INFO(QString("Visible camera: 增益滑条 = %1").arg(m_pendingGain));
}

void VisibleCamera::applyGain()
{
    const int v = qBound(0, m_pendingGain, 100);
    forceManualExposure();
    sendIsapiCommand(QString("http://%1:%2/ISAPI/Image/channels/1/gain").arg(m_ip).arg(m_port),
                     QString("<Gain xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                             "<GainLevel>%1</GainLevel><GainLimit>100</GainLimit></Gain>").arg(v));
    LOG_INFO(QString("Visible camera: 应用增益 GainLevel=%1").arg(v));
}

void VisibleCamera::setAutoExposure()
{
    sendIsapiCommand(QString("http://%1:%2/ISAPI/Image/channels/1/exposure").arg(m_ip).arg(m_port),
                     "<Exposure xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                     "<ExposureType>auto</ExposureType></Exposure>");
    LOG_INFO("Visible camera: 恢复自动曝光 (ExposureType=auto)");
}

// ============================================================
// 图像参数(对应相机 Web 端"图像"配置)
//   该端点(/ISAPI/Image/channels/1)不接受片段, 必须整篇 读-改-写。
//   为精确定位, 先按子块名(如 IrcutFilter/BLC/WDR)截取块, 再替换块内标签。
// ============================================================
void VisibleCamera::setImageParam(const QString &block, const QString &tag, const QString &value)
{
    const QString url = QString("http://%1:%2/ISAPI/Image/channels/1").arg(m_ip).arg(m_port);
    LOG_INFO(QString("Visible camera: 设置图像参数 %1/%2 = %3").arg(block, tag, value));

    QNetworkRequest getReq{QUrl(url)};
    QNetworkReply *getReply = m_networkManager->get(getReq);
    connect(getReply, &QNetworkReply::finished, this, [this, getReply, url, block, tag, value]() {
        if (getReply->error() != QNetworkReply::NoError) {
            LOG_ERROR(QString("Visible camera: 读取图像参数失败(%1/%2): %3")
                          .arg(block, tag, getReply->errorString()));
            getReply->deleteLater();
            return;
        }
        QString xml = QString::fromUtf8(getReply->readAll());
        getReply->deleteLater();

        // 1) 定位子块 <block ...>...</block>
        QRegularExpression bre(QString("<%1[^>]*>.*?</%1>").arg(block),
                               QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch bm = bre.match(xml);
        if (!bm.hasMatch()) {
            LOG_WARN(QString("Visible camera: 图像参数中未找到子块 <%1>, 跳过").arg(block));
            return;
        }
        QString blk = bm.captured(0);

        // 2) 替换块内标签值
        QRegularExpression tre(QString("<%1>[^<]*</%1>").arg(tag));
        if (!tre.match(blk).hasMatch()) {
            LOG_WARN(QString("Visible camera: 子块 <%1> 中未找到 <%2>, 跳过").arg(block, tag));
            return;
        }
        blk.replace(tre, QString("<%1>%2</%1>").arg(tag, value));
        xml.replace(bm.capturedStart(0), bm.capturedLength(0), blk);

        // 3) 整篇回写
        QNetworkRequest putReq{QUrl(url)};
        putReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml");
        QNetworkReply *putReply = m_networkManager->put(putReq, xml.toUtf8());
        connect(putReply, &QNetworkReply::finished, this, [putReply, block, tag, value]() {
            if (putReply->error() != QNetworkReply::NoError) {
                LOG_ERROR(QString("Visible camera: 设置图像参数 %1/%2=%3 失败: %4 (HTTP %5)")
                              .arg(block, tag, value).arg(putReply->errorString())
                              .arg(putReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
            } else {
                LOG_INFO(QString("Visible camera: 图像参数已设置 %1/%2 = %3").arg(block, tag, value));
            }
            putReply->deleteLater();
        });
    });
}

void VisibleCamera::queryImageParams()
{
    const QString url = QString("http://%1:%2/ISAPI/Image/channels/1").arg(m_ip).arg(m_port);
    QNetworkRequest req{QUrl(url)};
    QNetworkReply *reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            emit imageConfigReceived(QString::fromUtf8(reply->readAll()));
        } else {
            LOG_WARN(QString("Visible camera: 回读图像参数失败: %1").arg(reply->errorString()));
        }
        reply->deleteLater();
    });
}

// 日夜切换便捷封装: day=白天(彩色) / night=夜晚(黑白) / auto=自动
void VisibleCamera::setIrcutFilterDay()   { setImageParam("IrcutFilter", "IrcutFilterType", "day"); }
void VisibleCamera::setIrcutFilterNight() { setImageParam("IrcutFilter", "IrcutFilterType", "night"); }
void VisibleCamera::setIrcutFilterAuto()  { setImageParam("IrcutFilter", "IrcutFilterType", "auto"); }

// ============================================================
// 云台方向 / 镜头档位 / 预置点  (2026-09 按 iDS-2ZMN2312S 实测接口实现)
//   实测结论:
//     - 连续云台 PUT /ISAPI/PTZCtrl/channels/1/continuous 支持 pan/tilt/zoom(±100) -> 200 OK
//     - 该机芯 continuous 的 focus/iris 元素无效(实测画面清晰度/亮度无变化)
//     - 聚焦只有 "对焦模式(focusStyle) + 最小聚焦距离(focusLimited 档位)" 可用,
//       且 /ISAPI/Image/channels/1 不接受只含 FocusConfiguration 的片段(实测 500 deviceError),
//       必须整篇 read-modify-write
//     - 预置点调用 GET/空 body 会报 badXmlContent, 必须 PUT + <PTZPreset><id>N</id>
//       设备内置特殊预置点: 45=一键巡航, 49=掉电记忆
// ============================================================
void VisibleCamera::ptzPanTilt(int pan, int tilt)
{
    LOG_INFO(QString("Visible camera: 云台方向 pan=%1 tilt=%2 (按住连续)")
                 .arg(qBound(-100, pan, 100)).arg(qBound(-100, tilt, 100)));
    sendPtzContinuous(pan, tilt, 0);
}

void VisibleCamera::ptzStop()
{
    sendPtzContinuous(0, 0, 0);
    LOG_DEBUG("Visible camera: 云台停止");
}

void VisibleCamera::queryZoomPosition()
{
    const QString url = QString("http://%1:%2/ISAPI/PTZCtrl/channels/1/status")
                            .arg(m_ip).arg(m_port);
    QNetworkRequest req{QUrl(url)};
    QNetworkReply *reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            const QString xml = QString::fromUtf8(reply->readAll());
            const QRegularExpressionMatch m =
                QRegularExpression("<absoluteZoom>(\\d+)</absoluteZoom>").match(xml);
            if (m.hasMatch()) emit zoomPositionChanged(m.captured(1).toInt());
        }
        reply->deleteLater();
    });
}

void VisibleCamera::focusStep(int dir)
{
    const int idx = focusTableIndexOf(m_focusLimited);
    const int newIdx = qBound(0, idx + (dir > 0 ? +1 : -1), FOCUS_DIST_N - 1);
    if (newIdx == idx) {
        LOG_INFO(QString("Visible camera: 聚焦距离已在极限档位 %1")
                     .arg(focusLimitedLabel(m_focusLimited)));
        return;
    }
    m_focusLimited = FOCUS_DIST_TABLE[newIdx];
    LOG_INFO(QString("Visible camera: 聚焦%1 -> 距离档位 %2 (档位值 %3, 已切手动对焦)")
                 .arg(dir > 0 ? "+ (更远)" : "- (更近)")
                 .arg(focusLimitedLabel(m_focusLimited)).arg(m_focusLimited));
    putFocusLimited(m_focusLimited);
}

void VisibleCamera::putFocusLimited(int limited)
{
    const QString url = QString("http://%1:%2/ISAPI/Image/channels/1").arg(m_ip).arg(m_port);
    QNetworkRequest getReq{QUrl(url)};
    QNetworkReply *getReply = m_networkManager->get(getReq);
    connect(getReply, &QNetworkReply::finished, this, [this, getReply, url, limited]() {
        if (getReply->error() != QNetworkReply::NoError) {
            LOG_ERROR(QString("Visible camera: 读取图像参数失败, 无法设置聚焦距离: %1")
                          .arg(getReply->errorString()));
            getReply->deleteLater();
            return;
        }
        QString xml = QString::fromUtf8(getReply->readAll());
        getReply->deleteLater();

        const QString seg = QString(
            "<FocusConfiguration version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
            "<focusStyle>MANUAL</focusStyle><focusLimited>%1</focusLimited></FocusConfiguration>").arg(limited);
        QRegularExpression re("<FocusConfiguration[^>]*>.*?</FocusConfiguration>",
                              QRegularExpression::DotMatchesEverythingOption);
        if (re.match(xml).hasMatch()) {
            xml.replace(re, seg);
        } else {
            xml.replace("</ImageChannel>", seg + "</ImageChannel>");
        }

        QNetworkRequest putReq{QUrl(url)};
        putReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml");
        QNetworkReply *putReply = m_networkManager->put(putReq, xml.toUtf8());
        connect(putReply, &QNetworkReply::finished, this, [putReply, limited]() {
            if (putReply->error() != QNetworkReply::NoError) {
                LOG_ERROR(QString("Visible camera: 设置聚焦距离(%1)失败: %2 (HTTP %3)")
                              .arg(VisibleCamera::focusLimitedLabel(limited))
                              .arg(putReply->errorString())
                              .arg(putReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
            } else {
                LOG_INFO(QString("Visible camera: 聚焦距离设置成功 -> %1")
                             .arg(VisibleCamera::focusLimitedLabel(limited)));
            }
            putReply->deleteLater();
        });
    });
}

void VisibleCamera::irisStep(int dir)
{
    const int idx = irisTableIndexOf(m_irisLevelRaw);
    const int newIdx = qBound(0, idx + (dir > 0 ? -1 : +1), IRIS_TABLE_N - 1);
    if (newIdx == idx) {
        LOG_INFO(QString("Visible camera: 光圈已在极限档位 IrisLevel=%1").arg(m_irisLevelRaw));
        return;
    }
    m_irisLevelRaw = IRIS_TABLE[newIdx];
    forceManualExposure();   // 必须先切手动曝光, 否则自动曝光会立刻把亮度纠正回去
    sendIsapiCommand(QString("http://%1:%2/ISAPI/Image/channels/1/iris").arg(m_ip).arg(m_port),
                     QString("<Iris xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                             "<IrisLevel>%1</IrisLevel></Iris>").arg(m_irisLevelRaw));
    LOG_INFO(QString("Visible camera: 光圈%1 -> IrisLevel=%2 (滑条等效 %3)")
                 .arg(dir > 0 ? "+ (开大/更亮)" : "- (关小/更暗)")
                 .arg(m_irisLevelRaw).arg(irisSliderFromLevel(m_irisLevelRaw)));
}

void VisibleCamera::gotoPreset(int presetId)
{
    if (presetId <= 0) return;
    const QString url = QString("http://%1:%2/ISAPI/PTZCtrl/channels/1/presets/%3/goto")
                            .arg(m_ip).arg(m_port).arg(presetId);
    const QString body = QString(
        "<PTZPreset version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<id>%1</id></PTZPreset>").arg(presetId);
    QString note;
    if (presetId == 45)      note = " (一键巡航)";
    else if (presetId == 49) note = " (掉电记忆)";
    LOG_INFO(QString("Visible camera: 调用预置点 %1%2").arg(presetId).arg(note));
    sendIsapiCommand(url, body);
}

// ============================================================
// 录像取证: 用独立 ffmpeg 进程录制 RTSP 码流(-c:v copy 直拷, 不重编码)
//   为什么不用 OpenCV VideoWriter: 本机可见光采集走 OpenCV 的 FFMPEG 后端,
//   进程内再打开 FFMPEG 编码器会在 VideoWriter::open() 处直接崩溃(实测 SIGSEGV)。
//   改为独立进程后与采集线程完全解耦, 直拷码流 CPU 开销极低, 且录像时长不受界面影响。
// ============================================================
void VisibleCamera::startRecording(const QString &basePath, const QString &rtspUrl)
{
    if (m_recording) return;
    // 录像地址: 优先使用传入地址(可指定主码流录制 4K), 否则沿用当前预览码流
    const QString url = rtspUrl.isEmpty() ? m_rtspUrl : rtspUrl;
    if (url.isEmpty()) {
        LOG_ERROR("Visible camera: 无可用 RTSP 地址, 无法开始录像");
        emit recordingFailed(QStringLiteral("可见光未连接, 无法录像"));
        return;
    }
    if (basePath.isEmpty()) {
        emit recordingFailed(QStringLiteral("录像保存路径为空"));
        return;
    }

    m_recordFilePath = basePath + ".mp4";

    if (m_recordProcess) {
        m_recordProcess->deleteLater();
        m_recordProcess = nullptr;
    }
    m_recordProcess = new QProcess(this);
    m_recordProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_recordProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &VisibleCamera::onRecordProcessFinished);
    connect(m_recordProcess, &QProcess::errorOccurred,
            this, &VisibleCamera::onRecordProcessError);

    const QStringList args = {
        "-hide_banner", "-loglevel", "warning",
        "-rtsp_transport", "tcp",
        "-i", url,
        "-an",                                  // 录像不需要音频
        "-c:v", "copy",                         // 直接拷贝码流(H.265), 零重编码
        "-tag:v", "hvc1",                       // 提升 mp4 在各类播放器中的兼容性
        "-movflags", "+faststart",
        "-y", m_recordFilePath
    };

    LOG_INFO(QString("Visible camera: 启动 ffmpeg 录像 -> %1").arg(m_recordFilePath));
    m_recordProcess->start(QStringLiteral("ffmpeg"), args);
    if (!m_recordProcess->waitForStarted(5000)) {
        LOG_ERROR(QString("Visible camera: ffmpeg 启动失败: %1").arg(m_recordProcess->errorString()));
        m_recordFilePath.clear();
        m_recordProcess->deleteLater();
        m_recordProcess = nullptr;
        emit recordingFailed(QStringLiteral("ffmpeg 启动失败, 请确认系统已安装 ffmpeg"));
        return;
    }

    m_recording = true;
    emit recordingStarted(m_recordFilePath);
}

void VisibleCamera::stopRecording()
{
    if (!m_recording || !m_recordProcess) {
        m_recording = false;
        return;
    }
    LOG_INFO(QString("Visible camera: 结束 ffmpeg 录像 -> %1").arg(m_recordFilePath));

    // ffmpeg 必须收到 SIGINT 才会优雅收尾并写入 moov, 否则 mp4 不可播放
    const qint64 pid = m_recordProcess->processId();
    if (pid > 0) {
        ::kill(static_cast<pid_t>(pid), SIGINT);
    } else {
        m_recordProcess->terminate();
    }
    if (!m_recordProcess->waitForFinished(6000)) {
        LOG_WARN("Visible camera: ffmpeg 未在 6s 内退出, 强制结束");
        m_recordProcess->kill();
        m_recordProcess->waitForFinished(2000);
    }
    // 最终状态与文件路径由 onRecordProcessFinished() 统一上报
}

void VisibleCamera::onRecordProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString path = m_recordFilePath;
    const bool wasRecording = m_recording;
    m_recording = false;
    m_recordFilePath.clear();

    if (m_recordProcess) {
        const QString out = QString::fromLocal8Bit(m_recordProcess->readAll()).trimmed();
        if (!out.isEmpty()) LOG_WARN(QString("Visible camera: ffmpeg 输出: %1").arg(out));
        m_recordProcess->deleteLater();
        m_recordProcess = nullptr;
    }

    if (!wasRecording) return;

    // 以"文件是否真正生成"作为成功判据:
    //   - SIGINT 收尾时 ffmpeg 正常退出, exitCode 为 255, 属正常现象;
    //   - 若相机拒绝第二路 RTSP 会话或参数错误, ffmpeg 会立刻退出且不产生文件。
    QFileInfo fi(path);
    Q_UNUSED(status);
    if (!path.isEmpty() && fi.exists() && fi.size() > 0) {
        LOG_INFO(QString("Visible camera: 录像已保存 -> %1 (%2 字节, ffmpeg exit=%3)")
                     .arg(path).arg(fi.size()).arg(exitCode));
        emit recordingStopped(path);
    } else {
        LOG_ERROR(QString("Visible camera: 录像失败, 未生成有效文件 (ffmpeg exit=%1)").arg(exitCode));
        emit recordingFailed(QStringLiteral("录像失败: 未生成有效视频文件(相机可能拒绝第二路码流)"));
    }
}

void VisibleCamera::onRecordProcessError(QProcess::ProcessError error)
{
    LOG_ERROR(QString("Visible camera: ffmpeg 进程错误: %1").arg(static_cast<int>(error)));
    if (m_recording) {
        m_recording = false;
        emit recordingFailed(QStringLiteral("录像进程发生错误"));
    }
}
