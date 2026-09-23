#ifndef VISIBLECAMERA_H
#define VISIBLECAMERA_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QString>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <atomic>
#include <opencv2/opencv.hpp>

class VisibleCameraWorker : public QObject
{
    Q_OBJECT

public:
    explicit VisibleCameraWorker(QObject *parent = nullptr);
    ~VisibleCameraWorker();

    void setRtspUrl(const QString &url);
    // 预览目标尺寸: >0 时由 nvvidconv 在 GPU 上缩放到该尺寸后再转 BGR,
    // 可把 4K(3840x2160) 的 CPU 色彩转换/拷贝量降到 1/4, 帧率从约 16fps 提升到接近满帧。
    // 0/0 表示不缩放(保持码流原始分辨率)。
    void setPreviewSize(int w, int h);
    void requestStop();

    // 背压: 消费端(界面线程)处理完一帧后调用, 允许继续投递
    // 若上一帧尚未被消费, 采集循环直接丢弃当前帧, 避免事件队列与内存无限增长
    // (1920x1080 每帧约 6MB, 界面渲染追不上时会迅速耗尽内存触发 OOM)
    void acknowledgeFrame();

public slots:
    void startCapture();

signals:
    void frameReady(const QImage &frame);
    void connectionStatusChanged(bool connected);
    void fpsUpdated(double fps);
    void errorOccurred(const QString &error);
    void decoderChanged(const QString &decoder);   // 实际生效的解码链路描述(硬解/软解)

private:
    bool openHardwareStream();   // GStreamer + nvv4l2decoder 硬解(可 GPU 缩放), 成功返回 true
    bool openSoftStream();       // FFMPEG 软解(后备), 成功返回 true

    QString m_rtspUrl;
    int m_previewW = 0;           // 预览目标宽(GPU 缩放), 0 = 不缩放
    int m_previewH = 0;           // 预览目标高(GPU 缩放), 0 = 不缩放
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_frameInFlight{false};   // 有帧已在投递途中
    cv::VideoCapture m_capture;
    QString m_decoderDesc;        // 当前生效的解码链路(用于日志/界面显示)

    int m_droppedFrames = 0;      // 因界面未消费而丢弃的帧数
    qint64 m_lastDropLogMs = 0;   // 丢帧统计日志节流
    // ---- 低延时/健壮性(2026-09-22) ----
    int m_emptyFrameStreak = 0;    // 连续空帧计数: 偶发空帧不重连, 避免"重连->等 I 帧"卡顿
    qint64 m_lastFrameMs = 0;      // 最近一次成功取到帧的时间戳(判定"长时间无帧")
    qint64 m_frameInFlightMs = 0;  // 上一帧投递时间戳(背压超时自愈, 防界面卡死导致采集停摆)
};

class VisibleCamera : public QObject
{
    Q_OBJECT

public:
    explicit VisibleCamera(QObject *parent = nullptr);
    ~VisibleCamera();

    void start(const QString &rtspUrl);
    void stop();
    bool isRunning() const;

    // 预览目标尺寸(GPU 缩放): 在主码流 4K 预览时设为 1920x1080 可显著降低界面负担;
    // 需在 start() 之前设置, start() 时下发到采集线程。0/0 表示不缩放。
    void setPreviewSize(int w, int h);
    void setStreamChannel(int channel);   // 记录当前码流通道(101/102/103), 供录像分辨率提示
    int  streamChannel() const { return m_streamChannel; }
    // 依据码流通道返回原生分辨率文字(如 "3840x2160"), 用于界面提示与文件命名
    static QString streamResolutionText(int channel);

    // Set device credentials for ISAPI commands
    void setCredentials(const QString &ip, int port, const QString &user, const QString &pass);

