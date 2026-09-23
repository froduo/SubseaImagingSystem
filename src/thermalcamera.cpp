#include "thermalcamera.h"
#include "logger.h"
#include <QElapsedTimer>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QUrl>
#include <QRegularExpression>

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

// 生成与海康 ISAPI 一致的时间串: yyyy-MM-ddTHH:mm:ss+08:00
// 注意: Qt 的格式符 'z' 表示"毫秒"而不是时区偏移, 若写成 "...sszzz" 会得到
//       形如 2026-09-20T18:01:47476 的非法时间, 设备会直接返回 HTTP 400。
//       因此时区偏移必须按 offsetFromUtc() 自行拼接。
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
// ThermalCameraWorker
// ============================================================
ThermalCameraWorker::ThermalCameraWorker(QObject *parent)
    : QObject(parent)
{
}

ThermalCameraWorker::~ThermalCameraWorker()
{
    if (m_capture.isOpened()) {
        m_capture.release();
    }
}

void ThermalCameraWorker::setRtspUrl(const QString &url)
{
    m_rtspUrl = url;
}

void ThermalCameraWorker::requestStop()
{
    m_stopRequested = true;
}

// 由界面线程在消费完一帧后调用, 解除背压让采集循环可以投递下一帧
void ThermalCameraWorker::acknowledgeFrame()
{
    m_frameInFlight.store(false, std::memory_order_release);
}

