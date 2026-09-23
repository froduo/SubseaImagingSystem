#ifndef THERMALCAMERA_H
#define THERMALCAMERA_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include <atomic>
#include <opencv2/opencv.hpp>

class QNetworkAccessManager;

class ThermalCameraWorker : public QObject
{
    Q_OBJECT

public:
    explicit ThermalCameraWorker(QObject *parent = nullptr);
    ~ThermalCameraWorker();

    void setRtspUrl(const QString &url);
    void requestStop();

    // 背压: 消费端(界面线程)处理完一帧后调用, 允许继续投递
    // 若上一帧尚未被消费则丢弃当前帧, 防止事件队列与内存无限增长
    void acknowledgeFrame();

public slots:
    void startCapture();

signals:
    void frameReady(const QImage &frame);
    void connectionStatusChanged(bool connected);
    void fpsUpdated(double fps);
    void errorOccurred(const QString &error);

private:
    QString m_rtspUrl;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_frameInFlight{false};   // 有帧已在投递途中
    cv::VideoCapture m_capture;

    int m_droppedFrames = 0;      // 因界面未消费而丢弃的帧数
    qint64 m_lastDropLogMs = 0;   // 丢帧统计日志节流
};

class ThermalCamera : public QObject
{
    Q_OBJECT

public:
    explicit ThermalCamera(QObject *parent = nullptr);
    ~ThermalCamera();

    void start(const QString &rtspUrl);
    void stop();
    bool isRunning() const;

    // ---- ISAPI(HTTP) 能力: 目前用于"连接后自动校时" ----
    // 与 RTSP 视频链路相互独立; 未设置凭据时校时会直接返回失败。
    void setCredentials(const QString &ip, int port, const QString &user, const QString &pass);
    void syncSystemTime();          // 与本地系统时间校准(每次连接相机后自动调用一次)

signals:
    void frameReady(const QImage &frame);
    void connectionStatusChanged(bool connected);
    void fpsUpdated(double fps);
    void errorOccurred(const QString &error);
    void startRequested();
    void timeSyncFinished(bool ok, const QString &info);   // 校时结果(供界面记录/提示)

private:
    QThread m_thread;
    ThermalCameraWorker *m_worker = nullptr;

    // ---- ISAPI 控制通道(校时等) ----
    QString m_ip;
    int m_port = 80;
    QString m_user;
    QString m_pass;
    QNetworkAccessManager *m_net = nullptr;

    // ---- 校时流程 ----
    void writeTimePass(int pass);    // 第 pass 轮写入(补偿量 m_syncCompensationMs 已含在写入值中)
    void verifySyncedTime(int pass);  // 读回校验: 按偏差迭代修正补偿量, 满足精度或达上限后上报
    QString m_timeSyncUrl;            // 校时目标 URL(/ISAPI/System/time)
    QString m_timeDocTemplate;        // 设备时间文档模板(命名空间/时区/对时模式原样保留)
    qint64 m_syncCompensationMs = 0;  // 写入值相对"本机当前时间"的补偿量(闭环自学习, 跨连接保留)
};

#endif // THERMALCAMERA_H