    // ISAPI control functions (2026-09 已按 iDS-2ZMN2312S 实测接口修正)
    void setAutoFocus(bool enable);                  // true=AUTO 连续自动对焦 / false=MANUAL
    void setFocusMode(const QString &focusStyle);    // AUTO / SEMIAUTOMATIC / MANUAL
    void zoomIn();                                   // PTZ 连续变焦 (zoom=+speed)
    void zoomOut();                                   // PTZ 连续变焦 (zoom=-speed)
    void zoomStop();                                  // PTZ 变焦停止 (zoom=0)
    void zoomStep(int dir);                           // 单击步进变焦: 单向移动一档后自动停止(不反向)
    bool isZoomBusy() const { return m_zoomTarget > 0; }   // 是否正在执行变焦步进(用于回读节流)
    void setZoomPosition(int pos);                    // 闭环移动至指定变焦位置 (10..230, <=0 表示停止)
    void queryZoomPosition();                         // 回读当前变焦位置(状态显示用)

    // ---- 云台方向(按住连续转动, 松手停止), 速度 -100..100 ----
    void ptzPanTilt(int pan, int tilt);
    void ptzStop();

    // ---- 镜头: 聚焦距离档位 / 光圈档位(与相机网页一致的操作粒度) ----
    // 聚焦: 该机芯无"聚焦±步进"接口, 官方网页给的是"对焦模式 + 最小聚焦距离档位",
    //       因此这里按档位步进: dir>0 更远(趋向无限远), dir<0 更近。
    void focusStep(int dir);
    // 光圈: 直接步进设备 IrisLevel 档位(160..2200, 数值越大画面越暗), dir>0 开大(更亮)
    void irisStep(int dir);
    int  irisLevel() const { return m_irisLevelRaw; }        // 当前 IrisLevel(设备值)
    int  focusLimited() const { return m_focusLimited; }     // 当前聚焦距离档位值

    // ---- 预置点调用: 45=一键巡航, 49=掉电记忆(该设备内置特殊预置点) ----
    void gotoPreset(int presetId);

    // 档位换算(供界面同步滑条与显示)
    static int irisLevelFromSlider(int v);                   // 滑条0..100 -> IrisLevel
    static int irisSliderFromLevel(int level);               // IrisLevel -> 滑条0..100
    static QString focusLimitedLabel(int v);                 // 档位值 -> 可读文字(如 6.0m)

    void captureImage(const QString &savePath);
    void setExposure(int value);                      // 滑条 0..100 => 暗..亮 (手动曝光: 光圈+增益)
    void setIris(int value);                          // 光圈滑条 0..100 => 暗..亮 (IrisLevel 2200..160)
    void setGain(int value);                          // 增益滑条 0..100 (GainLevel)
    void setAutoExposure();                           // 恢复自动曝光

    // ---- 图像参数(对应相机 Web 端"图像"配置) ----
    // 相机该端点不接受片段, 必须整篇读-改-写; block 为子块名(如 IrcutFilter/BLC/WDR),
    // tag 为块内标签(如 IrcutFilterType), value 为目标值。
    void setImageParam(const QString &block, const QString &tag, const QString &value);
    void queryImageParams();                          // 回读整篇图像参数(供界面同步)

    // ---- 校时: 与本地系统时间校准(每次连接相机后自动调用一次) ----
    // 策略: 先 GET 设备时间文档(保留设备时区与时间模式), 仅把 localTime 改写为
    //       本机时间后原样写回, 不改变设备的时区/对时模式配置。
    void syncSystemTime();

    // ---- 日夜切换(IR-cut) 便捷封装: day=白天(彩色) / night=夜晚(黑白) / auto=自动 ----
    void setIrcutFilterDay();                         // 白天(强制彩色)
    void setIrcutFilterAuto();                        // 自动
    void setIrcutFilterNight();                       // 夜晚(黑白)