void ThermalCameraWorker::startCapture()
{
    LOG_INFO(QString("=== Thermal Camera: Starting capture ==="));
    LOG_INFO(QString("  RTSP URL: %1").arg(m_rtspUrl));

    // Attempt 1: Try FFMPEG backend first (supports timeout properties)
    // NOTE: FFMPEG backend may throw unhandled C++ exceptions, wrap in try-catch
    try {
        LOG_INFO(QString("Thermal camera: Trying FFMPEG backend (timeout=%1ms)...").arg(CONNECTION_TIMEOUT_MS));
        m_capture.set(CAP_PROP_OPEN_TIMEOUT_MSEC, CONNECTION_TIMEOUT_MS);
        m_capture.set(CAP_PROP_READ_TIMEOUT_MSEC, READ_TIMEOUT_MS);
        m_capture.open(m_rtspUrl.toStdString(), cv::CAP_FFMPEG);
    } catch (const std::exception &e) {
        LOG_ERROR(QString("Thermal camera: FFMPEG exception: %1").arg(e.what()));
        m_capture.release();
    } catch (...) {
        LOG_ERROR("Thermal camera: FFMPEG unknown exception caught");
        m_capture.release();
    }

    if (!m_capture.isOpened()) {
        // Attempt 2: GStreamer + NVIDIA 硬件解码 (H.264/AVC)
        // HM-TD2069N-18D 各通道均为 H.264(H.264 High/Main), 且本平台 GStreamer
        // 未提供 avdec_h264, 因此必须使用 nvv4l2decoder 硬解 (输出 BGRx 后转 BGR)
        try {
            LOG_WARN("Thermal camera: FFMPEG backend failed, trying GStreamer H.264 hardware pipeline...");
            long long timeoutNs = static_cast<long long>(CONNECTION_TIMEOUT_MS) * 1000000; // ms -> ns
            QString gstPipeline = QString(
                "rtspsrc location=%1 latency=0 timeout=%2 protocols=tcp ! "
                "rtph264depay ! h264parse ! nvv4l2decoder ! nvvidconv ! "
                "video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR ! appsink")
                .arg(m_rtspUrl).arg(timeoutNs);
            LOG_INFO(QString("Thermal camera: GStreamer pipeline: %1").arg(gstPipeline));
            m_capture.open(gstPipeline.toStdString(), cv::CAP_GSTREAMER);
        } catch (const std::exception &e) {
            LOG_ERROR(QString("Thermal camera: GStreamer H.264 pipeline exception: %1").arg(e.what()));
            m_capture.release();
        } catch (...) {
            LOG_ERROR("Thermal camera: GStreamer H.264 pipeline unknown exception");
            m_capture.release();
        }
    }

    if (!m_capture.isOpened()) {
        // Attempt 3: GStreamer + NVIDIA 硬件解码 (H.265/HEVC 兼容分支)
        try {
            LOG_WARN("Thermal camera: H.264 pipeline failed, trying GStreamer H.265 hardware pipeline...");
            long long timeoutNs = static_cast<long long>(CONNECTION_TIMEOUT_MS) * 1000000;
            QString gstPipeline = QString(
                "rtspsrc location=%1 latency=0 timeout=%2 protocols=tcp ! "
                "rtph265depay ! h265parse ! nvv4l2decoder ! nvvidconv ! "
                "video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR ! appsink")
                .arg(m_rtspUrl).arg(timeoutNs);
            LOG_INFO(QString("Thermal camera: GStreamer pipeline: %1").arg(gstPipeline));
            m_capture.open(gstPipeline.toStdString(), cv::CAP_GSTREAMER);
        } catch (const std::exception &e) {
            LOG_ERROR(QString("Thermal camera: GStreamer H.265 pipeline exception: %1").arg(e.what()));
            m_capture.release();
        } catch (...) {
            LOG_ERROR("Thermal camera: GStreamer H.265 pipeline unknown exception");
            m_capture.release();
        }
    }

    if (!m_capture.isOpened()) {
        // Attempt 4: Try simple GStreamer with URL
        try {
            LOG_WARN("Thermal camera: GStreamer pipelines failed, trying GStreamer with URL...");
            m_capture.open(m_rtspUrl.toStdString(), cv::CAP_GSTREAMER);
        } catch (const std::exception &e) {
            LOG_ERROR(QString("Thermal camera: GStreamer URL exception: %1").arg(e.what()));
            m_capture.release();
        } catch (...) {
            LOG_ERROR("Thermal camera: GStreamer URL unknown exception");
            m_capture.release();
        }
    }

    if (!m_capture.isOpened()) {
        LOG_ERROR("Thermal camera: All backends failed to open RTSP stream");
        emit errorOccurred("无法连接热像仪RTSP流(所有后端均失败): " + m_rtspUrl);
        emit connectionStatusChanged(false);
        return;
    }

    // Set buffer size
    m_capture.set(cv::CAP_PROP_BUFFERSIZE, 3);

    // Log connection success and stream properties
    double frameW = m_capture.get(cv::CAP_PROP_FRAME_WIDTH);
    double frameH = m_capture.get(cv::CAP_PROP_FRAME_HEIGHT);
    double fps = m_capture.get(cv::CAP_PROP_FPS);
    double backendId = m_capture.get(cv::CAP_PROP_BACKEND);
    LOG_INFO(QString("Thermal camera: RTSP stream opened successfully"));
    LOG_INFO(QString("  Backend ID: %1").arg(static_cast<int>(backendId)));
    LOG_INFO(QString("  Frame size: %1x%2").arg(static_cast<int>(frameW)).arg(static_cast<int>(frameH)));
    LOG_INFO(QString("  Reported FPS: %1").arg(fps, 0, 'f', 1));

    emit connectionStatusChanged(true);

    cv::Mat frame;
    QElapsedTimer fpsTimer;
    fpsTimer.start();
    int frameCount = 0;
    int reconnectCount = 0;

    while (!m_stopRequested) {
        if (!m_capture.read(frame) || frame.empty()) {
            reconnectCount++;
            LOG_WARN(QString("Thermal camera: Empty frame received, reconnect attempt %1/%2")
                         .arg(reconnectCount).arg(MAX_RECONNECT_ATTEMPTS));

            if (reconnectCount > MAX_RECONNECT_ATTEMPTS) {
                LOG_ERROR(QString("Thermal camera: Max reconnect attempts (%1) exceeded, giving up")
                              .arg(MAX_RECONNECT_ATTEMPTS));
                emit errorOccurred(QString("热像仪重连失败，已尝试 %1 次").arg(MAX_RECONNECT_ATTEMPTS));
                emit connectionStatusChanged(false);
                break;
            }

            emit connectionStatusChanged(false);
            m_capture.release();
            QThread::msleep(RECONNECT_DELAY_MS);

            // Try FFMPEG first on reconnect (supports timeout)
            try {
                LOG_INFO(QString("Thermal camera: Reconnect #%1 - trying FFMPEG...").arg(reconnectCount));
                m_capture.set(CAP_PROP_OPEN_TIMEOUT_MSEC, CONNECTION_TIMEOUT_MS);
                m_capture.set(CAP_PROP_READ_TIMEOUT_MSEC, READ_TIMEOUT_MS);
                m_capture.open(m_rtspUrl.toStdString(), cv::CAP_FFMPEG);
            } catch (...) {
                LOG_ERROR(QString("Thermal camera: Reconnect #%1 FFMPEG exception").arg(reconnectCount));
                m_capture.release();
            }
            if (!m_capture.isOpened()) {
                try {
                    LOG_INFO(QString("Thermal camera: Reconnect #%1 - trying GStreamer...").arg(reconnectCount));
                    m_capture.open(m_rtspUrl.toStdString(), cv::CAP_GSTREAMER);
                } catch (...) {
                    LOG_ERROR(QString("Thermal camera: Reconnect #%1 GStreamer exception").arg(reconnectCount));
                    m_capture.release();
                }
            }

            if (m_capture.isOpened()) {
                double rw = m_capture.get(cv::CAP_PROP_FRAME_WIDTH);
                double rh = m_capture.get(cv::CAP_PROP_FRAME_HEIGHT);
                LOG_INFO(QString("Thermal camera: Reconnected successfully (attempt #%1), frame: %2x%3")
                             .arg(reconnectCount).arg(static_cast<int>(rw)).arg(static_cast<int>(rh)));
                emit connectionStatusChanged(true);
                reconnectCount = 0;  // Reset counter on success
            } else {
                LOG_ERROR(QString("Thermal camera: Reconnect attempt #%1 failed").arg(reconnectCount));
            }
            continue;
        }

        // ---- 背压: 上一帧还未被界面线程消费时丢弃本帧, 防止事件队列/内存无限增长 ----
        if (m_frameInFlight.load(std::memory_order_acquire)) {
            m_droppedFrames++;
            if (m_lastDropLogMs == 0 ||
                QDateTime::currentMSecsSinceEpoch() - m_lastDropLogMs > 5000) {
                m_lastDropLogMs = QDateTime::currentMSecsSinceEpoch();
                LOG_DEBUG(QString("Thermal camera: 界面未消费, 已丢弃 %1 帧 (背压丢帧)")
                              .arg(m_droppedFrames));
            }
            continue;
        }

        // Convert BGR to RGB then to QImage
        cv::Mat rgb;
        cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);

        QImage qimg(rgb.data, rgb.cols, rgb.rows,
                     static_cast<int>(rgb.step), QImage::Format_RGB888);
        // Deep copy since rgb will be released
        m_frameInFlight.store(true, std::memory_order_release);
        emit frameReady(qimg.copy());

        frameCount++;
        qint64 elapsed = fpsTimer.elapsed();
        if (elapsed >= 1000) {
            double actualFps = frameCount * 1000.0 / elapsed;
            emit fpsUpdated(actualFps);
            frameCount = 0;
            fpsTimer.restart();
        }
    }

    m_capture.release();
    LOG_INFO("Thermal camera: Capture stopped");
}