    // ---- 录像取证 ----
    // 采用外部 ffmpeg 进程录制 RTSP 码流(-c:v copy 直拷), 不占用采集线程, 也不与
    // OpenCV 的 FFMPEG 采集后端冲突(实测进程内 VideoWriter 与该后端并存会导致崩溃)。
    // 开始录像(文件基名, 不含扩展名)
    // rtspUrl 为空则使用当前预览码流; 可传入主码流地址以录制 4K(直拷, 不解码)
    void startRecording(const QString &basePath, const QString &rtspUrl = QString());
    void stopRecording();                             // 结束录像并保存文件
    bool isRecording() const { return m_recording; }

signals:
    void frameReady(const QImage &frame);
    void connectionStatusChanged(bool connected);
    void fpsUpdated(double fps);
    void errorOccurred(const QString &error);
    void decoderChanged(const QString &decoder);   // 实际生效的解码链路(硬解/软解)
    void imageCaptured(const QString &filePath);
    void zoomPositionChanged(int absoluteZoom);   // 相机回读的实际变焦位置 (10..230)
    void recordingStarted(const QString &filePath);
    void recordingStopped(const QString &filePath);
    void recordingFailed(const QString &error);
    void imageConfigReceived(const QString &xml);     // 图像参数回读结果
    void timeSyncFinished(bool ok, const QString &info);   // 校时结果(供界面记录/提示)
    void startRequested();

public slots:
    void sendIsapiCommand(const QString &url, const QString &body);

private slots:
    void onRecordProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onRecordProcessError(QProcess::ProcessError error);

private:
    void sendPtzZoom(int speed);   // PTZ 连续变焦速率: -100..100, 0 = 停止
    void sendPtzContinuous(int pan, int tilt, int zoom);   // 云台连续转动/变焦(0=停止)
    void putFocusLimited(int limited);                     // 写入聚焦距离档位(整篇图像参数改写)
    void pollZoomStep();           // 变焦闭环: 读取实际位置并决定继续/停止
    void stopZoomConfirm();        // 二次确认停止: 再次下发 zoom=0, 防止镜头持续冲到极限
    void applyExposure();          // 曝光去抖后实际下发参数
    void applyIris();              // 光圈去抖后实际下发参数
    void applyGain();              // 增益去抖后实际下发参数
    void forceManualExposure();    // 切手动曝光 (否则自动曝光会立即纠正亮度)

    // ---- 校时内部流程 ----
    void writeTimePass(int pass);   // 第 pass 轮写入(补偿量 m_syncCompensationMs 已含在写入值中)
    void verifySyncedTime(int pass); // 读回校验: 按偏差迭代修正补偿量, 满足精度或达到上限后上报

    QThread m_thread;
    VisibleCameraWorker *m_worker = nullptr;
    QNetworkAccessManager *m_networkManager = nullptr;
    QString m_ip;
    int m_port = 80;
    QString m_user;
    QString m_pass;
    QString m_rtspUrl;             // 当前采集用的 RTSP 地址(录像 ffmpeg 复用)
    int m_previewW = 0;            // 预览目标宽(GPU 缩放)
    int m_previewH = 0;            // 预览目标高(GPU 缩放)
    int m_streamChannel = 101;     // 当前码流通道(101 主/102 子/103 第三)
    QImage m_lastFrame;

    QTimer *m_zoomStopTimer = nullptr;     // 变焦安全自动停止 (防止变焦失控)
    QTimer *m_exposureTimer = nullptr;     // 曝光滑条去抖
    QTimer *m_irisTimer = nullptr;         // 光圈滑条去抖
    QTimer *m_gainTimer = nullptr;         // 增益滑条去抖
    int m_pendingExposure = 50;
    int m_pendingIris = 100;
    int m_pendingGain = 60;

    int m_zoomTarget = -1;              // 变焦闭环目标位置, <=0 表示无进行中的闭环
    int m_zoomStepDir = 0;              // 当前步进方向(+1 拉近 / -1 拉远), 闭环期间不变向
    qint64 m_zoomDeadlineMs = 0;        // 闭环超时时间戳

    int m_irisLevelRaw = 1600;          // 设备当前 IrisLevel(用于 光圈± 步进)
    int m_focusLimited = 600;           // 设备当前聚焦距离档位(用于 聚焦± 步进)

    // ---- 录像状态 ----
    QProcess *m_recordProcess = nullptr;
    QString m_recordFilePath;
    bool m_recording = false;

    // ---- 校时状态 ----
    QString m_timeSyncUrl;             // 校时目标 URL(/ISAPI/System/time)
    QString m_timeDocTemplate;         // 设备时间文档模板(命名空间/时区/对时模式原样保留)
    qint64 m_syncCompensationMs = 0;   // 写入值相对"本机当前时间"的补偿量(闭环自学习, 跨连接保留)
};

#endif // VISIBLECAMERA_H