// ============================================================
// ThermalCamera (controller)
// ============================================================
ThermalCamera::ThermalCamera(QObject *parent)
    : QObject(parent)
{
    // ISAPI(HTTP) 控制通道: 用于连接后自动校时等操作, 与 RTSP 采集线程互不影响。
    // 认证方式为海康默认的 Digest 认证。
    m_net = new QNetworkAccessManager(this);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [this](QNetworkReply *, QAuthenticator *auth) {
                auth->setUser(m_user);
                auth->setPassword(m_pass);
            });
}

void ThermalCamera::setCredentials(const QString &ip, int port,
                                   const QString &user, const QString &pass)
{
    m_ip = ip;
    m_port = port;
    m_user = user;
    m_pass = pass;
    LOG_INFO(QString("Thermal camera credentials set: %1:%2 (user: %3)")
                 .arg(m_ip).arg(m_port).arg(m_user));
}

// ============================================================
// 校时: 把热像仪时间校准为本机系统时间(误差目标 < 1 秒)
//
//   流程: GET 读设备时间文档 -> 写入 -> 按实测 PUT 耗时补偿重写 -> 读回校验
//   为什么写两次: 设备是把收到的 localTime 直接当作当前时间, 而一次 PUT 从发出到
//                设备生效本身有耗时(实测热像仪约 0.3s, 可见光约 2.7s),
//                只写一次会让设备时间固定偏慢一个 PUT 耗时。
//                第一次写入用于测量耗时, 第二次用该耗时补偿。
//   注: 该热像仪返回的命名空间为 www.isapi.org/ver20/XMLSchema(可见光为 hikvision),
//       因此沿用设备返回的文档结构, 不硬编码命名空间。
// ============================================================
void ThermalCamera::syncSystemTime()
{
    if (m_ip.isEmpty() || !m_net) {
        emit timeSyncFinished(false, QStringLiteral("未配置热像仪 IP"));
        return;
    }

    m_timeSyncUrl = QString("http://%1:%2/ISAPI/System/time").arg(m_ip).arg(m_port);
    LOG_INFO(QString("热像仪校时: 读取设备时间 -> %1").arg(m_timeSyncUrl));

    QNetworkRequest getReq{QUrl(m_timeSyncUrl)};
    QNetworkReply *getReply = m_net->get(getReq);
    connect(getReply, &QNetworkReply::finished, this, [this, getReply]() {
        getReply->deleteLater();

        const int httpCode =
            getReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (getReply->error() != QNetworkReply::NoError) {
            const QString info = QStringLiteral("读取设备时间失败(HTTP %1: %2)")
                                     .arg(httpCode).arg(getReply->errorString());
            LOG_ERROR(QString("热像仪校时失败: %1").arg(info));
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
//   它包含 HTTP 往返 + 设备内部应用时间, 且每次不固定, 因此每轮写入后
//   读回校验并迭代修正补偿量, 而不是只靠一次耗时测量。
void ThermalCamera::writeTimePass(int pass)
{
    const QString stamp = isapiTimeString(
        QDateTime::currentDateTime().addMSecs(m_syncCompensationMs));

    QString body = m_timeDocTemplate;
    // 沿用设备返回的命名空间, 先取出该命名空间再构造回写文档
    QString ns = QStringLiteral("http://www.hikvision.com/ver20/XMLSchema");
    const QRegularExpression nsRe(QStringLiteral("xmlns=\"([^\"]+)\""));
    const QRegularExpressionMatch nsMatch = nsRe.match(m_timeDocTemplate);
    if (nsMatch.hasMatch()) ns = nsMatch.captured(1);

    const QRegularExpression ltRe(QStringLiteral("<localTime>[^<]*</localTime>"));
    if (ltRe.match(body).hasMatch()) {
        body.replace(ltRe, QStringLiteral("<localTime>%1</localTime>").arg(stamp));
    } else {
        QString tz = QStringLiteral("CST-8:00:00");
        const QRegularExpression tzRe(QStringLiteral("<timeZone>([^<]*)</timeZone>"));
        const QRegularExpressionMatch tzMatch = tzRe.match(m_timeDocTemplate);
        if (tzMatch.hasMatch() && !tzMatch.captured(1).trimmed().isEmpty()) {
            tz = tzMatch.captured(1).trimmed();
        }
        body = QStringLiteral(
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                   "<Time xmlns=\"%1\"><timeMode>manual</timeMode>"
                   "<localTime>%2</localTime><timeZone>%3</timeZone></Time>")
                   .arg(ns, stamp, tz);
    }

    QNetworkRequest putReq{QUrl(m_timeSyncUrl)};
    putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml"));
    QNetworkReply *putReply = m_net->put(putReq, body.toUtf8());
    connect(putReply, &QNetworkReply::finished, this, [this, putReply, pass, stamp]() {
        putReply->deleteLater();
        const int code =
            putReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (putReply->error() != QNetworkReply::NoError) {
            const QString info = QStringLiteral("写入失败(HTTP %1: %2)")
                                     .arg(code).arg(putReply->errorString());
            LOG_ERROR(QString("热像仪校时失败: %1").arg(info));
            emit timeSyncFinished(false, info);
            return;
        }
        LOG_INFO(QString("热像仪校时: 第 %1 轮写入 %2 (补偿 %3ms)")
                     .arg(pass).arg(stamp).arg(m_syncCompensationMs));
        verifySyncedTime(pass);
    });
}

// 读回设备时间, 计算与本机的偏差, 并按偏差迭代修正补偿量
//   Δ = 设备时间 - 本机时间 = 补偿量 - 实际延迟  =>  新补偿量 = 旧补偿量 - Δ
//   偏差收敛到 500ms 以内或达到 5 轮上限后上报结果
void ThermalCamera::verifySyncedTime(int pass)
{
    if (m_timeSyncUrl.isEmpty() || !m_net) return;

    const QDateTime t1 = QDateTime::currentDateTime();
    QNetworkRequest req{QUrl(m_timeSyncUrl)};
    QNetworkReply *reply = m_net->get(req);
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
            const QDateTime mid = t1.addMSecs(t1.msecsTo(t2) / 2);
            if (dev.isValid()) {
                const qint64 diffMs = dev.toMSecsSinceEpoch() - mid.toMSecsSinceEpoch();
                // 迭代修正: 把补偿量调整到使"设备时间 == 本机时间"
                m_syncCompensationMs =
                    qBound<qint64>(-5000, m_syncCompensationMs - diffMs, 5000);
                if (qAbs(diffMs) > 500 && pass < 5) {
                    LOG_INFO(QString("热像仪校时: 本轮偏差 %1ms, 进行第 %2 轮修正")
                                 .arg(diffMs).arg(pass + 1));
                    writeTimePass(pass + 1);
                    return;
                }
                info = QStringLiteral("已校准为 %1 (与本机偏差 %2 ms)").arg(devStr).arg(diffMs);
                if (qAbs(diffMs) > 1000) {
                    LOG_WARN(QString("热像仪校时偏差仍偏大: %1 ms").arg(diffMs));
                }
            } else {
                info = QStringLiteral("已校准为 %1").arg(devStr);
            }
        } else {
            info = QStringLiteral("已校准(未取到回读值)");
        }

        LOG_INFO(QString("热像仪校时成功: %1").arg(info));
        emit timeSyncFinished(true, info);
    });
}

ThermalCamera::~ThermalCamera()
{
    stop();
}

void ThermalCamera::start(const QString &rtspUrl)
{
    if (m_thread.isRunning() || m_worker) {
        stop();
    }
    if (m_worker) {
        // 上一次采集线程仍未退出(见 stop() 中的保护), 此时不能再新建 worker
        LOG_ERROR("Thermal camera: 上一次采集线程尚未退出, 忽略本次启动请求");
        return;
    }

    m_worker = new ThermalCameraWorker();
    m_worker->setRtspUrl(rtspUrl);
    m_worker->moveToThread(&m_thread);

    // Connect signals
    // 注意: 这里在消费完一帧后必须 acknowledgeFrame() 解除背压, 否则采集端会持续丢帧
    connect(m_worker, &ThermalCameraWorker::frameReady,
            this, [this](const QImage &frame) {
                if (m_worker) m_worker->acknowledgeFrame();
                emit frameReady(frame);
            }, Qt::QueuedConnection);
    connect(m_worker, &ThermalCameraWorker::connectionStatusChanged,
            this, &ThermalCamera::connectionStatusChanged, Qt::QueuedConnection);
    connect(m_worker, &ThermalCameraWorker::fpsUpdated,
            this, &ThermalCamera::fpsUpdated, Qt::QueuedConnection);
    connect(m_worker, &ThermalCameraWorker::errorOccurred,
            this, &ThermalCamera::errorOccurred, Qt::QueuedConnection);

    connect(this, &ThermalCamera::startRequested, m_worker, &ThermalCameraWorker::startCapture);

    m_thread.start();
    emit startRequested();
}

void ThermalCamera::stop()
{
    if (m_worker) {
        m_worker->requestStop();   // 采集循环每轮开头检查该标志
    }
    if (m_thread.isRunning()) {
        m_thread.quit();
        // 采集循环可能正阻塞在 m_capture.read()(FFMPEG 读超时 10s), 故等待时间需大于该超时。
        // 严禁 terminate(): 硬杀线程会破坏 OpenCV/GStreamer 内部状态, 退出时导致 SIGSEGV/SIGABRT。
        if (!m_thread.wait(12000)) {
            LOG_WARN("Thermal camera: 采集线程未在 12s 内退出, 暂不销毁 worker");
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

bool ThermalCamera::isRunning() const
{
    return m_thread.isRunning();
}
