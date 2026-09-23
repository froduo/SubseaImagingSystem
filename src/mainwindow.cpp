#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "thermalcamera.h"
#include "visiblecamera.h"
#include "irlaser.h"
#include "heatsourcedetector.h"
#include "anglecalculator.h"
#include "tcpserver.h"
#include "configmanager.h"
#include "logger.h"
#include <QMessageBox>
#include <QDateTime>
#include <QFileDialog>
#include <QSerialPortInfo>
#include <QApplication>
#include <QPalette>
#include <QSizePolicy>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QEvent>
#include <QFileInfo>
#include <QScrollBar>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>
#include <QTabWidget>
#include <QTabBar>
#include <QRegularExpression>
#include <cmath>

// 变焦位置(absoluteZoom)空间: 10..230 对应光学倍率 1.0×..23.0× (ZoomLimitRatio=23)
static const int ZOOM_POS_MIN = 10;
static const int ZOOM_POS_MAX = 230;
static const double ZOOM_POS_PER_X = 10.0;   // 每 10 个位置 = 1×

// 云台方向速度(-100..100, 实测该机芯连续云台范围)
static const int PTZ_PAN_SPEED  = 40;
static const int PTZ_TILT_SPEED = 40;

// 可见光码流通道号: 101=主码流(4K) / 102=子码流(704x576) / 103=第三码流(1080p)
static const int VISIBLE_STREAM_MAIN  = 101;
static const int VISIBLE_STREAM_SUB   = 102;
static const int VISIBLE_STREAM_THIRD = 103;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // Initialize modules
    m_thermalCamera = new ThermalCamera(this);
    m_visibleCamera = new VisibleCamera(this);
    m_irLaser = new IRLaser(this);
    m_heatDetector = new HeatSourceDetector(this);
    m_angleCalculator = new AngleCalculator(this);
    m_tcpServer = new TcpServer(this);

    // Configure from settings
    ConfigManager &cfg = ConfigManager::instance();
    m_heatDetector->setThresholdDelta(cfg.tempThresholdDelta());
    m_heatDetector->setMinArea(cfg.minArea());
    m_heatDetector->setMaxSources(cfg.maxSources());
    // 手动模式筛选参数: 最低温度 / 最小面积像素
    m_heatDetector->setManualMinTemp(cfg.getValue("detection/min_temp", 40.0).toDouble());
    m_heatDetector->setManualMinArea(cfg.getValue("detection/min_area_pixels", cfg.minArea()).toInt());
    m_heatDetector->setSelectMode(cfg.getValue("detection/manual_mode", false).toBool()
                                      ? HeatSourceDetector::Manual
                                      : HeatSourceDetector::AutoBrightArea);
    m_angleCalculator->setFOV(cfg.hfov(), cfg.vfov());
    m_angleCalculator->setImageSize(cfg.imageWidth(), cfg.imageHeight());

    // Set visible camera ISAPI credentials
    m_visibleCamera->setCredentials(
        cfg.visibleCameraIp(), cfg.visibleCameraPort(),
        cfg.visibleCameraUser(), cfg.visibleCameraPass());

    // Set thermal camera ISAPI credentials (用于连接后自动校时等控制操作)
    m_thermalCamera->setCredentials(
        cfg.thermalCameraIp(), cfg.thermalCameraPort(),
        cfg.thermalCameraUser(), cfg.thermalCameraPass());

    // Detection timer
    m_detectionTimer = new QTimer(this);
    connect(m_detectionTimer, &QTimer::timeout, this, &MainWindow::updateDetection);

    // Setup connections
    setupUi_connections();

    // 代码构建的扩展界面: 热源检测分组 / TCP地址 / 日志窗体 / 分辨率标签
    createExtraUi();

    // ---- 防止窗口纵向逐渐变大 ----
    // 原因: QLabel 的 sizeHint 来自其 pixmap, 每帧 setPixmap 后布局会把窗口"顶大"1~2px 并累积。
    // 处理: 视频标签与日志视图改为忽略尺寸提示(Ignored), 并由 minimumSize + 布局拉伸控制大小。
    //   - 热像仪 640x512 (5:4)   -> 最小 320x256
    //   - 可见光 1920x1080 (16:9) -> 最小 480x270
    ui->labelThermal->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    ui->labelThermal->setMinimumSize(320, 256);
    ui->labelThermal->setAlignment(Qt::AlignCenter);
    ui->labelThermal->setScaledContents(false);

    ui->labelVisible->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    ui->labelVisible->setMinimumSize(480, 270);
    ui->labelVisible->setAlignment(Qt::AlignCenter);
    ui->labelVisible->setScaledContents(false);

    // ---- 视频区宽度分配: 按两路图像宽高比 (640/512 : 1920/1080 ≈ 1.25 : 1.78) ----
    // 等高显示时理论上为 41% : 59%; 这里给热像仪(+4%)更多宽度, 保证热源目标细节清晰可见。
    if (ui->videoFrameLayout) {
        ui->videoFrameLayout->setStretch(0, 45);   // 热像仪画面列 (图 + 目标信息)
        ui->videoFrameLayout->setStretch(1, 55);   // 可见光画面
    }

    // ---- 目标信息: 2x2 网格内标签允许随列宽收缩(避免把热像仪画面列顶宽) ----
    for (QLabel *lbl : { ui->labelDeltaX, ui->labelDeltaY,
                         ui->labelDistance, ui->labelMaxTemp }) {
        lbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        lbl->setMinimumWidth(0);
    }
    // 控制面板改为 Tab 页后需要更宽的可视区域, 避免 4 个标签被挤压
    ui->controlPanel->setMinimumWidth(340);
    ui->controlPanel->setMaximumWidth(400);

    // 固定初始尺寸, 避免布局反复自增
    resize(1360, 820);
    setMinimumSize(1180, 700);

    // 未连接时"断开"按钮不可用
    ui->btnDisconnectThermal->setEnabled(false);
    ui->btnDisconnectVisible->setEnabled(false);
    ui->btnDisconnectLaser->setEnabled(false);

    // 科技风深色主题
    applyTechStyle();

    // ---- 目标信息高度: 在现有基础上再增加一倍(累计为原始自然高度的 2 倍) ----
    // 仍以"原始样式下的自然高度"为基准换算, 保证与前一版的高度口径一致
    ui->thermalInfoBox->ensurePolished();
    const int infoNaturalH = ui->thermalInfoBox->sizeHint().height();   // 原始自然高度(11pt 字号)
    const int infoPrevH    = qMax(28, infoNaturalH / 2);                // 最初"缩减一半"后的高度
    const int infoTargetH  = infoPrevH * 4;                             // 本版要求: 在上一版基础上再翻一倍
    {
        // 字号保持 .ui 原始大小(11pt 粗体), 内容填满加高后的区域
        if (ui->thermalInfoLayout) {
            ui->thermalInfoLayout->setContentsMargins(4, 2, 4, 2);
            ui->thermalInfoLayout->setVerticalSpacing(2);
            ui->thermalInfoLayout->setHorizontalSpacing(8);
        }
        ui->thermalInfoBox->setStyleSheet(
            "QGroupBox { margin-top: 12px; padding: 4px 6px 4px 6px; border-radius: 6px; }");
        ui->thermalInfoBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        ui->thermalInfoBox->setFixedHeight(infoTargetH);
    }
    LOG_INFO(QString("目标信息高度已增加一倍: %1px -> %2px").arg(infoPrevH).arg(infoTargetH));

    // 两路画面左上角标题(热源监测图像 / 可见光监测图像)
    setupVideoTitles();

    // 串口列表 + 点击下拉框自动刷新; 可见光画面支持滚轮缩放/拖拽
    refreshSerialPorts();
    ui->comboSerialPort->installEventFilter(this);
    ui->labelVisible->installEventFilter(this);
    ui->labelVisible->setMouseTracking(true);
    ui->labelVisible->setCursor(Qt::OpenHandCursor);

    // 热像仪画面: 同样支持滚轮缩放/左键拖拽/双击复位, 并支持鼠标温度探针
    ui->labelThermal->installEventFilter(this);
    ui->labelThermal->setMouseTracking(true);
    ui->labelThermal->setCursor(Qt::OpenHandCursor);

    // 其余滑条范围与初值(变焦/光圈滑条已按需求移除, 改为按钮步进 + 数值显示)
    ui->sliderBrightness->setRange(0, 100);
    ui->sliderLaserZoom->setRange(0, 100);
    ui->sliderExposure->setRange(0, 100);
    ui->sliderGain->setRange(0, 100);
    ui->sliderBrightness->blockSignals(true);
    ui->sliderBrightness->setValue(cfg.defaultBrightness());
    ui->sliderBrightness->blockSignals(false);
    ui->labelBrightness->setText(QString("亮度: %1").arg(cfg.defaultBrightness()));
    ui->sliderLaserZoom->blockSignals(true);
    ui->sliderLaserZoom->setValue(cfg.defaultZoom());
    ui->sliderLaserZoom->blockSignals(false);
    ui->labelLaserZoom->setText(QString("光斑大小: %1").arg(cfg.defaultZoom()));

    // 曝光/增益滑条: 仅设置初始位置并同步数值显示, 屏蔽信号以免启动时就切到手动曝光
    const int initExposure = 50;
    const int initGain = 60;    // 相机当前 GainLevel=60

    ui->sliderExposure->blockSignals(true);
    ui->sliderExposure->setValue(initExposure);
    ui->sliderExposure->blockSignals(false);
    ui->labelExposure->setText(QString("曝光: %1").arg(initExposure));

    ui->sliderGain->blockSignals(true);
    ui->sliderGain->setValue(initGain);
    ui->sliderGain->blockSignals(false);
    ui->labelGain->setText(QString("增益 Gain: %1").arg(initGain));

    // ---- 变焦位置实时回读(1Hz): 让"变焦 ±"旁持续显示相机当前实际倍率 ----
    m_zoomMonitorTimer = new QTimer(this);
    m_zoomMonitorTimer->setInterval(1000);
    connect(m_zoomMonitorTimer, &QTimer::timeout, this, [this]() {
        // 步进过程中由闭环自身回读, 避免并发读抖动
        if (m_visibleConnected && !m_visibleCamera->isZoomBusy()) {
            m_visibleCamera->queryZoomPosition();
        }
    });
    m_zoomMonitorTimer->start();

    updateStatusBar();
    updateResolutionLabels();
    LOG_INFO("MainWindow initialized");

    // 启动后自动连接三台设备(延迟 800ms 让界面先完成显示)
    QTimer::singleShot(800, this, &MainWindow::autoConnectDevices);
}

MainWindow::~MainWindow()
{
    // ---- 退出顺序很关键 ----
    // ui 由 delete ui 显式销毁, 而各模块对象(m_irLaser 等)是本窗口的 QObject 子对象,
    // 它们会在 ~MainWindow 函数体执行完之后才被 ~QObject 删除。若此时它们再发信号
    // (例如 IRLaser::~IRLaser -> close() -> emit connectionChanged), 回调会访问已被
    // 销毁的 ui 控件(ui->labelLaserStatus), 造成退出时 SIGSEGV(use-after-free)。
    // 处理: 先停掉后台活动, 并断开各模块 -> 本窗口的所有信号, 再销毁 ui。
    for (QObject *module : { static_cast<QObject *>(m_thermalCamera),
                             static_cast<QObject *>(m_visibleCamera),
                             static_cast<QObject *>(m_irLaser),
                             static_cast<QObject *>(m_heatDetector),
                             static_cast<QObject *>(m_angleCalculator),
                             static_cast<QObject *>(m_tcpServer) }) {
        if (module) module->disconnect(this);
    }

    m_detectionTimer->stop();
    m_thermalCamera->stop();
    m_visibleCamera->stop();
    m_tcpServer->stop();
    m_irLaser->close();

    delete ui;
    ui = nullptr;
    // 置空指向 ui 子控件的指针, 避免析构后期(日志/分辨率回调)写入已释放内存
    m_tcpLogView = nullptr;
    m_controlTabs = nullptr;
    m_comboStream = nullptr;
    m_titleThermal = nullptr;
    m_titleVisible = nullptr;
    m_labelDetectStatus = nullptr;
    m_labelTcpAddress = nullptr;
    m_labelThermalRes = nullptr;
    m_labelVisibleRes = nullptr;
    m_labelStatusThermal = nullptr;
    m_labelStatusVisible = nullptr;
    m_labelStatusLaser   = nullptr;
    m_labelStatusTcp     = nullptr;
    m_labelThermalConn   = nullptr;
    m_labelVisibleConn   = nullptr;
    m_btnLaserRefresh = nullptr;
    m_comboDetectMode = nullptr;
    m_spinMinTemp = nullptr;
    m_spinMinArea = nullptr;
    m_checkGrid = nullptr;
    m_labelZoomValue = nullptr;
    m_labelIrisValue = nullptr;
    m_btnRecordStart = nullptr;
    m_btnRecordStop = nullptr;
    m_btnSelectSavePath = nullptr;
    m_labelSavePath = nullptr;
    m_labelProbe = nullptr;
    m_zoomMonitorTimer = nullptr;
    m_comboIrcut = nullptr;
    m_imageCombos.clear();
    m_imageBlocks.clear();
    m_imageTags.clear();
    m_tcpExampleView = nullptr;
    m_btnLaserSelfTest = nullptr;
    m_labelLaserComms = nullptr;
    m_labelLaserAngle = nullptr;
    m_labelLaserBri = nullptr;
    m_labelLaserSwitch = nullptr;
    m_labelPreviewInfo = nullptr;
    m_checkPreviewNative = nullptr;
}

void MainWindow::setupUi_connections()
{
    // Thermal camera signals
    connect(m_thermalCamera, &ThermalCamera::frameReady,
            this, &MainWindow::onThermalFrameReady);
    connect(m_thermalCamera, &ThermalCamera::connectionStatusChanged,
            this, &MainWindow::onThermalConnectionChanged);
    connect(m_thermalCamera, &ThermalCamera::fpsUpdated,
            this, &MainWindow::onThermalFpsUpdated);

    // Visible camera signals
    connect(m_visibleCamera, &VisibleCamera::frameReady,
            this, &MainWindow::onVisibleFrameReady);
    connect(m_visibleCamera, &VisibleCamera::connectionStatusChanged,
            this, &MainWindow::onVisibleConnectionChanged);
    connect(m_visibleCamera, &VisibleCamera::fpsUpdated,
            this, &MainWindow::onVisibleFpsUpdated);
    connect(m_visibleCamera, &VisibleCamera::decoderChanged,
            this, &MainWindow::onDecoderChanged);   // 显示实际生效的解码链路(硬解/软解)

    // Heat detection signals
    connect(m_heatDetector, &HeatSourceDetector::heatSourcesDetected,
            this, &MainWindow::onHeatSourcesDetected);

    // Angle calculator signals
    connect(m_angleCalculator, &AngleCalculator::angleCalculated,
            this, &MainWindow::onAngleCalculated);

    // Visible camera zoom feedback (变焦按钮旁显示实际倍率)
    connect(m_visibleCamera, &VisibleCamera::zoomPositionChanged,
            this, &MainWindow::onZoomPositionChanged);

    // 相机图像参数回读 -> 同步"图像参数"下拉
    connect(m_visibleCamera, &VisibleCamera::imageConfigReceived,
            this, &MainWindow::syncImageParamsUi);

    // ---- 相机校时结果(每次连接后自动与本地系统时间校准一次) ----
    connect(m_visibleCamera, &VisibleCamera::timeSyncFinished, this,
            [](bool ok, const QString &info) {
                LOG_INFO(QString("可见光相机校时: %1 - %2")
                             .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败"), info));
            });
    connect(m_thermalCamera, &ThermalCamera::timeSyncFinished, this,
            [](bool ok, const QString &info) {
                LOG_INFO(QString("热像仪校时: %1 - %2")
                             .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败"), info));
            });

    // ---- 录像取证状态 ----
    connect(m_visibleCamera, &VisibleCamera::recordingStarted, this, [this](const QString &path) {
        LOG_INFO(QString("录像取证: 已开始 -> %1").arg(path));
        setRecordingUi(true);
        if (m_labelSavePath) m_labelSavePath->setText(QString("正在录像: %1").arg(path));
    });
    connect(m_visibleCamera, &VisibleCamera::recordingStopped, this, [this](const QString &path) {
        LOG_INFO(QString("录像取证: 已结束, 文件 -> %1").arg(path));
        setRecordingUi(false);
        if (m_labelSavePath) m_labelSavePath->setText(QString("最近录像: %1").arg(path));
    });
    connect(m_visibleCamera, &VisibleCamera::recordingFailed, this, [this](const QString &err) {
        LOG_ERROR(QString("录像取证失败: %1").arg(err));
        setRecordingUi(false);
        QMessageBox::warning(this, "录像取证", err);
    });

    // Laser status (同时控制连接/断开按钮可用性)
    connect(m_irLaser, &IRLaser::connectionChanged, this, [this](bool ok) {
        ui->labelLaserStatus->setText(ok ? "已连接" : "未连接");
        ui->labelLaserStatus->setStyleSheet(ok ? "color: #22c55e; font-weight: bold;"
                                               : "color: #ef4444; font-weight: bold;");
        ui->btnConnectLaser->setEnabled(!ok);
        ui->btnDisconnectLaser->setEnabled(ok);
        // 激光器开关状态: 断开时无回读, 显示"未知"; 连接后等待周期回读刷新
        if (m_labelLaserSwitch) {
            m_labelLaserSwitch->setText(ok ? QStringLiteral("激光: 回读中…")
                                           : QStringLiteral("激光: 未知"));
            m_labelLaserSwitch->setStyleSheet("color: #9fc4e8; font-weight: bold;");
        }
        updateStatusBar();                           // 同步状态栏"红外激光"连接状态
        if (ok) {
            m_irLaser->queryStatus();               // 连接后立即回读一次设备状态
            if (m_laserStatusTimer) m_laserStatusTimer->start();
        } else if (m_laserStatusTimer) {
            m_laserStatusTimer->stop();
        }
    });

    // ---- 红外激光状态回读(应答解析结果) ----
    connect(m_irLaser, &IRLaser::brightnessValueChanged, this, [this](int raw) {
        m_laserBrightnessRaw = raw;
        if (m_labelLaserBri) {
            m_labelLaserBri->setText(QString("亮度 %1/255").arg(raw));
        }
    });
    // 行程位置仅记录日志(界面不再单独显示行程状态; 光斑大小统一以"光斑角度"表示)
    connect(m_irLaser, &IRLaser::positionValueChanged, this, [](int raw) {
        LOG_INFO(QString("激光器出光角度行程位置: 0x%1 (%2/16384)")
                     .arg(raw, 4, 16, QChar('0')).arg(raw));
    });
    connect(m_irLaser, &IRLaser::angleValueChanged, this, [this](double deg) {
        if (m_labelLaserAngle) {
            m_labelLaserAngle->setText(QString("光斑角度 %1°").arg(deg, 0, 'f', 2));
        }
        LOG_INFO(QString("激光器当前光斑角度(出光角度): %1°").arg(deg, 0, 'f', 2));
    });

    // 收到任一有效应答帧: 解除"无应答"提示
    connect(m_irLaser, &IRLaser::rawFrameReceived, this, [this](const QString &hex) {
        m_laserGotReply = true;
        if (m_labelLaserComms) {
            m_labelLaserComms->setText(QStringLiteral("通信: 正常(已收到应答)"));
            m_labelLaserComms->setStyleSheet("color: #22c55e; font-weight: bold;");
        }
        LOG_DEBUG(QString("激光应答帧: %1").arg(hex));
    });
    connect(m_irLaser, &IRLaser::laserSwitchChanged, this, [this](bool on) {
        LOG_INFO(QString("激光器开关状态回读: %1").arg(on ? "开启" : "关闭"));
        // "开启激光/关闭激光"按钮之后显示当前实际开关状态(以设备应答为准)
        if (m_labelLaserSwitch) {
            m_labelLaserSwitch->setText(on ? QStringLiteral("激光: 开启")
                                           : QStringLiteral("激光: 关闭"));
            m_labelLaserSwitch->setStyleSheet(on ? "color: #22c55e; font-weight: bold;"
                                                 : "color: #9fc4e8; font-weight: bold;");
        }
    });
    connect(m_irLaser, &IRLaser::fanChanged, this, [](bool on) {
        LOG_INFO(QString("激光器风扇状态回读: %1").arg(on ? "开启" : "关闭"));
    });

    // TCP server signals
    connect(m_tcpServer, &TcpServer::clientConnected,
            this, &MainWindow::onTcpClientConnected);
    connect(m_tcpServer, &TcpServer::clientDisconnected,
            this, &MainWindow::onTcpClientDisconnected);
    connect(m_tcpServer, &TcpServer::dataReceived,
            this, &MainWindow::onTcpDataReceived);

    // TCP server angle forwarding
    connect(m_angleCalculator, &AngleCalculator::angleCalculated,
            m_tcpServer, &TcpServer::onAngleCalculated);

    LOG_INFO("All UI signal connections established");
}

// ============================================================
// Camera slots
// ============================================================
void MainWindow::onThermalFrameReady(const QImage &frame)
{
    QMutexLocker locker(&m_frameMutex);
    m_lastThermalFrame = frame;
    locker.unlock();

    // 叠加重绘节流: 限制到 ~10fps(与检测定时器同频), 避免叠加绘制占满界面线程
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastThermalRenderMs >= 100) {
        m_lastThermalRenderMs = now;
        renderThermal();
    }
    updateResolutionLabels();
}

void MainWindow::onVisibleFrameReady(const QImage &frame)
{
    QMutexLocker locker(&m_frameMutex);
    m_lastVisibleFrame = frame;
    locker.unlock();

    // 缩放已由采集端在 GPU 内完成(输出即显示尺寸), 界面渲染开销很小,
    // 这里按 40ms(~25fps, 与相机源同频)节流, 兼顾流畅度与 CPU 占用。
    // 采集端的背压机制会把多余的帧丢弃, 因此这里限速不会造成内存堆积。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastVisibleRenderMs >= 40) {
        m_lastVisibleRenderMs = now;
        renderVisible();
    }
    updateResolutionLabels();
}

void MainWindow::onThermalConnectionChanged(bool connected)
{
    m_thermalConnected = connected;
    // 连接状态与帧率统一在状态栏显示, 顶部按钮区不再重复显示
    // 已连接后禁止再次点击"连接热像仪", 连接失败时恢复可点击
    ui->btnConnectThermal->setEnabled(!connected);
    ui->btnDisconnectThermal->setEnabled(connected);
    updateStatusBar();
    LOG_INFO(QString("Thermal camera connection status: %1").arg(connected ? "CONNECTED" : "DISCONNECTED"));
    // ---- 每次连接成功后自动与本地系统时间校准一次 ----
    // 热像仪为海康设备, 走 ISAPI(HTTP) 校时, 与 RTSP 视频链路相互独立
    if (connected) {
        QTimer::singleShot(1200, this, [this]() {
            if (m_thermalConnected) m_thermalCamera->syncSystemTime();
        });
    }
}

void MainWindow::onVisibleConnectionChanged(bool connected)
{
    m_visibleConnected = connected;
    // 连接状态与帧率统一在状态栏显示, 顶部按钮区不再重复显示
    // 已连接后禁止再次点击"连接可见光", 连接失败时恢复可点击
    ui->btnConnectVisible->setEnabled(!connected);
    ui->btnDisconnectVisible->setEnabled(connected);
    updateStatusBar();
    if (connected) {
        // 连接建立后先下发一次停止, 清除设备可能遗留的云台/变焦运动
        // (例如此前的"一键巡航"或未收尾的闭环变焦), 避免出现"自动变焦"现象。
        m_visibleCamera->ptzStop();
        m_visibleCamera->zoomStop();
        // 连接稳定后回读相机图像参数, 同步"图像参数"下拉显示
        QTimer::singleShot(2000, this, [this]() { m_visibleCamera->queryImageParams(); });
        // 延迟回读一次变焦位置, 让"变焦 ±"按钮旁的倍率显示有初值
        QTimer::singleShot(1500, this, [this]() { m_visibleCamera->queryZoomPosition(); });
        updateIrisValueLabel();
        // ---- 每次连接成功后自动与本地系统时间校准一次 ----
        // 延迟 1.2s 等 RTSP/ISAPI 链路稳定, 避免刚连上就发 HTTP 请求失败
        QTimer::singleShot(1200, this, [this]() {
            if (m_visibleConnected) m_visibleCamera->syncSystemTime();
        });
    }
    LOG_INFO(QString("Visible camera connection status: %1").arg(connected ? "CONNECTED" : "DISCONNECTED"));
}

void MainWindow::onThermalFpsUpdated(double fps)
{
    m_thermalFps = fps;
    updateStatusBar();
}

void MainWindow::onVisibleFpsUpdated(double fps)
{
    m_visibleFps = fps;
    updateStatusBar();
}

// ============================================================
// 热像仪画面叠加绘制: 网格 / 中心十字与X-Y方向 / 标记框 / 右上角状态
// ============================================================
void MainWindow::renderThermal()
{
    QMutexLocker locker(&m_frameMutex);
    if (m_lastThermalFrame.isNull()) return;
    QImage img = m_lastThermalFrame.copy();
    locker.unlock();

    QPainter p(&img);
    const int w = img.width();
    const int h = img.height();
    const int cx = w / 2;
    const int cy = h / 2;

    // ---- 1. 网格线 (可开关) ----
    if (m_gridEnabled) {
        const int cols = 8, rows = 6;
        p.setPen(QPen(QColor(0, 255, 0, 80), 1, Qt::DotLine));
        for (int i = 1; i < cols; i++) p.drawLine(w * i / cols, 0, w * i / cols, h);
        for (int j = 1; j < rows; j++) p.drawLine(0, h * j / rows, w, h * j / rows);
        p.setPen(QColor(0, 255, 0, 150));
        QFont gf = p.font(); gf.setPointSize(7); p.setFont(gf);
        for (int i = 1; i < cols; i++)
            p.drawText(w * i / cols + 2, 11, QString::number(i * w / cols));
        for (int j = 1; j < rows; j++)
            p.drawText(2, h * j / rows - 3, QString::number(j * h / rows));
    }

    // ---- 2. 以图像中心为原点的十字与 X/Y 方向标识 ----
    p.setPen(QPen(Qt::green, 1, Qt::DashLine));
    p.drawLine(cx - 25, cy, cx + 25, cy);
    p.drawLine(cx, cy - 25, cx, cy + 25);
    p.setPen(QPen(Qt::green, 1));
    QFont af = p.font(); af.setPointSize(8); af.setBold(true); p.setFont(af);
    p.drawText(cx + 28, cy - 4, "X+");
    p.drawText(cx - 40, cy - 4, "X-");
    p.drawText(cx + 4, cy - 28, "Y+");
    p.drawText(cx + 4, cy + 36, "Y-");

    // ---- 3. 所有候选热源: 细黄框 ----
    p.setPen(QPen(QColor(255, 200, 0), 1));
    for (const auto &s : m_lastSources) {
        p.drawRect(s.boundingBoxX, s.boundingBoxY, s.boundingBoxW, s.boundingBoxH);
    }

    // ---- 4. 选中目标: 粗红框 + 偏差连线 + 参数标注 ----
    if (m_targetFound) {
        const HeatSource &t = m_target;
        p.setPen(QPen(Qt::red, 2));
        p.drawRect(t.boundingBoxX, t.boundingBoxY, t.boundingBoxW, t.boundingBoxH);
        p.drawLine(t.centerX - 8, t.centerY, t.centerX + 8, t.centerY);
        p.drawLine(t.centerX, t.centerY - 8, t.centerX, t.centerY + 8);

        if (t.centerX != cx || t.centerY != cy) {
            p.setPen(QPen(Qt::yellow, 1));
            p.drawLine(cx, cy, t.centerX, t.centerY);
        }

        const int dxPix = t.centerX - cx;   // 正=偏右, 负=偏左
        const int dyPix = t.centerY - cy;   // 正=偏下, 负=偏上
        const QString info = QString("T=%1°C  A=%2px  dX=%3px  dY=%4px")
                                 .arg(t.maxTemp, 0, 'f', 1).arg(t.area)
                                 .arg(dxPix).arg(dyPix);
        QFont tf = p.font(); tf.setPointSize(8); tf.setBold(true); p.setFont(tf);
        const QRect infoRect(qMax(0, t.boundingBoxX), qMax(0, t.boundingBoxY - 16), 280, 15);
        p.fillRect(infoRect, QColor(0, 0, 0, 160));
        p.setPen(Qt::white);
        p.drawText(infoRect.adjusted(3, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, info);
    }

    // ---- 5. 右上角: 检测状态提示 ----
    const QString statusText = m_targetFound ? QString("检测到热源目标")
                                             : QString("未检测到热源目标");
    QFont sf = p.font(); sf.setPointSize(10); sf.setBold(true); p.setFont(sf);
    const QFontMetrics fm(sf);
    const int tw = fm.width(statusText) + 14;
    const QRect stRect(w - tw - 6, 6, tw, fm.height() + 8);
    p.fillRect(stRect, m_targetFound ? QColor(0, 130, 0, 190) : QColor(130, 0, 0, 190));
    p.setPen(Qt::white);
    p.drawText(stRect, Qt::AlignCenter, statusText);
    p.end();

    // 缩放到标签大小后叠加显示, 支持滚轮缩放/左键拖拽(与可见光画面一致)。
    // 先在 QImage 侧缩放再转 QPixmap, 避免把整帧上传到 X 服务器(每帧多传约 1MB)。
    const QSize target = ui->labelThermal->size();
    if (target.isEmpty()) return;

    const QSize base = img.size().scaled(target, Qt::KeepAspectRatio);
    const QSize scaled(qMax(1, static_cast<int>(base.width() * m_thermalScale)),
                       qMax(1, static_cast<int>(base.height() * m_thermalScale)));
    const QImage scaledImg = img.scaled(scaled, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QPixmap canvas(target);
    canvas.fill(Qt::black);
    {
        QPainter cp(&canvas);
        const int ox = (target.width() - scaledImg.width()) / 2 + m_thermalOffset.x();
        const int oy = (target.height() - scaledImg.height()) / 2 + m_thermalOffset.y();
        cp.drawImage(ox, oy, scaledImg);

        // ---- 鼠标探针: 十字标记 + 坐标/灰度/温度 (两行, 避免挡住画面) ----
        if (m_hasProbe) {
            const int px = m_probeWidgetPos.x();
            const int py = m_probeWidgetPos.y();
            cp.setPen(QPen(QColor(255, 255, 0), 1));
            cp.drawLine(px - 10, py, px + 10, py);
            cp.drawLine(px, py - 10, px, py + 10);

            const QString line1 = QString("(%1,%2)").arg(m_probeImgX).arg(m_probeImgY);
            const QString line2 = (m_probeGray < 0)
                                      ? QString("灰度 --   温度 --")
                                      : QString("灰度 %1   温度 %2°C")
                                            .arg(m_probeGray).arg(m_probeTemp, 0, 'f', 1);
            QFont pf = cp.font(); pf.setPointSize(8); pf.setBold(true); cp.setFont(pf);
            const QFontMetrics fm(pf);
            const int boxW = qMax(fm.width(line1), fm.width(line2)) + 10;
            const int boxH = fm.height() * 2 + 6;
            const QRect r(px + 12, py - boxH - 6, boxW, boxH);
            cp.fillRect(r, QColor(0, 0, 0, 180));
            cp.setPen(QColor(255, 255, 0));
            cp.drawText(QRect(r.x() + 4, r.y() + 2, boxW - 8, fm.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, line1);
            cp.drawText(QRect(r.x() + 4, r.y() + 2 + fm.height(), boxW - 8, fm.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, line2);
        }

        // 左下角操作提示
        cp.setPen(QColor(160, 190, 220));
        QFont hf = cp.font(); hf.setPointSize(8); cp.setFont(hf);
        cp.drawText(8, target.height() - 8,
                    QString("缩放 %1×  (滚轮缩放 / 左键拖拽 / 双击复位 / 鼠标移动查看坐标·灰度·温度)")
                        .arg(m_thermalScale, 0, 'f', 2));
    }

    ui->labelThermal->setPixmap(canvas);
}

// ============================================================
// 可见光画面渲染: 支持鼠标滚轮缩放与左键拖拽平移
// ============================================================
void MainWindow::renderVisible()
{
    // 4K 主码流与其它码流一样, 直接在本程序界面内渲染(不再借助外部窗口):
    // 采集端已用 NVDEC 硬解, 并在 GPU(VIC) 内把 4K 缩放到显示区尺寸(见 applyStreamProfile),
    // 因此这里的界面缩放几乎为零开销, 界面线程不会被 4K 贴图拖累。

    QMutexLocker locker(&m_frameMutex);
    if (m_lastVisibleFrame.isNull()) return;
    const QImage img = m_lastVisibleFrame;
    locker.unlock();

    const QSize target = ui->labelVisible->size();
    if (target.isEmpty()) return;

    // 基础缩放(适应窗口) × 用户缩放系数
    const QSize base = img.size().scaled(target, Qt::KeepAspectRatio);
    const QSize scaled(qMax(1, static_cast<int>(base.width() * m_visibleScale)),
                       qMax(1, static_cast<int>(base.height() * m_visibleScale)));

    // ---- 分级渲染策略 (2026-09-22 "4K 零延时"改造) ----
    // 采集端已在 GPU(VIC) 内把 4K 缩放到"显示区尺寸"(见 applyStreamProfile/previewOutputSize),
    // 因此绝大多数情况下这里"零缩放", 界面线程开销趋近于 0, 不再成为采集端的背压来源。
    //   1) 无缩放/无平移 且 源图尺寸≈目标尺寸 -> 直接贴图(零缩放, 最快)
    //   2) 无缩放/无平移 但尺寸有差           -> 单次 FastTransformation(便宜)
    //   3) 用户滚轮缩放/拖拽(低频交互)        -> 折半预缩 + 平滑缩放(保画质)
    // 原实现在界面线程对 3840x2160 做多级+平滑缩放(单帧 60~150ms), 是"延时持续增大"的根因。
    const bool plain = qFuzzyCompare(m_visibleScale, 1.0) && m_visibleOffset.isNull();
    QImage scaledImg;
    if (plain && qAbs(img.width() - scaled.width()) <= 2 &&
                 qAbs(img.height() - scaled.height()) <= 2) {
        scaledImg = img;   // 零缩放快路径: 与显示区 1:1, 直接使用
    } else if (plain) {
        scaledImg = img.scaled(scaled, Qt::KeepAspectRatio, Qt::FastTransformation);
    } else {
        QImage src = img;
        while (src.width() > scaled.width() * 2 && src.width() > 640) {
            src = src.scaled(src.width() / 2, src.height() / 2,
                             Qt::KeepAspectRatio, Qt::FastTransformation);
        }
        scaledImg = src.scaled(scaled, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QPixmap canvas(target);
    canvas.fill(Qt::black);
    {
        QPainter p(&canvas);
        const int x = (target.width() - scaledImg.width()) / 2 + m_visibleOffset.x();
        const int y = (target.height() - scaledImg.height()) / 2 + m_visibleOffset.y();
        p.drawImage(x, y, scaledImg);

        // 左下角显示当前缩放倍率与操作提示
        p.setPen(QColor(255, 255, 0));
        QFont hf = p.font(); hf.setPointSize(8); p.setFont(hf);
        p.drawText(8, target.height() - 8,
                   QString("缩放 %1×  (滚轮缩放 / 左键拖拽 / 双击复位)")
                       .arg(m_visibleScale, 0, 'f', 2));
    }

    ui->labelVisible->setPixmap(canvas);
}

// ============================================================
// 状态栏: 两台相机的分辨率
// ============================================================
void MainWindow::updateResolutionLabels()
{
    QMutexLocker locker(&m_frameMutex);
    const QSize thSize = m_lastThermalFrame.size();
    const QSize viSize = m_lastVisibleFrame.size();
    locker.unlock();

    // 分辨率只在变化时刷新, 避免每帧触发 setText/重绘
    if (m_labelThermalRes && thSize != m_lastThermalResSize) {
        m_lastThermalResSize = thSize;
        m_labelThermalRes->setText(thSize.isEmpty() ? "热像仪: --"
                                                    : QString("热像仪: %1x%2")
                                                          .arg(thSize.width()).arg(thSize.height()));
    }
    if (m_labelVisibleRes && viSize != m_lastVisibleResSize) {
        m_lastVisibleResSize = viSize;
        m_labelVisibleRes->setText(viSize.isEmpty() ? "可见光: --"
                                                    : QString("可见光: %1x%2")
                                                          .arg(viSize.width()).arg(viSize.height()));
    }
}

// ============================================================
// 串口列表刷新 / 本机服务IP / 自动连接 / 日志追加 / 事件过滤
// ============================================================
void MainWindow::refreshSerialPorts()
{
    const QString current = ui->comboSerialPort->currentText();
    const auto ports = QSerialPortInfo::availablePorts();

    ui->comboSerialPort->blockSignals(true);
    ui->comboSerialPort->clear();

    // 稳定设备名优先: udev 规则生成的 /dev/ir_laser (FTDI 红外激光器)
    if (QFile::exists("/dev/ir_laser")) {
        ui->comboSerialPort->addItem("ir_laser");
    }
    for (const auto &port : ports) {
        if (port.portName() != "ir_laser") {
            ui->comboSerialPort->addItem(port.portName());
        }
        LOG_DEBUG(QString("Serial port: %1 (%2) [%3]")
                      .arg(port.portName(), port.description(), port.systemLocation()));
    }

    int idx = ui->comboSerialPort->findText(current);
    if (idx < 0) idx = ui->comboSerialPort->findText("ir_laser");
    if (idx < 0 && ui->comboSerialPort->count() > 0) idx = 0;
    if (idx >= 0) ui->comboSerialPort->setCurrentIndex(idx);
    ui->comboSerialPort->blockSignals(false);

    LOG_INFO(QString("串口列表已刷新: %1 个端口, 当前选择 %2")
                 .arg(ui->comboSerialPort->count())
                 .arg(ui->comboSerialPort->currentText()));
}

QString MainWindow::localIpString() const
{
    QStringList candidates;
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        if (!(ni.flags() & QNetworkInterface::IsUp)) continue;
        if (ni.flags() & QNetworkInterface::IsLoopBack) continue;
        for (const QNetworkAddressEntry &e : ni.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            if (e.ip().toString().startsWith("169.254.")) continue;
            candidates << e.ip().toString();
        }
    }
    // 优先返回主网络地址, 便于外部客户端连接
    for (const QString &ip : candidates) {
        if (ip.startsWith("192.168.10.")) return ip;
    }
    return candidates.isEmpty() ? QString("-") : candidates.first();
}

void MainWindow::autoConnectDevices()
{
    LOG_INFO("=== 启动自动连接: 可见光 / 热像仪 / 红外激光 ===");
    // 1) 红外激光(串口)
    on_btnConnectLaser_clicked();
    // 2) 可见光相机(RTSP)
    on_btnConnectVisible_clicked();
    // 3) 热像仪(RTSP) + 启动热源检测定时器
    on_btnConnectThermal_clicked();
}

// TCP 客户端交互日志: 只显示最新的交互信息, 追加到 TCP 服务器分组内的文本框
void MainWindow::appendTcpLog(const QString &line)
{
    if (!m_tcpLogView) return;
    m_tcpLogView->appendPlainText(QString("[%1] %2")
                                      .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
                                      .arg(line));
    m_tcpLogView->verticalScrollBar()->setValue(m_tcpLogView->verticalScrollBar()->maximum());
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // 画面控件尺寸变化: 左上角标题跟随重新定位
    if ((watched == ui->labelThermal || watched == ui->labelVisible) &&
        event->type() == QEvent::Resize) {
        repositionVideoTitles();
    }

    // ---- 热像仪画面: 滚轮缩放 / 左键拖拽 / 双击复位 / 鼠标移动显示坐标与温度 ----
    if (watched == ui->labelThermal) {
        if (event->type() == QEvent::Wheel) {
            QWheelEvent *we = static_cast<QWheelEvent *>(event);
            const double step = (we->angleDelta().y() > 0) ? 1.1 : (1.0 / 1.1);
            m_thermalScale = qBound(0.2, m_thermalScale * step, 8.0);
            renderThermal();
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            m_thermalScale = 1.0;
            m_thermalOffset = QPoint(0, 0);
            renderThermal();
            LOG_INFO("热像仪画面缩放已复位");
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_thermalDragging = true;
                m_thermalDragStart = me->pos();
                ui->labelThermal->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            if (m_thermalDragging) {
                m_thermalOffset += (me->pos() - m_thermalDragStart);
                m_thermalDragStart = me->pos();
                renderThermal();
            } else {
                updateThermalProbe(me->pos().x(), me->pos().y());
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            m_thermalDragging = false;
            ui->labelThermal->setCursor(Qt::OpenHandCursor);
            return true;
        }
        if (event->type() == QEvent::Leave) {
            m_hasProbe = false;
            m_probeGray = -1;
            m_probeTemp = 0.0;
            if (m_labelProbe) m_labelProbe->setText(QStringLiteral("坐标: --   灰度: --   温度: --"));
            renderThermal();
            return true;
        }
    }

    // 串口下拉框: 鼠标点击时自动刷新可用端口
    if (watched == ui->comboSerialPort && event->type() == QEvent::MouseButtonPress) {
        refreshSerialPorts();
    }
    // 可见光画面: 滚轮缩放 / 左键拖拽 / 双击复位
    else if (watched == ui->labelVisible) {
        if (event->type() == QEvent::Wheel) {
            QWheelEvent *we = static_cast<QWheelEvent *>(event);
            const double step = (we->angleDelta().y() > 0) ? 1.1 : (1.0 / 1.1);
            m_visibleScale = qBound(0.2, m_visibleScale * step, 8.0);
            renderVisible();
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            m_visibleScale = 1.0;
            m_visibleOffset = QPoint(0, 0);
            renderVisible();
            LOG_INFO("可见光画面缩放已复位");
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_visibleDragging = true;
                m_visibleDragStart = me->pos();
                ui->labelVisible->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && m_visibleDragging) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            m_visibleOffset += (me->pos() - m_visibleDragStart);
            m_visibleDragStart = me->pos();
            renderVisible();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            m_visibleDragging = false;
            ui->labelVisible->setCursor(Qt::OpenHandCursor);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ============================================================
// Detection & angle
// ============================================================
void MainWindow::onHeatSourcesDetected(const QVector<HeatSource> &sources)
{
    m_lastSources = sources;

    if (sources.isEmpty()) {
        m_targetFound = false;
        m_target = {};
        AngleDifference angle = m_angleCalculator->calculateInvalid();
        updateAngleDisplay(angle);
        ui->labelMaxTemp->setText("温度: --");
        if (m_labelDetectStatus) {
            m_labelDetectStatus->setText("检测状态: 未检测到热源目标");
            m_labelDetectStatus->setStyleSheet("color: red; font-weight: bold;");
        }
        renderThermal();
        return;
    }

    // 目标选择:
    //   自动模式 -> 亮度(最亮) 与 面积(最大) 归一化加权评分, 得分最高者为目标
    //   手动模式 -> 已由检测器按"最低温度/最小面积像素"筛选, 取其中面积最大者
    HeatSource target;
    if (m_heatDetector->selectMode() == HeatSourceDetector::Manual) {
        target = m_heatDetector->findHottest(sources);
        for (const auto &s : sources) {
            if (s.area > target.area) target = s;
        }
    } else {
        target = m_heatDetector->selectTarget(sources);
    }

    m_target = target;
    m_targetFound = true;

    // 目标信息节流: 位置变化>15px 或 距上次记录>1s 才写入日志, 避免 10Hz 刷屏
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool moved = (qAbs(target.centerX - m_lastLogTargetX) > 15 ||
                        qAbs(target.centerY - m_lastLogTargetY) > 15);
    if (moved || (nowMs - m_lastTargetLogMs) > 1000) {
        LOG_INFO(QString("热源目标已确认: 候选%1个, 选中 中心(%2,%3) 温度%4°C 面积%5px")
                     .arg(sources.size()).arg(target.centerX).arg(target.centerY)
                     .arg(target.maxTemp, 0, 'f', 1).arg(target.area));
        m_lastLogTargetX = target.centerX;
        m_lastLogTargetY = target.centerY;
        m_lastTargetLogMs = nowMs;
    }

    if (m_labelDetectStatus) {
        m_labelDetectStatus->setText("检测状态: 检测到热源目标");
        m_labelDetectStatus->setStyleSheet("color: green; font-weight: bold;");
    }

    // 标记框/网格/中心十字/右上角状态 统一由 renderThermal() 绘制
    renderThermal();

    AngleDifference angle = m_angleCalculator->calculate(
        target.centerX, target.centerY, target.maxTemp);
    updateAngleDisplay(angle);

    ui->labelMaxTemp->setText(QString("温度: %1°C (%2px)")
                                  .arg(target.maxTemp, 0, 'f', 1).arg(target.area));
}

void MainWindow::onAngleCalculated(const AngleDifference &angle)
{
    // This is also forwarded to TCP server via signal connection
    Q_UNUSED(angle);
}

void MainWindow::updateAngleDisplay(const AngleDifference &angle)
{
    if (angle.valid) {
        // 以图像中心为起点的像素偏差(正X=偏右, 正Y=偏下)
        int imgW = 640, imgH = 512;
        {
            QMutexLocker locker(&m_frameMutex);
            if (!m_lastThermalFrame.isNull()) {
                imgW = m_lastThermalFrame.width();
                imgH = m_lastThermalFrame.height();
            }
        }
        const int dxPix = angle.sourceX - imgW / 2;
        const int dyPix = angle.sourceY - imgH / 2;
        const QString dirX = (dxPix > 0) ? "右" : ((dxPix < 0) ? "左" : "居中");
        const QString dirY = (dyPix > 0) ? "下" : ((dyPix < 0) ? "上" : "居中");
        const int distPix = qRound(std::sqrt(static_cast<double>(dxPix * dxPix + dyPix * dyPix)));

        // 文案保持紧凑, 避免撑宽"目标信息"容器
        ui->labelDeltaX->setText(QString("ΔX: %1px %2 %3°")
                                     .arg(dxPix).arg(dirX).arg(angle.deltaX, 0, 'f', 1));
        ui->labelDeltaY->setText(QString("ΔY: %1px %2 %3°")
                                     .arg(dyPix).arg(dirY).arg(angle.deltaY, 0, 'f', 1));
        ui->labelDistance->setText(QString("距中心: %1px %2°")
                                       .arg(distPix).arg(angle.distance, 0, 'f', 1));

        // Color coding: green if close to center, red if far
        QString style = (angle.distance < 2.0) ? "color: green; font-weight: bold;" :
                        (angle.distance < 5.0) ? "color: orange; font-weight: bold;" :
                                                  "color: red; font-weight: bold;";
        ui->labelDeltaX->setStyleSheet(style);
        ui->labelDeltaY->setStyleSheet(style);
        ui->labelDistance->setStyleSheet(style);
    } else {
        ui->labelDeltaX->setText("ΔX: --");
        ui->labelDeltaY->setText("ΔY: --");
        ui->labelDistance->setText("距中心: --");
        ui->labelDeltaX->setStyleSheet("");
        ui->labelDeltaY->setStyleSheet("");
        ui->labelDistance->setStyleSheet("");
    }
}

void MainWindow::updateDetection()
{
    // This is called periodically to trigger detection on latest thermal frame
    // In a real system, temperature data would come from ISAPI
    // For now, we use the thermal frame image as a proxy
    QMutexLocker locker(&m_frameMutex);
    if (m_lastThermalFrame.isNull()) {
        return;   // 未连接热像仪时静默跳过(此处原本每 100ms 写一条日志, 会刷爆日志)
    }
    QImage thermalFrame = m_lastThermalFrame.copy();
    locker.unlock();

    // Convert to grayscale and create synthetic temperature data
    // In production, this would come from ISAPI thermal data endpoint
    const int w = thermalFrame.width();
    const int h = thermalFrame.height();
    const int need = w * h;

    // 复用温度缓冲, 避免每 100ms 一次 1.3MB 的 new/delete 抖动
    if (m_tempData.size() != need) {
        m_tempData.resize(need);
    }
    float *tempData = m_tempData.data();

    QImage gray = thermalFrame.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < h; y++) {
        const uchar *line = gray.scanLine(y);
        float *row = tempData + y * w;
        for (int x = 0; x < w; x++) {
            // Map pixel intensity to temperature range (-20 to 150°C)
            row[x] = -20.0f + (line[x] / 255.0f) * 170.0f;
        }
    }

    // Assume environment temperature ~25°C
    m_heatDetector->detect(tempData, w, h, 25.0f);
}

// ============================================================
// TCP
// ============================================================
void MainWindow::onTcpClientConnected(const QString &address)
{
    ui->labelTcpStatus->setText(QString("客户端: %1").arg(m_tcpServer->clientCount()));
    LOG_INFO(QString("TCP 客户端已连接: %1  (客户端数: %2)")
                 .arg(address).arg(m_tcpServer->clientCount()));
    appendTcpLog(QString("客户端已连接 %1 (当前 %2 个)")
                     .arg(address).arg(m_tcpServer->clientCount()));
    updateStatusBar();
}

void MainWindow::onTcpClientDisconnected(const QString &address)
{
    ui->labelTcpStatus->setText(QString("客户端: %1").arg(m_tcpServer->clientCount()));
    LOG_INFO(QString("TCP 客户端已断开: %1  (剩余客户端数: %2)")
                 .arg(address).arg(m_tcpServer->clientCount()));
    appendTcpLog(QString("客户端已断开 %1 (剩余 %2 个)")
                     .arg(address).arg(m_tcpServer->clientCount()));
    updateStatusBar();
}

void MainWindow::onTcpDataReceived(const QString &data)
{
    LOG_INFO(QString("TCP 收到客户端数据: %1").arg(data));
    appendTcpLog(QString("收到: %1").arg(data));
    handleTcpCommand(data);
}

// ============================================================
// TCP 客户端控制指令: 便于第三方客户端远程操作云台/镜头
//   指令: zoom +/- | focus +/- | iris +/- | ptz up|down|left|right [ms] | stop | status | help
// ============================================================
void MainWindow::handleTcpCommand(const QString &line)
{
    if (!m_tcpServer) return;
    // Qt 5.12 无 Qt::SkipEmptyParts, 使用 QString::SkipEmptyParts 以兼容
    const QStringList p = line.split(QRegularExpression("\\s+"), QString::SkipEmptyParts);
    if (p.isEmpty()) return;

    const QString c = p.value(0).toLower();
    const qint64 ts = QDateTime::currentMSecsSinceEpoch();

    if (c == "help" || c == "?") {
        m_tcpServer->sendText(QString(
            "{\"type\":\"ack\",\"cmd\":\"%1\",\"result\":\"help\",\"timestamp\":%2,"
            "\"usage\":\"zoom +/- | focus +/- | iris +/- | ptz up|down|left|right [ms] | stop | status\"}")
            .arg(line).arg(ts));
        return;
    }

    if (c == "status") {
        // 数值统一保留两位小数
        m_tcpServer->sendText(QString(
            "{\"type\":\"status\",\"timestamp\":%1,\"visible_connected\":%2,"
            "\"thermal_connected\":%3,\"zoom\":%4,\"iris_level\":%5}")
            .arg(ts)
            .arg(m_visibleConnected ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(m_thermalConnected ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(QString::number(m_currentZoomPos / ZOOM_POS_PER_X, 'f', 2))
            .arg(m_visibleCamera->irisLevel()));
        appendTcpLog(QStringLiteral("已回复 status"));
        return;
    }

    QString result = "ok";
    QString extra;   // 附加字段(如云台点动时长)
    if (c == "zoom") {
        const QString d = p.value(1);
        if (!m_visibleConnected) {
            result = "camera-not-connected";
        } else if (d == "+" || d == "-") {
            m_lastZoomActionMs = QDateTime::currentMSecsSinceEpoch();
            m_visibleCamera->zoomStep(d == "+" ? +1 : -1);
        } else {
            result = "bad-arg";
        }
    } else if (c == "focus") {
        const QString d = p.value(1);
        if (!m_visibleConnected) {
            result = "camera-not-connected";
        } else if (d == "+" || d == "-") {
            m_visibleCamera->focusStep(d == "+" ? +1 : -1);
        } else {
            result = "bad-arg";
        }
    } else if (c == "iris") {
        const QString d = p.value(1);
        if (!m_visibleConnected) {
            result = "camera-not-connected";
        } else if (d == "+" || d == "-") {
            m_visibleCamera->irisStep(d == "+" ? +1 : -1);
            updateIrisValueLabel();
        } else {
            result = "bad-arg";
        }
    } else if (c == "ptz") {
        const QString d = p.value(1).toLower();
        int ms = p.value(2).toInt();
        if (ms <= 0) ms = 300;
        if (ms > 5000) ms = 5000;
        if (!m_visibleConnected) {
            result = "camera-not-connected";
        } else if (d == "up")    { m_visibleCamera->ptzPanTilt(0, PTZ_TILT_SPEED); }
        else if (d == "down")    { m_visibleCamera->ptzPanTilt(0, -PTZ_TILT_SPEED); }
        else if (d == "left")    { m_visibleCamera->ptzPanTilt(-PTZ_PAN_SPEED, 0); }
        else if (d == "right")   { m_visibleCamera->ptzPanTilt(PTZ_PAN_SPEED, 0); }
        else                     { result = "bad-arg"; }
        if (result == "ok") {
            QTimer::singleShot(ms, this, [this]() { m_visibleCamera->ptzStop(); });
            extra = QString(",\"duration_ms\":%1").arg(ms);
        }
    } else if (c == "stop") {
        m_visibleCamera->ptzStop();
        m_visibleCamera->zoomStop();
    } else {
        result = "unknown-cmd";
    }

    m_tcpServer->sendText(QString(
        "{\"type\":\"ack\",\"cmd\":\"%1\",\"result\":\"%2\",\"timestamp\":%3%4}")
        .arg(line).arg(result).arg(ts).arg(extra));
    appendTcpLog(QString("指令 '%1' -> %2").arg(line, result));
}

// ============================================================
// UI Actions
// ============================================================
void MainWindow::on_btnConnectThermal_clicked()
{
    if (m_thermalConnected) {
        LOG_INFO("热像仪已连接, 忽略重复连接请求");
        return;
    }
    ConfigManager &cfg = ConfigManager::instance();
    QString url = cfg.thermalRtspUrl();
    LOG_INFO(QString("=== UI Action: Connect Thermal Camera ==="));
    LOG_INFO(QString("  RTSP URL: %1").arg(url));
    ui->btnConnectThermal->setEnabled(false);   // 建链成功前禁止重复点击
    m_thermalCamera->start(url);
    m_detectionTimer->start(100);  // 10Hz detection
    LOG_INFO("Detection timer started (100ms interval)");
    // 安全网: 若因上一次采集线程未退出等原因未能建链, 12.5s 后恢复按钮可点击
    QTimer::singleShot(12500, this, [this]() {
        if (ui && !m_thermalConnected) ui->btnConnectThermal->setEnabled(true);
    });
}

void MainWindow::on_btnDisconnectThermal_clicked()
{
    LOG_INFO("=== UI Action: Disconnect Thermal Camera ===");
    m_detectionTimer->stop();
    m_thermalCamera->stop();
    ui->labelThermal->clear();
    ui->labelThermal->setText("热像仪画面");
    // 采集线程自身 stop() 不会发出状态信号, 这里显式复位界面状态,
    // 否则"连接热像仪"按钮会一直停留在禁用状态而无法再次点击。
    onThermalConnectionChanged(false);
    LOG_INFO("Thermal camera disconnected, detection timer stopped");
}

void MainWindow::on_btnConnectVisible_clicked()
{
    if (m_visibleConnected) {
        LOG_INFO("可见光相机已连接, 忽略重复连接请求");
        return;
    }
    ConfigManager &cfg = ConfigManager::instance();
    QString url = cfg.visibleRtspUrl();
    LOG_INFO(QString("=== UI Action: Connect Visible Camera ==="));
    LOG_INFO(QString("  RTSP URL: %1").arg(url));
    // 连接前按配置的码流通道设定预览分辨率: 主码流 4K 走 GPU 缩放到 1080p(满帧流畅),
    // 1080p/子码流保持原分辨率。该设置必须在 start() 之前完成。
    {
        const QRegularExpressionMatch m = QRegularExpression("Channels/(\\d+)$").match(url);
        applyStreamProfile(m.hasMatch() ? m.captured(1).toInt() : VISIBLE_STREAM_THIRD);
    }
    ui->btnConnectVisible->setEnabled(false);   // 建链成功前禁止重复点击
    m_visibleCamera->start(url);
    // 安全网: 未能建链时恢复按钮可点击, 避免界面停留在禁用态
    QTimer::singleShot(12500, this, [this]() {
        if (ui && !m_visibleConnected) ui->btnConnectVisible->setEnabled(true);
    });
}

void MainWindow::on_btnDisconnectVisible_clicked()
{
    LOG_INFO("=== UI Action: Disconnect Visible Camera ===");
    if (m_recording) stopRecording();   // 断开前先结束录像, 确保视频文件正常收尾
    m_visibleCamera->stop();
    // 断开时把相机恢复为自动曝光, 避免遗留手动曝光参数影响后续单独使用
    m_visibleCamera->setAutoExposure();
    ui->labelVisible->clear();
    ui->labelVisible->setText("可见光画面");
    // 同热像仪: 显式复位界面状态, 恢复"连接可见光"按钮可点击
    onVisibleConnectionChanged(false);
    LOG_INFO("Visible camera disconnected");
}

void MainWindow::onZoomPositionChanged(int absoluteZoom)
{
    // 相机回读的实际绝对变焦位置: 直接刷新"变焦 ±"按钮旁的实际倍率显示
    const int pos = qBound(ZOOM_POS_MIN, absoluteZoom, ZOOM_POS_MAX);

    // ---- 非预期变焦检测 ----
    // 用户未操作、也没有正在执行的步进闭环, 但位置明显变化 => 记录告警,
    // 便于判断"相机自动变焦"是设备侧行为还是软件误动作。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastZoomReported > 0 && !m_visibleCamera->isZoomBusy() &&
        qAbs(pos - m_lastZoomReported) > 2 && (now - m_lastZoomActionMs) > 3000) {
        LOG_WARN(QString("检测到变焦位置自行变化: %1 -> %2 (%.1f× -> %.1f×), 非本软件下发")
                     .arg(m_lastZoomReported).arg(pos)
                     .arg(m_lastZoomReported / ZOOM_POS_PER_X, 0, 'f', 1)
                     .arg(pos / ZOOM_POS_PER_X, 0, 'f', 1));
    }
    m_lastZoomReported = pos;

    updateZoomValueLabel(pos);
}

void MainWindow::on_sliderExposure_valueChanged(int value)
{
    LOG_INFO(QString("=== UI Action: Exposure slider = %1 ===").arg(value));
    ui->labelExposure->setText(QString("曝光: %1").arg(value));
    // 曝光滑条同时驱动光圈与增益, 因此同步刷新增益滑条显示
    ui->sliderGain->blockSignals(true);
    ui->sliderGain->setValue(value);
    ui->sliderGain->blockSignals(false);
    ui->labelGain->setText(QString("增益 Gain: %1").arg(value));
    m_visibleCamera->setExposure(value);
    // 光圈滑条已移除: 光影光圈档位改在"光圈 ±"按钮旁显示
    if (m_labelIrisValue) {
        m_labelIrisValue->setText(QString("Iris %1")
                                      .arg(VisibleCamera::irisLevelFromSlider(value)));
    }
}

void MainWindow::on_sliderGain_valueChanged(int value)
{
    LOG_INFO(QString("=== UI Action: Gain slider = %1 ===").arg(value));
    ui->labelGain->setText(QString("增益 Gain: %1").arg(value));
    m_visibleCamera->setGain(value);
}

void MainWindow::on_sliderBrightness_valueChanged(int value)
{
    LOG_INFO(QString("=== UI Action: Brightness slider = %1 ===").arg(value));
    ui->labelBrightness->setText(QString("亮度: %1 (下发中…)").arg(value));
    m_irLaser->setBrightness(value);
}

void MainWindow::on_sliderLaserZoom_valueChanged(int value)
{
    LOG_INFO(QString("=== UI Action: Laser zoom slider = %1 ===").arg(value));
    ui->labelLaserZoom->setText(QString("光斑大小: %1 (下发中…)").arg(value));
    m_irLaser->setZoomPosition(value);
}

void MainWindow::on_btnStartTcp_clicked()
{
    ConfigManager &cfg = ConfigManager::instance();
    int port = cfg.tcpServerPort();
    LOG_INFO(QString("=== UI Action: Start TCP Server ==="));
    LOG_INFO(QString("  Port: %1").arg(port));
    if (m_tcpServer->start(static_cast<quint16>(port))) {
        ui->labelTcpStatus->setText("监听中");
        ui->labelTcpStatus->setStyleSheet("color: green;");
        const QString ip = localIpString();
        if (m_labelTcpAddress) {
            m_labelTcpAddress->setText(QString("服务地址: %1:%2").arg(ip).arg(port));
        }
        LOG_INFO(QString("TCP 服务器已启动: %1:%2 (监听 0.0.0.0:%2)").arg(ip).arg(port));
        appendTcpLog(QString("服务器已启动 %1:%2").arg(ip).arg(port));
    } else {
        LOG_ERROR(QString("Failed to start TCP server on port %1").arg(port));
        if (m_labelTcpAddress) m_labelTcpAddress->setText("服务地址: 启动失败");
        appendTcpLog(QString("服务器启动失败 (端口 %1)").arg(port));
    }
}

void MainWindow::on_btnStopTcp_clicked()
{
    LOG_INFO("=== UI Action: Stop TCP Server ===");
    m_tcpServer->stop();
    ui->labelTcpStatus->setText("已停止");
    ui->labelTcpStatus->setStyleSheet("color: red;");
    LOG_INFO("TCP server stopped");
    appendTcpLog("服务器已停止");
}

void MainWindow::on_btnConnectLaser_clicked()
{
    if (m_irLaser->isOpen()) {
        LOG_INFO("红外激光已连接, 忽略重复连接请求");
        return;
    }
    ConfigManager &cfg = ConfigManager::instance();
    QString portName = ui->comboSerialPort->currentText();
    if (portName.isEmpty()) {
        portName = cfg.serialPortName();
    }
    // 补全设备路径: 组合框内可能是 "ir_laser" / "ttyUSB0" 这类短名
    if (!portName.startsWith("/dev/") && QFile::exists("/dev/" + portName)) {
        portName = "/dev/" + portName;
    }
    int baudRate = cfg.serialBaudRate();
    LOG_INFO(QString("=== UI Action: Connect Laser ==="));
    LOG_INFO(QString("  Serial port: %1 @ %2bps").arg(portName).arg(baudRate));
    // RS-485 方向控制: FT232 类"USB转RS-485"适配器由 RTS 控制收发方向,
    // 不配置的话收发器常驻接收态 -> 数据发不上总线, 表现为"永远无应答"。
    m_irLaser->setRs485RtsEnabled(cfg.serialRs485Rts());
    m_irLaser->setRs485RtsActiveLow(cfg.serialRs485RtsActiveLow());
    m_irLaser->setAddress(cfg.getValue("serial/address", 1).toInt());
    LOG_INFO(QString("  RS-485 RTS 方向控制: %1%2, 通讯地址: %3")
                 .arg(cfg.serialRs485Rts() ? "启用" : "关闭")
                 .arg(cfg.serialRs485RtsActiveLow() ? "(反相)" : "")
                 .arg(m_irLaser->address()));

    ui->btnConnectLaser->setEnabled(false);
    if (m_irLaser->open(portName, baudRate)) {
        LOG_INFO("Laser connected successfully");
        // 连接后立即回读一次并启动"无应答"检测
        QTimer::singleShot(400, this, [this]() {
            m_irLaser->queryStatus();
            onLaserCommandSent();
        });
    } else {
        ui->btnConnectLaser->setEnabled(true);
        ui->labelLaserStatus->setText("连接失败");
        ui->labelLaserStatus->setStyleSheet("color: #ef4444; font-weight: bold;");
        LOG_ERROR(QString("Failed to connect laser on %1").arg(portName));
    }
}

void MainWindow::on_btnDisconnectLaser_clicked()
{
    LOG_INFO("=== UI Action: Disconnect Laser ===");
    m_irLaser->close();
    ui->labelLaserStatus->setText("未连接");
    ui->labelLaserStatus->setStyleSheet("color: red;");
    LOG_INFO("Laser disconnected");
}

// 设备连接状态标签统一刷新: 已连接=绿色, 未连接=红色
//   状态栏(界面最低端): 热像仪 -> 可见光 -> 红外激光 -> TCP客户端
//   各功能页内:          热源检测页热像仪状态 / 可见光页可见光状态
void MainWindow::updateDeviceStatusLabels()
{
    static const QString kGreen = QStringLiteral("color: #22c55e; padding: 0 6px; font-weight: bold;");
    static const QString kRed   = QStringLiteral("color: #ef4444; padding: 0 6px; font-weight: bold;");
    const bool laserOk = (m_irLaser != nullptr) && m_irLaser->isOpen();

    auto apply = [&](QLabel *lbl, const QString &name, bool ok, const QString &extra) {
        if (!lbl) return;
        lbl->setText(QStringLiteral("%1: %2%3")
                         .arg(name,
                              ok ? QStringLiteral("已连接") : QStringLiteral("未连接"),
                              extra));
        lbl->setStyleSheet(ok ? kGreen : kRed);
    };

    apply(m_labelStatusThermal, QStringLiteral("热像仪"), m_thermalConnected,
          m_thermalConnected ? QStringLiteral(" (%1fps)").arg(m_thermalFps, 0, 'f', 1)
                             : QString());
    apply(m_labelStatusVisible, QStringLiteral("可见光"), m_visibleConnected,
          m_visibleConnected ? QStringLiteral(" (%1fps)").arg(m_visibleFps, 0, 'f', 1)
                             : QString());
    apply(m_labelStatusLaser,   QStringLiteral("红外激光"), laserOk, QString());

    // 功能页内的连接状态标签
    apply(m_labelThermalConn,   QStringLiteral("热像仪"), m_thermalConnected, QString());
    apply(m_labelVisibleConn,   QStringLiteral("可见光"), m_visibleConnected, QString());

    if (m_labelStatusTcp) {
        const int n = m_tcpServer ? m_tcpServer->clientCount() : 0;
        m_labelStatusTcp->setText(QStringLiteral("TCP客户端: %1").arg(n));
        m_labelStatusTcp->setStyleSheet(QStringLiteral("color: #9fc4e8; padding: 0 6px;"));
    }
}

void MainWindow::updateStatusBar()
{
    // 析构阶段 ui 已销毁: 直接返回, 避免访问已释放的 statusBar()/标签
    if (!m_labelStatusThermal) return;
    updateDeviceStatusLabels();
    // 注意: showMessage() 的临时消息会隐藏用 addWidget() 添加的部件,
    //       因此设备连接状态一律使用 addPermanentWidget() 添加, 保证常驻可见。
    statusBar()->showMessage(QString("热像仪 %1fps | 可见光 %2fps")
                                 .arg(m_thermalFps, 0, 'f', 1)
                                 .arg(m_visibleFps, 0, 'f', 1));
}

// ============================================================
// 扩展界面构建: 热源检测分组 / TCP地址 / 日志窗体 / 分辨率标签
// ============================================================
void MainWindow::createExtraUi()
{
    // ---------- 1. 热源检测分组 ----------
    // 分组框(含"连接/断开热像仪"按钮)已在 .ui 中定义, 位置在"可见光控制"之后
    m_groupDetect = ui->groupDetect;
    QVBoxLayout *dl = ui->detectLayout;

    // 热像仪连接状态(绿=已连接 / 红=未连接), 紧跟"连接/断开热像仪"按钮行
    m_labelThermalConn = new QLabel(m_groupDetect);
    m_labelThermalConn->setTextFormat(Qt::PlainText);
    m_labelThermalConn->setStyleSheet("color: #ef4444; font-weight: bold;");
    dl->insertWidget(1, m_labelThermalConn);

    m_labelDetectStatus = new QLabel("检测状态: 未检测到热源目标", m_groupDetect);
    m_labelDetectStatus->setStyleSheet("color: red; font-weight: bold;");
    dl->addWidget(m_labelDetectStatus);

    dl->addWidget(new QLabel("检测模式", m_groupDetect));
    m_comboDetectMode = new QComboBox(m_groupDetect);
    m_comboDetectMode->addItem("自动 (最亮+最大面积)");
    m_comboDetectMode->addItem("手动 (最低温度/最小面积)");
    m_comboDetectMode->setCurrentIndex(
        m_heatDetector->selectMode() == HeatSourceDetector::Manual ? 1 : 0);
    dl->addWidget(m_comboDetectMode);

    QLabel *tempLabel = new QLabel(m_groupDetect);
    tempLabel->setText(QString("最低温度: %1 °C")
                           .arg(m_heatDetector->manualMinTemp(), 0, 'f', 1));
    dl->addWidget(tempLabel);
    m_spinMinTemp = new QDoubleSpinBox(m_groupDetect);
    m_spinMinTemp->setRange(-20.0, 650.0);
    m_spinMinTemp->setDecimals(1);
    m_spinMinTemp->setSingleStep(1.0);
    m_spinMinTemp->setValue(m_heatDetector->manualMinTemp());
    m_spinMinTemp->setToolTip("手动模式: 只保留温度高于该值的区域");
    dl->addWidget(m_spinMinTemp);

    QLabel *areaLabel = new QLabel(m_groupDetect);
    areaLabel->setText(QString("最小面积: %1 px").arg(m_heatDetector->manualMinArea()));
    dl->addWidget(areaLabel);
    m_spinMinArea = new QSpinBox(m_groupDetect);
    m_spinMinArea->setRange(1, 100000);
    m_spinMinArea->setValue(m_heatDetector->manualMinArea());
    m_spinMinArea->setToolTip("手动模式: 只保留像素数不小于该值的区域");
    dl->addWidget(m_spinMinArea);

    m_checkGrid = new QCheckBox("显示网格线", m_groupDetect);
    m_checkGrid->setChecked(m_gridEnabled);
    dl->addWidget(m_checkGrid);

    const bool manual = (m_heatDetector->selectMode() == HeatSourceDetector::Manual);
    m_spinMinTemp->setEnabled(manual);
    m_spinMinArea->setEnabled(manual);

    // 数值标签随控件实时更新
    connect(m_spinMinTemp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [tempLabel](double v) {
                tempLabel->setText(QString("最低温度: %1 °C").arg(v, 0, 'f', 1));
            });
    connect(m_spinMinArea, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [areaLabel](int v) {
                areaLabel->setText(QString("最小面积: %1 px").arg(v));
            });
    connect(m_comboDetectMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::on_comboDetectMode_currentIndexChanged);
    connect(m_spinMinTemp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::on_spinMinTemp_valueChanged);
    connect(m_spinMinArea, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::on_spinMinArea_valueChanged);
    connect(m_checkGrid, &QCheckBox::toggled, this, &MainWindow::on_checkGrid_toggled);

    // ---------- 2. TCP 服务器: 服务地址 + 客户端交互日志 ----------
    m_labelTcpAddress = new QLabel("服务地址: 未启动", this);
    m_labelTcpAddress->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_labelTcpAddress->setWordWrap(true);
    if (ui->tcpLayout) ui->tcpLayout->insertWidget(1, m_labelTcpAddress);

    // TCP 交互示例: 输出格式 + 控制指令, 方便第三方客户端对接云台
    m_tcpExampleView = new QPlainTextEdit(ui->groupTcp);
    m_tcpExampleView->setReadOnly(true);
    m_tcpExampleView->setMaximumHeight(150);
    m_tcpExampleView->setStyleSheet("font-family: monospace; font-size: 11px;");
    m_tcpExampleView->setPlainText(QStringLiteral(
        "【服务端输出示例】(每行一条 JSON)\n"
        "{\"type\":\"welcome\",\"message\":\"SubseaImagingSystem TCP Server\",\"timestamp\":1730000000000}\n"
        "{\"type\":\"angle_data\",\"timestamp\":1730000000000,\"delta_x\":1.20,\"delta_y\":-0.80,"
        "\"distance\":1.44,\"valid\":true,\"max_temp\":52.30,\"source_x\":330,\"source_y\":250}\n"
        "\n"
        "【客户端控制指令】(每行一条, 回车结束)\n"
        "  zoom + | zoom -              调焦拉近 / 拉远一档\n"
        "  focus + | focus -            聚焦更远 / 更近\n"
        "  iris + | iris -              光圈开大 / 关小\n"
        "  ptz up|down|left|right [ms]  云台点动(默认300ms, 最长5000ms)\n"
        "  stop                         停止云台与调焦\n"
        "  status | help                查询状态 / 帮助\n"
        "应答示例: {\"type\":\"ack\",\"cmd\":\"zoom +\",\"result\":\"ok\",\"timestamp\":1730000000000}\n"));
    if (ui->tcpLayout) ui->tcpLayout->addWidget(m_tcpExampleView);

    // TCP 分组内的客户端交互日志文本框(只显示最新交互信息)
    m_tcpLogView = new QPlainTextEdit(ui->groupTcp);
    m_tcpLogView->setObjectName("tcpLogView");   // 供科技风样式表定位
    m_tcpLogView->setReadOnly(true);
    m_tcpLogView->setMaximumBlockCount(200);     // 只保留最新 200 条交互记录
    m_tcpLogView->setPlaceholderText("TCP 客户端交互日志");
    m_tcpLogView->setMinimumHeight(72);
    m_tcpLogView->setMaximumHeight(120);
    m_tcpLogView->setStyleSheet("font-family: monospace; font-size: 11px;");
    if (ui->tcpLayout) ui->tcpLayout->addWidget(m_tcpLogView);

    // ---------- 3.1 红外激光: 手动刷新状态按钮 + 周期回读定时器 ----------
    m_btnLaserRefresh = new QPushButton("刷新激光状态", this);
    if (ui->laserLayout) ui->laserLayout->addWidget(m_btnLaserRefresh);
    connect(m_btnLaserRefresh, &QPushButton::clicked, this, [this]() {
        m_irLaser->queryStatus();
        onLaserCommandSent();
    });

    m_laserStatusTimer = new QTimer(this);
    m_laserStatusTimer->setInterval(3000);   // 每 3 秒回读一次激光器状态
    connect(m_laserStatusTimer, &QTimer::timeout, this, [this]() {
        if (m_irLaser->isOpen()) {
            m_irLaser->queryStatus();
            onLaserCommandSent();
        }
    });

    // 下发后 1.5s 内无任何应答 -> 提示"无应答"并给出排查方向
    m_laserReplyTimer = new QTimer(this);
    m_laserReplyTimer->setSingleShot(true);
    m_laserReplyTimer->setInterval(1500);
    connect(m_laserReplyTimer, &QTimer::timeout, this, [this]() {
        if (m_laserGotReply) return;
        if (m_labelLaserComms) {
            m_labelLaserComms->setText(QStringLiteral(
                "通信: 无应答 —— 请检查 RS-485 A/B(T/R+、T/R-)是否接反、"
                "适配器是否处于 RS-422 四线模式、波特率/地址是否一致"));
            m_labelLaserComms->setStyleSheet("color: #ef4444; font-weight: bold;");
        }
    });

    // 红外激光控制面板(开关 / 光斑± / 角度± / 亮度± / 波特率 / 自检)
    setupLaserControlPanel();

    // ---------- 4. 状态栏(界面最低端): 设备连接状态 + 两台相机分辨率 ----------
    // 设备连接状态顺序: 热像仪 -> 可见光 -> 红外激光 -> TCP客户端
    // 必须用 addPermanentWidget: showMessage() 的临时消息会隐藏 addWidget() 添加的部件。
    m_labelStatusThermal = new QLabel(this);
    m_labelStatusVisible = new QLabel(this);
    m_labelStatusLaser   = new QLabel(this);
    m_labelStatusTcp     = new QLabel(this);
    for (QLabel *lbl : { m_labelStatusThermal, m_labelStatusVisible,
                         m_labelStatusLaser, m_labelStatusTcp }) {
        lbl->setTextFormat(Qt::PlainText);
        lbl->setStyleSheet("color: #ef4444; padding: 0 6px; font-weight: bold;");
    }
    statusBar()->addPermanentWidget(m_labelStatusThermal);
    statusBar()->addPermanentWidget(m_labelStatusVisible);
    statusBar()->addPermanentWidget(m_labelStatusLaser);
    statusBar()->addPermanentWidget(m_labelStatusTcp);

    m_labelThermalRes = new QLabel("热像仪: --", this);
    m_labelVisibleRes = new QLabel("可见光: --", this);
    statusBar()->addPermanentWidget(m_labelThermalRes);
    statusBar()->addPermanentWidget(m_labelVisibleRes);

    // ---------- 5. 目标信息内新增一行: 鼠标坐标 + 灰度 + 温度 探针 ----------
    m_labelProbe = new QLabel(QStringLiteral("坐标: --   灰度: --   温度: --"), ui->thermalInfoBox);
    m_labelProbe->setStyleSheet("color: #67e8f9; font-weight: bold;");
    m_labelProbe->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_labelProbe->setMinimumWidth(0);
    if (ui->thermalInfoLayout) {
        ui->thermalInfoLayout->addWidget(m_labelProbe, 2, 0, 1, 2);
    }

    LOG_INFO("扩展界面已创建: 热源检测分组 / TCP服务地址 / TCP客户端交互日志 / 分辨率标签");

    // 可见光控制页: 拉取码流选择 + 镜头控制(变焦/聚焦/光圈±) + 录像取证
    setupVisibleControlPanel();

    // 把四个功能分组整理进控制面板的四个 Tab 页
    setupControlTabs();
}

// ============================================================
// 可见光控制页增强:
//   1) 拉取码流选择(主码流101 4K / 第三码流103 1080p / 子码流102 704x576)
//   2) 变焦滑条 -> 变焦±/聚焦±/光圈±/方向/一键巡航 按钮(与安防相机网页操作一致)
//   3) 数值标签固定宽度, 消除调节时面板"左右滑动"的抖动
// ============================================================
// ============================================================
// 红外激光控制面板(代码构建)
//   依据《激光器控制协议》:
//     开关        0x01,0x01 (data1: 0关/1开)
//     变倍±(光斑)  0x00,0x20 / 0x00,0x40 ; 停止 0x00,0x00
//     出光角度±   0x01,0x04 (data1: 0减小/1增大)
//     行程位置    0x01,0x05 (0x0000~0x4000)
//     亮度        0x01,0x03 (data1: 0~255)
//     查询        0x02,0x01 / 0x02,0x03 / 0x02,0x05 / 0x02,0x0F / 0x09,0x01
// ============================================================
void MainWindow::setupLaserControlPanel()
{
    QVBoxLayout *lay = ui->laserLayout;
    if (!lay) return;

    // 旧的"光斑大小/亮度"滑条与标签改由按钮步进控制, 隐藏旧控件
    ui->labelLaserZoom->hide();
    ui->sliderLaserZoom->hide();
    ui->labelBrightness->hide();
    ui->sliderBrightness->hide();

    m_labelLaserComms = new QLabel(QStringLiteral("通信: --"), ui->groupLaser);
    m_labelLaserComms->setWordWrap(true);
    m_labelLaserComms->setStyleSheet("color: #9fc4e8;");
    lay->addWidget(m_labelLaserComms);

    QGroupBox *panel = new QGroupBox(QStringLiteral("激光器控制"), ui->groupLaser);
    panel->setObjectName("ptzPanel");
    QGridLayout *g = new QGridLayout(panel);
    g->setContentsMargins(8, 16, 8, 8);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(6);

    auto mkBtn = [&](const QString &t, const QString &tip) {
        QPushButton *b = new QPushButton(t, panel);
        b->setToolTip(tip);
        b->setMinimumHeight(30);
        b->setMinimumWidth(64);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    auto mkVal = [&](const QString &t) {
        QLabel *l = new QLabel(t, panel);
        l->setMinimumWidth(104);
        l->setStyleSheet("color: #67e8f9; font-weight: bold;");
        return l;
    };

    // 光斑大小即为出光角度(角度越大光斑越大), 因此用"光斑角度 ±"调节即可,
    // 不再单独提供变倍按钮; 行程位置状态也不再单独显示。
    //   角度范围 2°~65°; 2°=远角(光斑最小): 有效距离>800m, 光斑直径<21m
    QPushButton *angDown = mkBtn(QStringLiteral("光斑角度 -"), QStringLiteral("角度减小: 光斑变小/照射更远"));
    QPushButton *angUp   = mkBtn(QStringLiteral("光斑角度 +"), QStringLiteral("角度增大: 光斑变大/照射更近"));
    m_labelLaserAngle = mkVal(QStringLiteral("光斑角度 --"));
    m_labelLaserAngle->setToolTip(QStringLiteral("2°=远角: 有效距离>800m, 光斑直径<21m"));
    connect(angUp,   &QPushButton::clicked, this, [this]() { m_irLaser->angleStep(+1); onLaserCommandSent(); });
    connect(angDown, &QPushButton::clicked, this, [this]() { m_irLaser->angleStep(-1); onLaserCommandSent(); });

    // 亮度±(本地 0-255 步进, 通过 0x01,0x03 下发绝对值)
    QPushButton *briDown = mkBtn(QStringLiteral("亮度 -"), QStringLiteral("亮度减小(每档 16/255)"));
    QPushButton *briUp   = mkBtn(QStringLiteral("亮度 +"), QStringLiteral("亮度增大(每档 16/255)"));
    m_labelLaserBri = mkVal(QString("亮度 %1/255").arg(m_laserBrightnessRaw));
    auto stepBrightness = [this](int delta) {
        m_laserBrightnessRaw = qBound(0, m_laserBrightnessRaw + delta, 255);
        m_irLaser->setBrightness(qRound(m_laserBrightnessRaw * 100.0 / 255.0));
        if (m_labelLaserBri) {
            m_labelLaserBri->setText(QString("亮度 %1/255").arg(m_laserBrightnessRaw));
        }
        onLaserCommandSent();
    };
    connect(briUp,   &QPushButton::clicked, this, [stepBrightness]() { stepBrightness(+16); });
    connect(briDown, &QPushButton::clicked, this, [stepBrightness]() { stepBrightness(-16); });

    // 激光开关(0x01,0x01)
    QPushButton *laserOn  = mkBtn(QStringLiteral("开启激光"), QStringLiteral("开启激光输出(0x01,0x01)"));
    QPushButton *laserOff = mkBtn(QStringLiteral("关闭激光"), QStringLiteral("关闭激光输出(0x01,0x01)"));
    connect(laserOn,  &QPushButton::clicked, this, [this]() { m_irLaser->laserSwitch(true);  onLaserCommandSent(); });
    connect(laserOff, &QPushButton::clicked, this, [this]() { m_irLaser->laserSwitch(false); onLaserCommandSent(); });

    // 当前激光器开/关状态显示(位于"开启/关闭激光"按钮右侧, 以设备应答回读为准)
    m_labelLaserSwitch = mkVal(QStringLiteral("激光: 未知"));
    m_labelLaserSwitch->setToolTip(QStringLiteral("当前激光器开/关状态(来自设备应答回读)"));

    g->addWidget(angDown,           0, 0);
    g->addWidget(angUp,             0, 1);
    g->addWidget(m_labelLaserAngle, 0, 2);
    g->addWidget(briDown,           1, 0);
    g->addWidget(briUp,             1, 1);
    g->addWidget(m_labelLaserBri,   1, 2);
    g->addWidget(laserOn,            2, 0);
    g->addWidget(laserOff,           2, 1);
    g->addWidget(m_labelLaserSwitch, 2, 2);
    g->setColumnStretch(2, 1);

    // 光斑角度规格说明
    QLabel *angleHint = new QLabel(QStringLiteral(
        "光斑角度 2°~65°；2°=远角(有效距离>800m, 光斑直径<21m)"), panel);
    angleHint->setWordWrap(true);
    angleHint->setStyleSheet("color: #9fc4e8;");
    g->addWidget(angleHint, 3, 0, 1, 3);

    lay->addWidget(panel);

    // 波特率选择(便于排查波特率不匹配; 改动后自动重连)
    QHBoxLayout *baudRow = new QHBoxLayout;
    baudRow->addWidget(new QLabel(QStringLiteral("波特率"), ui->groupLaser));
    QComboBox *baudBox = new QComboBox(ui->groupLaser);
    for (int b : { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 }) {
        baudBox->addItem(QString::number(b), b);
    }
    {
        const int idx = baudBox->findData(ConfigManager::instance().serialBaudRate());
        if (idx >= 0) baudBox->setCurrentIndex(idx);
    }
    baudRow->addWidget(baudBox, 1);
    lay->addLayout(baudRow);
    connect(baudBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, baudBox](int) {
        const int b = baudBox->currentData().toInt();
        ConfigManager::instance().setValue("serial/baud_rate", b);
        LOG_INFO(QString("激光串口波特率改为 %1, 正在重连").arg(b));
        m_irLaser->close();
        QTimer::singleShot(300, this, [this]() { on_btnConnectLaser_clicked(); });
    });

    // 串口自检(排查接线 / 波特率 / RS-485 方向)
    m_btnLaserSelfTest = new QPushButton(QStringLiteral("串口自检"), ui->groupLaser);
    m_btnLaserSelfTest->setToolTip(QStringLiteral("依次下发 5 条查询指令并统计应答, 用于排查通信问题"));
    lay->addWidget(m_btnLaserSelfTest);
    connect(m_btnLaserSelfTest, &QPushButton::clicked, this, [this]() {
        onLaserCommandSent();
        m_irLaser->selfTest();
    });

    LOG_INFO("红外激光控制面板已创建: 开关/光斑±/角度±/亮度±/波特率/自检");
}

void MainWindow::onLaserCommandSent()
{
    m_laserGotReply = false;
    if (m_laserReplyTimer) m_laserReplyTimer->start();
    if (m_labelLaserComms) {
        m_labelLaserComms->setText(QStringLiteral("通信: 等待应答…"));
        m_labelLaserComms->setStyleSheet("color: #facc15; font-weight: bold;");
    }
}

void MainWindow::setupVisibleControlPanel()
{
    QVBoxLayout *lay = ui->visCamLayout;
    if (!lay) return;

    // ---------- 0. 可见光连接状态(绿=已连接 / 红=未连接) ----------
    m_labelVisibleConn = new QLabel(ui->groupVisibleCam);
    m_labelVisibleConn->setTextFormat(Qt::PlainText);
    m_labelVisibleConn->setStyleSheet("color: #ef4444; font-weight: bold;");
    lay->insertWidget(1, m_labelVisibleConn);   // 紧跟"连接/断开可见光"按钮行

    // ---------- 1. 拉取码流选择 ----------
    QHBoxLayout *streamRow = new QHBoxLayout;
    streamRow->setSpacing(6);
    streamRow->addWidget(new QLabel(QStringLiteral("拉取码流"), ui->groupVisibleCam));

    m_comboStream = new QComboBox(ui->groupVisibleCam);
    m_comboStream->setToolTip(QStringLiteral("切换可见光 RTSP 码流: 显示与录像均使用所选码流的原生分辨率"));
    // Jetson Xavier NX 内置 NVDEC 硬解: 各码流均按原生分辨率硬解显示, 与录像分辨率一致。
    m_comboStream->addItem(QStringLiteral("主码流 4K (101)"), VISIBLE_STREAM_MAIN);
    m_comboStream->addItem(QStringLiteral("第三码流 1080p (103)"), VISIBLE_STREAM_THIRD);
    m_comboStream->addItem(QStringLiteral("子码流 704x576 (102)"), VISIBLE_STREAM_SUB);
    m_comboStream->setItemData(0, QStringLiteral("本界面内直接显示 4K 主码流(3840x2160, NVDEC 硬解), 录像保存 3840x2160"), Qt::ToolTipRole);
    m_comboStream->setItemData(1, QStringLiteral("显示 1920x1080, 录像保存 1920x1080"), Qt::ToolTipRole);
    m_comboStream->setItemData(2, QStringLiteral("显示 704x576, 录像保存 704x576"), Qt::ToolTipRole);

    // 按当前配置的 RTSP URL 选中对应码流
    int curChannel = VISIBLE_STREAM_THIRD;
    {
        const QRegularExpressionMatch m =
            QRegularExpression("Channels/(\\d+)$").match(ConfigManager::instance().visibleRtspUrl());
        if (m.hasMatch()) curChannel = m.captured(1).toInt();
    }
    for (int i = 0; i < m_comboStream->count(); ++i) {
        if (m_comboStream->itemData(i).toInt() == curChannel) {
            m_comboStream->setCurrentIndex(i);
            break;
        }
    }
    streamRow->addWidget(m_comboStream, 1);
    lay->insertLayout(2, streamRow);              // 紧跟在"可见光连接状态"之后
    connect(m_comboStream, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onStreamChannelChanged);

    // ---------- 1.1 4K 主码流提示 ----------
    // 选到主码流 101(4K) 时, 画面直接在本程序 Qt 界面内显示(不再借助外部窗口):
    // 采集端 nvv4l2decoder 走 NVDEC 硬解, 默认输出 4K 原生(3840x2160)帧;
    // 界面仅在贴图时按控件尺寸缩放, 因此分辨率栏显示 3840x2160, 缩放查看保留 4K 细节。
    m_label4kHint = new QLabel(ui->groupVisibleCam);
    m_label4kHint->setWordWrap(true);
    m_label4kHint->setStyleSheet("color: #fbbf24; font-weight: bold;");
    m_label4kHint->setVisible(false);
    lay->insertWidget(3, m_label4kHint);   // 紧跟"拉取码流"下拉框

    // 预览分辨率策略(2026-09-23 调整): 主码流 4K 默认按"4K 原生"(3840x2160)输出,
    // 保证界面显示的帧就是真实 4K(分辨率栏显示 3840x2160, 缩放查看仍有 4K 细节)。
    // 取消勾选则改由 nvvidconv 在 GPU 内缩放到显示区尺寸(满帧 25fps, 更省 CPU/带宽)。
    // 录像不受影响 —— 始终 -c:v copy 直拷所选码流(4K)。
    m_checkPreviewNative = new QCheckBox(QStringLiteral("预览 4K 原生(默认; 取消则缩放到显示区省资源)"),
                                         ui->groupVisibleCam);
    m_checkPreviewNative->setToolTip(QStringLiteral(
        "勾选(默认): 预览输出 4K 原生分辨率(3840x2160), 分辨率栏显示 3840x2160\n"
        "不勾选: 预览由 GPU 缩放到显示区尺寸, 满帧 25fps, CPU/带宽占用更低\n"
        "两者都不影响录像分辨率(录像直拷所选码流, 仍为 4K)"));
    m_nativePreview = ConfigManager::instance()
                          .getValue("visible/native_preview_4k", true).toBool();
    m_checkPreviewNative->setChecked(m_nativePreview);
    lay->insertWidget(3, m_checkPreviewNative);
    connect(m_checkPreviewNative, &QCheckBox::toggled, this, [this](bool on) {
        m_nativePreview = on;
        ConfigManager::instance().setValue("visible/native_preview_4k", on);
        LOG_INFO(QString("可见光预览策略切换: %1")
                     .arg(on ? QStringLiteral("4K 原生(3840x2160)")
                             : QStringLiteral("GPU 缩放显示区尺寸(满帧25fps)")));
        applyStreamProfile(m_visibleCamera->streamChannel());
        // 预览尺寸在建立采集管线时生效, 已连接时需要按新策略重连一次
        if (m_visibleCamera->isRunning() || m_visibleConnected) {
            onStreamChannelChanged(m_comboStream->currentIndex());
        }
    });

    // ---------- 2. 镜头控制面板: 变焦± / 聚焦± / 光圈± (按钮旁显示实际数值) ----------
    // 变焦滑条与光圈滑条已按需求删除; 一键巡航/云台方向键/自动对焦按钮一并移除。
    QGroupBox *panel = new QGroupBox(QStringLiteral("镜头参数"), ui->groupVisibleCam);
    panel->setObjectName("ptzPanel");
    QGridLayout *grid = new QGridLayout(panel);
    grid->setContentsMargins(8, 16, 8, 8);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);

    auto mkBtn = [&](const QString &text, const QString &tip) {
        QPushButton *b = new QPushButton(text, panel);
        b->setToolTip(tip);
        b->setMinimumHeight(30);
        b->setMinimumWidth(58);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    auto mkValue = [&](const QString &text) {
        QLabel *lbl = new QLabel(text, panel);
        lbl->setMinimumWidth(92);
        lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        lbl->setStyleSheet("color: #67e8f9; font-weight: bold;");
        return lbl;
    };

    // 变焦±: 单击步进一档(闭环, 自动停止), 每次点击后由相机回读刷新实际倍率
    QPushButton *btnZoomOut = mkBtn(QStringLiteral("调焦 -"), QStringLiteral("镜头拉远一档(单击步进, 自动停止)"));
    QPushButton *btnZoomIn  = mkBtn(QStringLiteral("调焦 +"), QStringLiteral("镜头拉近一档(单击步进, 自动停止)"));
    m_labelZoomValue = mkValue(QStringLiteral("调焦 --"));
    m_labelZoomValue->setToolTip(QStringLiteral("当前实际光学倍率(相机回读)"));
    // 调焦放大后画面变暗时相机可能自动切到黑白; 步进结束后按当前"日夜转换"选择重新下发
    auto refreshColorAfterZoom = [this]() {
        QTimer::singleShot(3000, this, [this]() {
            if (m_comboIrcut) {
                m_visibleCamera->setImageParam("IrcutFilter", "IrcutFilterType",
                                               m_comboIrcut->currentData().toString());
            }
        });
    };
    connect(btnZoomIn,  &QPushButton::clicked, this, [this, refreshColorAfterZoom]() {
        m_lastZoomActionMs = QDateTime::currentMSecsSinceEpoch();
        m_visibleCamera->zoomStep(+1);
        refreshColorAfterZoom();
    });
    connect(btnZoomOut, &QPushButton::clicked, this, [this, refreshColorAfterZoom]() {
        m_lastZoomActionMs = QDateTime::currentMSecsSinceEpoch();
        m_visibleCamera->zoomStep(-1);
        refreshColorAfterZoom();
    });


    // 聚焦±: 该机芯无聚焦步进接口, 按"最小聚焦距离档位"步进(10cm~无限远)
    QPushButton *btnFocusNear = mkBtn(QStringLiteral("聚焦 -"), QStringLiteral("聚焦更近(档位趋向近处)"));
    QPushButton *btnFocusFar  = mkBtn(QStringLiteral("聚焦 +"), QStringLiteral("聚焦更远(档位趋向无限远)"));
    QLabel *labelFocusValue = mkValue(VisibleCamera::focusLimitedLabel(m_visibleCamera->focusLimited()));
    labelFocusValue->setToolTip(QStringLiteral("当前最小聚焦距离档位"));
    connect(btnFocusFar,  &QPushButton::clicked, this, [this, labelFocusValue]() {
        m_visibleCamera->focusStep(+1);
        labelFocusValue->setText(VisibleCamera::focusLimitedLabel(m_visibleCamera->focusLimited()));
    });
    connect(btnFocusNear, &QPushButton::clicked, this, [this, labelFocusValue]() {
        m_visibleCamera->focusStep(-1);
        labelFocusValue->setText(VisibleCamera::focusLimitedLabel(m_visibleCamera->focusLimited()));
    });

    // 光圈±: 步进设备 IrisLevel 档位, 按钮旁显示当前档位值
    QPushButton *btnIrisClose = mkBtn(QStringLiteral("光圈 -"), QStringLiteral("光圈关小(画面更暗)"));
    QPushButton *btnIrisOpen  = mkBtn(QStringLiteral("光圈 +"), QStringLiteral("光圈开大(画面更亮)"));
    m_labelIrisValue = mkValue(QStringLiteral("Iris --"));
    m_labelIrisValue->setToolTip(QStringLiteral("当前相机光圈档位 IrisLevel"));
    connect(btnIrisOpen,  &QPushButton::clicked, this, [this]() { m_visibleCamera->irisStep(+1); updateIrisValueLabel(); });
    connect(btnIrisClose, &QPushButton::clicked, this, [this]() { m_visibleCamera->irisStep(-1); updateIrisValueLabel(); });

    grid->addWidget(btnZoomOut,       0, 0);
    grid->addWidget(btnZoomIn,        0, 1);
    grid->addWidget(m_labelZoomValue, 0, 2, 1, 2);
    grid->addWidget(btnFocusNear,     1, 0);
    grid->addWidget(btnFocusFar,      1, 1);
    grid->addWidget(labelFocusValue,  1, 2, 1, 2);
    grid->addWidget(btnIrisClose,     2, 0);
    grid->addWidget(btnIrisOpen,      2, 1);
    grid->addWidget(m_labelIrisValue, 2, 2, 1, 2);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);

    lay->insertWidget(4, panel);   // 连接状态(1) + 拉取码流(2) + 预览策略(3) 之后

    // ---------- 3. 录像取证: 开始/结束录像 + 保存路径选择 ----------
    QGroupBox *recBox = new QGroupBox(QStringLiteral("录像取证"), ui->groupVisibleCam);
    recBox->setObjectName("recPanel");
    QGridLayout *rgrid = new QGridLayout(recBox);
    rgrid->setContentsMargins(8, 16, 8, 8);
    rgrid->setHorizontalSpacing(6);
    rgrid->setVerticalSpacing(6);

    m_btnRecordStart    = new QPushButton(QStringLiteral("开始录像"), recBox);
    m_btnRecordStop     = new QPushButton(QStringLiteral("结束录像"), recBox);
    m_btnSelectSavePath = new QPushButton(QStringLiteral("选择保存路径"), recBox);
    m_btnRecordStart->setToolTip(QStringLiteral("开始录制可见光视频(与画面同源)"));
    m_btnRecordStop->setToolTip(QStringLiteral("结束录像并保存视频文件"));
    m_btnSelectSavePath->setToolTip(QStringLiteral("选择录像文件的保存目录"));
    m_btnRecordStop->setEnabled(false);
    for (QPushButton *b : { m_btnRecordStart, m_btnRecordStop, m_btnSelectSavePath }) {
        b->setMinimumHeight(30);
        b->setFocusPolicy(Qt::NoFocus);
    }

    // 解析为确实可写的目录(配置里的路径可能在当前用户下不可创建)
    m_recordSaveDir = ensureWritableDir(
        ConfigManager::instance().getValue("record/save_path",
                                           ConfigManager::instance().savePath()).toString());
    m_labelSavePath = new QLabel(QStringLiteral("保存路径: %1").arg(m_recordSaveDir), recBox);
    m_labelSavePath->setWordWrap(true);
    m_labelSavePath->setStyleSheet("color: #9fc4e8;");

    connect(m_btnRecordStart,    &QPushButton::clicked, this, &MainWindow::startRecording);
    connect(m_btnRecordStop,     &QPushButton::clicked, this, &MainWindow::stopRecording);
    connect(m_btnSelectSavePath, &QPushButton::clicked, this, &MainWindow::chooseRecordPath);

    // 录像码流/分辨率提示: 随"拉取码流"选择实时更新, 明确录像会保存成什么分辨率
    m_labelRecordInfo = new QLabel(recBox);
    m_labelRecordInfo->setWordWrap(true);
    m_labelRecordInfo->setStyleSheet("color: #67e8f9; font-weight: bold;");

    rgrid->addWidget(m_btnRecordStart,    0, 0);
    rgrid->addWidget(m_btnRecordStop,     0, 1);
    rgrid->addWidget(m_btnSelectSavePath, 1, 0, 1, 2);
    rgrid->addWidget(m_labelRecordInfo,   2, 0, 1, 2);
    rgrid->addWidget(m_labelSavePath,     3, 0, 1, 2);

    // 曝光/增益滑条属于电子图像参数: 先从原纵向布局摘出, 稍后并入"图像参数"分区
    QList<QWidget *> expGainWidgets;
    expGainWidgets << ui->labelExposure << ui->sliderExposure
                   << ui->labelGain << ui->sliderGain;
    for (QWidget *w : expGainWidgets) {
        if (w) lay->removeWidget(w);
    }

    // 录像取证排在"图像参数"之后(索引 6: 按钮行/连接状态/码流/预览策略/镜头参数/图像参数)
    lay->insertWidget(6, recBox);

    // ---------- 4. 图像参数(对应相机 Web 端"图像"配置) ----------
    QGroupBox *imgBox = new QGroupBox(QStringLiteral("图像参数"), ui->groupVisibleCam);
    imgBox->setObjectName("imgParamPanel");
    QGridLayout *igrid = new QGridLayout(imgBox);
    igrid->setContentsMargins(8, 16, 8, 8);
    igrid->setHorizontalSpacing(6);
    igrid->setVerticalSpacing(6);

    m_imageCombos.clear();
    m_imageBlocks.clear();
    m_imageTags.clear();

    int irow = 0;
    auto addParam = [&](const QString &label, const QString &block, const QString &tag,
                        const QList<QPair<QString, QString>> &opts) {
        igrid->addWidget(new QLabel(label, imgBox), irow, 0);
        QComboBox *cb = new QComboBox(imgBox);
        for (const auto &o : opts) cb->addItem(o.first, o.second);
        cb->setToolTip(QStringLiteral("相机 ISAPI: %1/%2").arg(block, tag));
        igrid->addWidget(cb, irow, 1);

        const int idx = m_imageCombos.size();
        m_imageCombos.append(cb);
        m_imageBlocks.append(block);
        m_imageTags.append(tag);
        connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, idx](int) { applyImageParam(idx); });
        ++irow;
    };

    // 曝光 / 增益(相机侧图像参数, 原位于可见光分组顶部): 统一归入"图像参数"分区
    if (ui->labelExposure && ui->sliderExposure) {
        igrid->addWidget(ui->labelExposure,  irow, 0);
        igrid->addWidget(ui->sliderExposure, irow, 1);
        ++irow;
    }
    if (ui->labelGain && ui->sliderGain) {
        igrid->addWidget(ui->labelGain,  irow, 0);
        igrid->addWidget(ui->sliderGain, irow, 1);
        ++irow;
    }

    // 日夜转换: 白天(彩色) / 夜晚(黑白) / 自动
    addParam(QStringLiteral("日夜转换"), QStringLiteral("IrcutFilter"), QStringLiteral("IrcutFilterType"),
             {{QStringLiteral("白天"), QStringLiteral("day")},
              {QStringLiteral("夜晚"), QStringLiteral("night")},
              {QStringLiteral("自动"), QStringLiteral("auto")}});
    m_comboIrcut = m_imageCombos.last();

    addParam(QStringLiteral("曝光模式"), QStringLiteral("Exposure"), QStringLiteral("ExposureType"),
             {{QStringLiteral("自动"), QStringLiteral("auto")},
              {QStringLiteral("手动"), QStringLiteral("manual")}});
    addParam(QStringLiteral("宽动态 WDR"), QStringLiteral("WDR"), QStringLiteral("mode"),
             {{QStringLiteral("关闭"), QStringLiteral("close")},
              {QStringLiteral("开启"), QStringLiteral("open")}});
    addParam(QStringLiteral("背光补偿"), QStringLiteral("BLC"), QStringLiteral("enabled"),
             {{QStringLiteral("关闭"), QStringLiteral("false")},
              {QStringLiteral("开启"), QStringLiteral("true")}});
    addParam(QStringLiteral("数字降噪"), QStringLiteral("NoiseReduce"), QStringLiteral("mode"),
             {{QStringLiteral("关闭"), QStringLiteral("close")},
              {QStringLiteral("一般"), QStringLiteral("general")}});
    addParam(QStringLiteral("白平衡"), QStringLiteral("WhiteBalance"), QStringLiteral("WhiteBalanceStyle"),
             {{QStringLiteral("自动"), QStringLiteral("auto")},
              {QStringLiteral("手动"), QStringLiteral("manual")},
              {QStringLiteral("锁定"), QStringLiteral("lock")},
              {QStringLiteral("白炽灯"), QStringLiteral("incandescent")},
              {QStringLiteral("暖光灯"), QStringLiteral("warmlight")},
              {QStringLiteral("荧光灯"), QStringLiteral("fluorescent")}});
    addParam(QStringLiteral("镜像翻转"), QStringLiteral("ImageFlip"), QStringLiteral("enabled"),
             {{QStringLiteral("关闭"), QStringLiteral("false")},
              {QStringLiteral("开启"), QStringLiteral("true")}});
    addParam(QStringLiteral("电源频率"), QStringLiteral("powerLineFrequency"), QStringLiteral("powerLineFrequencyMode"),
             {{QStringLiteral("50Hz"), QStringLiteral("50hz")},
              {QStringLiteral("60Hz"), QStringLiteral("60hz")}});
    addParam(QStringLiteral("数字慢快门"), QStringLiteral("DSS"), QStringLiteral("enabled"),
             {{QStringLiteral("关闭"), QStringLiteral("false")},
              {QStringLiteral("开启"), QStringLiteral("true")}});

    lay->insertWidget(5, imgBox);   // 图像参数排在"镜头参数"之后、"录像取证"之前

    // 未连接时先按配置显示"日夜转换"; 连接后由相机回读覆盖
    if (m_comboIrcut) {
        const int idx = m_comboIrcut->findData(
            ConfigManager::instance().getValue("visible/ircut_mode", "day").toString());
        if (idx >= 0) {
            m_comboIrcut->blockSignals(true);
            m_comboIrcut->setCurrentIndex(idx);
            m_comboIrcut->blockSignals(false);
        }
    }

    // 实际解码链路提示: 由采集端上报(硬解/软解), 内容在 onDecoderChanged 中刷新
    m_labelDecoderInfo = new QLabel(QStringLiteral("解码能力: NVDEC 硬解已启用"),
                                   ui->groupVisibleCam);
    m_labelDecoderInfo->setWordWrap(true);
    m_labelDecoderInfo->setStyleSheet("color: #9fc4e8; font-size: 11px;");
    lay->addWidget(m_labelDecoderInfo);

    // 预览策略提示: 拉流/预览/录像 三者的分辨率关系(内容在 updatePreviewStrategyLabel 中刷新)
    m_labelPreviewInfo = new QLabel(ui->groupVisibleCam);
    m_labelPreviewInfo->setWordWrap(true);
    m_labelPreviewInfo->setStyleSheet("color: #9fc4e8; font-size: 11px;");
    lay->addWidget(m_labelPreviewInfo);

    // 初始数值显示(变焦倍率待相机回读后刷新)
    updateIrisValueLabel();

    // 依据配置中的码流通道, 初始化预览/录像分辨率提示(预览与录像均按所选码流原生分辨率)。
    {
        int ch = VISIBLE_STREAM_THIRD;
        const QRegularExpressionMatch m =
            QRegularExpression("Channels/(\\d+)$").match(ConfigManager::instance().visibleRtspUrl());
        if (m.hasMatch()) ch = m.captured(1).toInt();
        applyStreamProfile(ch);
    }

    LOG_INFO("可见光控制页已重建: 连接状态 + 拉取码流 + 镜头参数(调焦/聚焦/光圈) + 图像参数 + 录像取证");
}

// ============================================================
// 热像仪: 鼠标探针 (图像坐标 + 原始灰度值 + 换算温度)
// ============================================================
int MainWindow::thermalGrayAt(int imgX, int imgY)
{
    QMutexLocker locker(&m_frameMutex);
    if (m_lastThermalFrame.isNull()) return -1;
    if (imgX < 0 || imgY < 0 ||
        imgX >= m_lastThermalFrame.width() || imgY >= m_lastThermalFrame.height()) {
        return -1;
    }
    // qGray 即热源检测所用的亮度: (r*11 + g*16 + b*5) / 32
    return qGray(m_lastThermalFrame.pixel(imgX, imgY));
}

double MainWindow::thermalTempAt(int imgX, int imgY)
{
    const int gray = thermalGrayAt(imgX, imgY);
    if (gray < 0) return 0.0;
    // 与热源检测一致: 以灰度强度线性映射到 -20..150°C
    return -20.0 + gray / 255.0 * 170.0;
}

void MainWindow::updateThermalProbe(int widgetX, int widgetY)
{
    QSize frameSize;
    {
        QMutexLocker locker(&m_frameMutex);
        frameSize = m_lastThermalFrame.size();
    }
    if (frameSize.isEmpty()) return;

    const QSize target = ui->labelThermal->size();
    if (target.isEmpty()) return;

    // 与 renderThermal() 完全相同的缩放/居中/平移几何
    const QSize base = frameSize.scaled(target, Qt::KeepAspectRatio);
    const QSize scaled(qMax(1, static_cast<int>(base.width() * m_thermalScale)),
                       qMax(1, static_cast<int>(base.height() * m_thermalScale)));
    const int ox = (target.width() - scaled.width()) / 2 + m_thermalOffset.x();
    const int oy = (target.height() - scaled.height()) / 2 + m_thermalOffset.y();

    if (widgetX < ox || widgetY < oy ||
        widgetX >= ox + scaled.width() || widgetY >= oy + scaled.height()) {
        m_hasProbe = false;
        m_probeGray = -1;
        if (m_labelProbe) m_labelProbe->setText(QStringLiteral("坐标: --   灰度: --   温度: --"));
        renderThermal();
        return;
    }

    const int imgX = qBound(0, (widgetX - ox) * frameSize.width() / scaled.width(), frameSize.width() - 1);
    const int imgY = qBound(0, (widgetY - oy) * frameSize.height() / scaled.height(), frameSize.height() - 1);
    const int gray = thermalGrayAt(imgX, imgY);
    const double temp = thermalTempAt(imgX, imgY);

    m_hasProbe = true;
    m_probeWidgetPos = QPoint(widgetX, widgetY);
    m_probeImgX = imgX;
    m_probeImgY = imgY;
    m_probeGray = gray;
    m_probeTemp = temp;

    if (m_labelProbe) {
        m_labelProbe->setText(QString("坐标: (%1,%2)   灰度: %3   温度: %4°C")
                                  .arg(imgX).arg(imgY).arg(gray)
                                  .arg(temp, 0, 'f', 1));
    }
    // 鼠标移动高频触发, 探针叠加重绘节流到 ~20fps
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastProbeRenderMs >= 50) {
        m_lastProbeRenderMs = nowMs;
        renderThermal();
    }
}

// 选择可写的取证目录: 优先使用配置/用户选择目录, 不可写时逐级回退。
// 避免出现 "mkpath 失败 -> ffmpeg 无法写文件 -> 录像失败" 的现象。
QString MainWindow::ensureWritableDir(const QString &preferred) const
{
    QStringList candidates;
    if (!preferred.isEmpty()) candidates << preferred;
    candidates << QDir::homePath() + "/evidence"
               << QDir::tempPath() + "/subsea_evidence"
               << QCoreApplication::applicationDirPath() + "/evidence";
    for (const QString &dir : candidates) {
        if (dir.isEmpty()) continue;
        if (!QDir(dir).exists() && !QDir().mkpath(dir)) continue;
        QFile probe(QDir(dir).filePath(".subsea_write_test"));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            probe.remove();
            return dir;
        }
    }
    return QDir::tempPath();
}

// ============================================================
// 可见光: 相机侧图像参数(Web 端"图像"配置)下发与回读同步
// ============================================================
void MainWindow::applyImageParam(int index)
{
    if (index < 0 || index >= m_imageCombos.size()) return;
    QComboBox *cb = m_imageCombos.at(index);
    if (!cb) return;

    const QString value = cb->currentData().toString();
    const QString block = m_imageBlocks.at(index);
    const QString tag   = m_imageTags.at(index);
    if (value.isEmpty() || block.isEmpty() || tag.isEmpty()) return;

    LOG_INFO(QString("=== UI Action: 图像参数 %1/%2 -> %3 ===").arg(block, tag, value));

    if (!m_visibleConnected) {
        LOG_WARN("可见光未连接, 图像参数仅记录在界面, 连接后可重新选择以生效");
        return;
    }
    m_visibleCamera->setImageParam(block, tag, value);

    // 记住"日夜转换"选择, 供下次启动与调焦后重新下发使用
    if (m_comboIrcut && cb == m_comboIrcut) {
        ConfigManager::instance().setValue("visible/ircut_mode", value);
    }
}

void MainWindow::syncImageParamsUi(const QString &xml)
{
    for (int i = 0; i < m_imageCombos.size(); ++i) {
        QComboBox *cb = m_imageCombos.at(i);
        if (!cb) continue;

        QRegularExpression bre(QString("<%1[^>]*>.*?</%1>").arg(m_imageBlocks.at(i)),
                               QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch bm = bre.match(xml);
        if (!bm.hasMatch()) continue;

        const QRegularExpressionMatch tm =
            QRegularExpression(QString("<%1>([^<]*)</%1>").arg(m_imageTags.at(i)))
                .match(bm.captured(0));
        if (!tm.hasMatch()) continue;

        const QString cur = tm.captured(1).trimmed();
        const int idx = cb->findData(cur);
        if (idx >= 0) {
            cb->blockSignals(true);
            cb->setCurrentIndex(idx);
            cb->blockSignals(false);
        }
        LOG_DEBUG(QString("图像参数回读: %1/%2 = %3")
                      .arg(m_imageBlocks.at(i), m_imageTags.at(i), cur));
    }
}

// ============================================================
// 可见光控制: 按钮旁数值显示
// ============================================================
void MainWindow::updateZoomValueLabel(int pos)
{
    m_currentZoomPos = pos;
    if (!m_labelZoomValue) return;
    // 绝对位置 10..230 对应光学倍率 1.0×..23.0×
    m_labelZoomValue->setText(QString("调焦 %1×").arg(pos / ZOOM_POS_PER_X, 0, 'f', 1));
}

void MainWindow::updateIrisValueLabel()
{
    if (!m_labelIrisValue) return;
    m_labelIrisValue->setText(QString("Iris %1").arg(m_visibleCamera->irisLevel()));
}

// ============================================================
// 可见光: 录像取证(录像起止时间之间保存为一段视频)
// ============================================================
void MainWindow::setRecordingUi(bool recording)
{
    m_recording = recording;
    if (m_btnRecordStart) m_btnRecordStart->setEnabled(!recording);
    if (m_btnRecordStop)  m_btnRecordStop->setEnabled(recording);
}

void MainWindow::startRecording()
{
    if (m_recording) return;
    if (!m_visibleConnected) {
        QMessageBox::information(this, QStringLiteral("录像取证"),
                                 QStringLiteral("可见光相机未连接, 无法录像"));
        return;
    }
    // 目录必须可写, 否则 ffmpeg 无法创建文件(录像会直接失败)
    QString dir = m_recordSaveDir;
    if (dir.isEmpty()) dir = ConfigManager::instance().savePath();
    const QString writable = ensureWritableDir(dir);
    if (writable != dir) {
        LOG_WARN(QString("录像目录不可写, 已改用: %1 (原: %2)").arg(writable).arg(dir));
        dir = writable;
    }
    m_recordSaveDir = dir;
    ConfigManager::instance().setValue("record/save_path", dir);
    if (m_labelSavePath) m_labelSavePath->setText(QStringLiteral("保存路径: %1").arg(dir));

    // 录像码流跟随"拉取码流"选择: 选主码流则录 4K, 选第三码流则录 1080p, 选子码流则录 704x576。
    // 仍为 -c:v copy 直拷(不解码), 不占用本机解码能力, 因此录像分辨率与所选码流严格一致。
    const int recChannel = m_visibleCamera->streamChannel();
    const QString recordUrl = ConfigManager::instance().visibleRtspUrl();   // 当前预览/所选码流地址
    const QString recRes = VisibleCamera::streamResolutionText(recChannel);
    const QString base = QString("%1/record_%2_%3")
                             .arg(dir)
                             .arg(recChannel)   // 文件名带码流通道, 便于区分分辨率
                             .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    LOG_INFO(QString("=== UI Action: 开始录像取证 -> %1 ===").arg(base));
    LOG_INFO(QString("  录像码流(直拷, 不解码): 通道 %1, 分辨率 %2").arg(recChannel).arg(recRes));
    LOG_INFO(QString("  录像 RTSP URL: %1").arg(recordUrl));
    m_visibleCamera->startRecording(base, recordUrl);
    // 以实际录像状态为准: 启动失败时 startRecording 内部已发出 recordingFailed 并复位界面
    setRecordingUi(m_visibleCamera->isRecording());
}

void MainWindow::stopRecording()
{
    if (!m_recording) return;
    LOG_INFO("=== UI Action: 结束录像取证 ===");
    m_visibleCamera->stopRecording();
    // 最终按钮状态与文件路径由 recordingStopped 信号刷新
}

void MainWindow::chooseRecordPath()
{
    QString startDir = m_recordSaveDir;
    if (startDir.isEmpty()) startDir = ConfigManager::instance().savePath();
    startDir = ensureWritableDir(startDir);   // 对话框默认落在可写目录
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择录像保存路径"), startDir);
    if (dir.isEmpty()) return;
    m_recordSaveDir = dir;
    ConfigManager::instance().setValue("record/save_path", dir);
    if (m_labelSavePath) m_labelSavePath->setText(QStringLiteral("保存路径: %1").arg(dir));
    LOG_INFO(QString("录像保存路径已设置为: %1").arg(dir));
}

// 画面左上角标题(覆盖在图像之上, 不拦截鼠标事件)
void MainWindow::setupVideoTitles()
{
    auto makeTitle = [](QWidget *parent, const QString &text) -> QLabel * {
        QLabel *lbl = new QLabel(text, parent);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);   // 不影响画面滚轮缩放/拖拽
        lbl->setStyleSheet(
            "background-color: rgba(8, 16, 28, 200); color: #67e8f9;"
            "border: 1px solid #1e3a5f; border-radius: 4px;"
            "padding: 2px 8px; font-weight: bold; font-size: 13px;");
        lbl->adjustSize();
        lbl->move(8, 8);
        lbl->raise();
        return lbl;
    };

    m_titleThermal = makeTitle(ui->labelThermal, QStringLiteral("热源监测图像"));
    m_titleVisible = makeTitle(ui->labelVisible, QStringLiteral("可见光监测图像"));

    ui->labelThermal->installEventFilter(this);   // 尺寸变化时重新定位(可见光已安装)
    LOG_INFO("画面左上角标题已创建: 热源监测图像 / 可见光监测图像");
}

void MainWindow::repositionVideoTitles()
{
    const auto place = [](QLabel *title) {
        if (!title) return;
        title->adjustSize();
        title->move(8, 8);
        title->raise();
    };
    place(m_titleThermal);
    place(m_titleVisible);
}

// ============================================================
// 可见光拉取码流切换: 记录到配置并立即重连该路码流
// ============================================================
void MainWindow::onStreamChannelChanged(int index)
{
    if (!m_comboStream || index < 0) return;
    const int channel = m_comboStream->itemData(index).toInt();
    if (channel <= 0) return;

    ConfigManager &cfg = ConfigManager::instance();
    const QString url = QString("rtsp://%1:%2@%3:554/Streaming/Channels/%4")
                            .arg(cfg.visibleCameraUser(), cfg.visibleCameraPass(),
                                 cfg.visibleCameraIp())
                            .arg(channel);
    LOG_INFO(QString("=== UI Action: 切换可见光拉取码流 -> 通道 %1 ===").arg(channel));
    LOG_INFO(QString("  RTSP URL: %1").arg(url));
    cfg.setValue("visible/rtsp_url", url);

    // 切换码流后同步设定对应的预览分辨率(主码流 4K→GPU 缩放 1080p)与录像分辨率提示
    applyStreamProfile(channel);

    if (m_visibleCamera->isRunning() || m_visibleConnected) {
        ui->btnConnectVisible->setEnabled(false);   // 重连期间避免重复点击
        m_visibleCamera->start(url);                // start() 内部会先停掉旧码流
        LOG_INFO("可见光码流已切换, 正在重连...");
    } else {
        LOG_INFO("可见光当前未连接, 码流选择已保存, 连接时生效");
    }
}

// ============================================================
// 依据所选码流设定"解码 / 预览 / 录像"三者的分辨率策略
//
//   主码流 101(4K): 解码 3840x2160 -> 预览默认 4K 原生(3840x2160) -> 录像直拷 4K
//   第三码流 103   : 解码/预览/录像 均 1920x1080(原生)
//   子码流  102    : 解码/预览/录像 均 704x576(原生)
//
//   4K 主码流的两档预览策略(2026-09-23):
//     - 默认 "4K 原生": 采集端输出 3840x2160, 界面分辨率栏显示 3840x2160,
//       缩放查看仍保留 4K 细节(代价: GPU->CPU 每帧约 33MB 拷贝, 帧率约 12fps);
//     - 可选 "GPU 缩放到显示区尺寸": nvvidconv 在 GPU(VIC) 内缩放, 界面零缩放、满帧 25fps。
//   两者都不影响录像: 始终 -c:v copy 直拷"所选码流"(仍为 4K)。
// ============================================================
void MainWindow::applyStreamProfile(int channel)
{
    if (channel <= 0 || !m_visibleCamera) return;
    m_visibleCamera->setStreamChannel(channel);

    if (channel == VISIBLE_STREAM_MAIN) {
        if (m_nativePreview) {
            // 4K 原生: 采集端不做 GPU 缩放, 直接输出码流原生分辨率(3840x2160)
            m_visibleCamera->setPreviewSize(0, 0);        // 0,0 = 原分辨率输出
            m_currentPreviewSize = QSize(3840, 2160);     // 供分辨率提示/策略标签显示
            LOG_INFO("可见光主码流: 预览输出 4K 原生 3840x2160 (界面按控件尺寸缩放)");
        } else {
            // GPU 缩放到显示区尺寸: 界面零缩放, 满帧 25fps, 更省 CPU/带宽
            const QSize out = previewOutputSize();
            m_visibleCamera->setPreviewSize(out.width(), out.height());
            m_currentPreviewSize = out;
            LOG_INFO(QString("可见光主码流: 预览输出 %1x%2 (GPU缩放, 界面零缩放)")
                         .arg(out.width()).arg(out.height()));
        }
    } else {
        // 子码流(704x576) / 第三码流(1920x1080) 本身不大, 保持原生分辨率输出
        m_visibleCamera->setPreviewSize(0, 0);
        m_currentPreviewSize = QSize(0, 0);
    }
    updateRecordStreamLabel();
    updatePreviewStrategyLabel();

    // ---- 4K 主码流: 直接在本程序界面内显示, 仅刷新提示文字(见 renderVisible) ----
    update4kHint(channel);
}

// ============================================================
// 4K 主码流显示方式: 直接在本程序 Qt 界面内显示
//
// 能力依据(2026-09 实测, Jetson Xavier NX):
//   · 采集端走 GStreamer + nvv4l2decoder(NVDEC) 硬解 4K H.265;
//   · nvvidconv 在 GPU(VIC) 内把 4K 缩放到"显示区尺寸"(勾选"预览 4K 原生"时输出 4K 原生);
//   · 界面侧只对已缩放的帧做一次贴图, 近乎零缩放, 界面线程不会被 4K 拖累;
//   · 实测 4K 主码流硬解 + GPU 缩放预览可达满帧 25fps, 延迟稳定在亚秒级。
// 因此无需外部 GStreamer 窗口, 4K 主码流与其它码流一样在 labelVisible 内显示。
// ============================================================
void MainWindow::update4kHint(int channel)
{
    if (!m_label4kHint) return;

    const bool want4k = (channel == VISIBLE_STREAM_MAIN);
    if (want4k) {
        m_label4kHint->setText(QStringLiteral(
            "4K 主码流: 本界面内直接显示(NVDEC 硬解 + 4K 原生, 可缩放查看 4K 细节)"));
        m_label4kHint->setVisible(true);
    } else {
        m_label4kHint->setVisible(false);
    }
}

// 预览输出尺寸 = 可见光显示区实际尺寸(向上取偶数), 由 nvvidconv 在 GPU 内直接缩放。
// 【仅在取消"预览 4K 原生"时使用】这样界面侧"零缩放", 帧率更高、更省资源;
// 勾选"预览 4K 原生"(默认)时改走 applyStreamProfile 的原分辨率分支, 不再缩放到显示区。
// 上限 1920x1080: 再大对显示无收益, 只增加 GPU->CPU 拷贝量。
QSize MainWindow::previewOutputSize() const
{
    QSize box = ui->labelVisible->size();
    if (box.isEmpty()) box = QSize(1280, 720);

    const int maxW = 1920;
    const int maxH = 1080;
    box.setWidth(qMin(box.width(),  maxW));
    box.setHeight(qMin(box.height(), maxH));

    // 关键: 预览输出必须保持"源画面的宽高比"(主码流 3840x2160 = 16:9)。
    // 若直接把输出尺寸设成显示区的原始尺寸(例如竖向的 510x778), nvvidconv 会把
    // 16:9 的画面硬拉伸成竖向 -> 画面变形。这里改为"16:9 内接于显示区",
    // 与 renderVisible() 的 KeepAspectRatio 一致, 界面侧仍然零缩放(或仅差 1~2 像素)。
    int w = box.width();
    int h = w * 9 / 16;
    if (h > box.height()) {
        h = box.height();
        w = h * 16 / 9;
    }
    w &= ~1;   // nvvidconv 要求偶数尺寸
    h &= ~1;
    return QSize(qMax(320, w), qMax(180, h));
}

// 预览策略说明标签: 明确"解码 / 预览 / 录像"三者的分辨率关系, 避免误以为 4K 被降级
void MainWindow::updatePreviewStrategyLabel()
{
    if (!m_labelPreviewInfo || !m_visibleCamera) return;
    const int ch = m_visibleCamera->streamChannel();

    QString text;
    if (ch == VISIBLE_STREAM_MAIN) {
        const QSize out = m_currentPreviewSize;
        QString outText;
        if (m_nativePreview) {
            outText = QStringLiteral("4K 原生 %1x%2").arg(out.width()).arg(out.height());
        } else if (out.width() > 0) {
            outText = QString("GPU 缩放 %1x%2").arg(out.width()).arg(out.height());
        } else {
            outText = QStringLiteral("码流原生分辨率");
        }
        text = QStringLiteral("主码流 4K: 解码 3840x2160 | 预览 %1 | 录像直拷 4K")
                   .arg(outText);
    } else {
        text = QStringLiteral("通道 %1: 解码/预览/录像 均为 %2(原生分辨率)")
                   .arg(ch).arg(VisibleCamera::streamResolutionText(ch));
    }
    m_labelPreviewInfo->setText(text + QStringLiteral(
        "\nNVDEC 硬解; 4K 主码流直接在本界面显示; 采集队列深度 1(始终只取最新帧)"));
    m_labelPreviewInfo->setStyleSheet("color: #9fc4e8; font-size: 11px;");
}

void MainWindow::updateRecordStreamLabel()
{
    if (!m_labelRecordInfo || !m_visibleCamera) return;
    const int ch = m_visibleCamera->streamChannel();
    QString name;
    switch (ch) {
    case VISIBLE_STREAM_MAIN:  name = QStringLiteral("主码流");   break;
    case VISIBLE_STREAM_SUB:   name = QStringLiteral("子码流");   break;
    case VISIBLE_STREAM_THIRD: name = QStringLiteral("第三码流"); break;
    default:                   name = QStringLiteral("未知");     break;
    }
    m_labelRecordInfo->setText(
        QStringLiteral("录像码流: %1 (通道 %2)  →  保存分辨率 %3 (直拷)")
            .arg(name).arg(ch).arg(VisibleCamera::streamResolutionText(ch)));
}

// 采集端上报的实际解码链路(硬解/软解): 显示到界面, 便于确认是否真正走 NVDEC 硬解
void MainWindow::onDecoderChanged(const QString &decoder)
{
    LOG_INFO(QString("可见光解码链路: %1").arg(decoder));
    if (!m_labelDecoderInfo) return;
    const bool hw = decoder.contains(QStringLiteral("硬解"));
    m_labelDecoderInfo->setText(
        QStringLiteral("当前解码: %1  (NVDEC 硬件解码, 全分辨率输出)").arg(decoder));
    m_labelDecoderInfo->setStyleSheet(hw ? "color: #22c55e; font-size: 11px;"
                                         : "color: #f59e0b; font-size: 11px;");
}

// ============================================================
// 控制面板 Tab 化: 热源检测 / 可见光控制 / 红外激光 / TCP服务器
// 目的: 原来 4 个分组纵向堆在一个面板里过于紧凑, 改为分页后每页只显示一组,
//       空间充裕且便于操作; 各分组控件本身未做任何改动。
// ============================================================
void MainWindow::setupControlTabs()
{
    if (!ui->controlPanel || !ui->controlLayout) return;

    // 原布局末尾的弹性空白不再需要(Tab 页自己占满剩余空间)
    if (ui->verticalSpacer) {
        ui->controlLayout->removeItem(ui->verticalSpacer);
        delete ui->verticalSpacer;
        ui->verticalSpacer = nullptr;
    }

    m_controlTabs = new QTabWidget(ui->controlPanel);
    m_controlTabs->setObjectName("controlTabs");
    m_controlTabs->setDocumentMode(true);
    m_controlTabs->setUsesScrollButtons(false);   // 4 个标签直接铺满, 不出现滚动箭头
    m_controlTabs->setElideMode(Qt::ElideNone);
    if (m_controlTabs->tabBar()) {
        m_controlTabs->tabBar()->setExpanding(true);
        m_controlTabs->tabBar()->setDrawBase(false);
    }

    // 顺序: 热源检测 -> 可见光控制 -> 红外激光 -> TCP服务器
    struct TabDef { const char *title; QGroupBox *group; };
    const TabDef defs[] = {
        { "热源检测",   ui->groupDetect },
        { "可见光控制", ui->groupVisibleCam },
        { "红外激光",   ui->groupLaser },
        { "TCP服务器",  ui->groupTcp },
    };

    int tabIndex = 0;
    for (const TabDef &def : defs) {
        if (!def.group) continue;

        QWidget *page = new QWidget(m_controlTabs);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(6, 10, 6, 6);
        pageLayout->setSpacing(10);

        ui->controlLayout->removeWidget(def.group);   // 先从原布局摘除, 再放进 Tab 页
        pageLayout->addWidget(def.group);
        pageLayout->addStretch(1);                    // 控件靠上, 底部留白, 避免挤压

        m_controlTabs->addTab(page, QString::fromUtf8(def.title));
        ++tabIndex;
    }

    ui->controlLayout->addWidget(m_controlTabs, 1);
    ui->controlLayout->setContentsMargins(6, 6, 6, 6);
    ui->controlLayout->setSpacing(6);

    LOG_INFO(QString("控制面板已分为 %1 个 Tab 页: 热源检测 / 可见光控制 / 红外激光 / TCP服务器")
                 .arg(tabIndex));
}

// ============================================================
// 新增控件槽函数
// ============================================================
void MainWindow::on_comboDetectMode_currentIndexChanged(int index)
{
    const bool manual = (index == 1);
    m_heatDetector->setSelectMode(manual ? HeatSourceDetector::Manual
                                         : HeatSourceDetector::AutoBrightArea);
    if (m_spinMinTemp) m_spinMinTemp->setEnabled(manual);
    if (m_spinMinArea) m_spinMinArea->setEnabled(manual);
    ConfigManager::instance().setValue("detection/manual_mode", manual);
    LOG_INFO(QString("检测模式切换为: %1")
                 .arg(manual ? "手动 (最低温度/最小面积)" : "自动 (最亮+最大面积)"));
}

void MainWindow::on_spinMinTemp_valueChanged(double value)
{
    m_heatDetector->setManualMinTemp(value);
    ConfigManager::instance().setValue("detection/min_temp", value);
    LOG_INFO(QString("手动筛选参数: 最低温度 = %1 °C").arg(value, 0, 'f', 1));
}

void MainWindow::on_spinMinArea_valueChanged(int value)
{
    m_heatDetector->setManualMinArea(value);
    ConfigManager::instance().setValue("detection/min_area_pixels", value);
    LOG_INFO(QString("手动筛选参数: 最小面积 = %1 px").arg(value));
}

void MainWindow::on_checkGrid_toggled(bool checked)
{
    m_gridEnabled = checked;
    ConfigManager::instance().setValue("display/grid", checked);
    LOG_INFO(QString("网格线显示: %1").arg(checked ? "开启" : "关闭"));
    renderThermal();
}

// ============================================================
// 科技风深色主题 (全局 QSS + 深色调色板)
// ============================================================
void MainWindow::applyTechStyle()
{
    QPalette pal = qApp->palette();
    pal.setColor(QPalette::Window, QColor(11, 18, 32));
    pal.setColor(QPalette::WindowText, QColor(226, 236, 248));
    pal.setColor(QPalette::Base, QColor(15, 24, 41));
    pal.setColor(QPalette::AlternateBase, QColor(20, 31, 51));
    pal.setColor(QPalette::Text, QColor(226, 236, 248));
    pal.setColor(QPalette::Button, QColor(22, 34, 56));
    pal.setColor(QPalette::ButtonText, QColor(226, 236, 248));
    pal.setColor(QPalette::Highlight, QColor(34, 211, 238));
    pal.setColor(QPalette::HighlightedText, QColor(6, 12, 22));
    pal.setColor(QPalette::ToolTipBase, QColor(15, 24, 41));
    pal.setColor(QPalette::ToolTipText, QColor(226, 236, 248));
    qApp->setPalette(pal);

    const QString qss = R"(
    QMainWindow, QWidget { background-color: #0b1220; color: #e2ecf8; font-size: 12px; }
    QGroupBox {
        border: 1px solid #1e3a5f; border-radius: 8px; margin-top: 14px;
        background-color: #0f1a2b; padding: 8px 8px 6px 8px;
    }
    QGroupBox::title {
        subcontrol-origin: margin; subcontrol-position: top left; left: 10px; padding: 2px 8px;
        color: #22d3ee; font-weight: bold; letter-spacing: 1px;
        background-color: #0b1220; border: 1px solid #1e3a5f; border-radius: 6px;
    }
    QLabel { background: transparent; color: #c7d7ea; }
    QPushButton {
        background-color: #16263c; color: #dbeafe; border: 1px solid #2b4a70;
        border-radius: 6px; padding: 5px 12px; font-weight: bold;
    }
    QPushButton:hover { background-color: #1d3350; border-color: #22d3ee; color: #ffffff; }
    QPushButton:pressed { background-color: #0e1c2e; border-color: #67e8f9; }
    QPushButton:disabled { background-color: #101a29; color: #4b5f78; border-color: #1b2a3f; }
    QSlider::groove:horizontal { height: 6px; background: #16263c; border-radius: 3px; }
    QSlider::sub-page:horizontal { background: #22d3ee; border-radius: 3px; }
    QSlider::handle:horizontal {
        width: 14px; margin: -5px 0; border-radius: 7px;
        background: #67e8f9; border: 1px solid #0e7490;
    }
    QSlider::handle:horizontal:hover { background: #a5f3fc; }
    QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QLineEdit {
        background-color: #0e1a2b; border: 1px solid #24425f; border-radius: 6px;
        padding: 3px 6px; color: #e2ecf8; selection-background-color: #0e7490;
    }
    QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #22d3ee; }
    QComboBox::drop-down { border: none; width: 18px; }
    QComboBox QAbstractItemView {
        background-color: #0f1a2b; border: 1px solid #22d3ee; color: #e2ecf8;
        selection-background-color: #0e7490;
    }
    QCheckBox { color: #c7d7ea; spacing: 6px; }
    QCheckBox::indicator { width: 14px; height: 14px; border-radius: 3px;
        border: 1px solid #2b4a70; background: #0e1a2b; }
    QCheckBox::indicator:checked { background: #22d3ee; border-color: #67e8f9; }
    QPlainTextEdit#tcpLogView { background-color: #08101c; color: #9fe8ff; font-family: monospace; }
    /* 控制面板 Tab(热源检测/可见光控制/红外激光/TCP服务器) */
    QTabWidget::pane {
        border: 1px solid #1e3a5f; border-radius: 8px; background-color: #0f1a2b; top: -1px;
    }
    QTabBar::tab {
        background: #132238; color: #9fc4e8; border: 1px solid #1e3a5f; border-bottom: none;
        padding: 7px 10px; margin-right: 3px; min-width: 58px;
        border-top-left-radius: 7px; border-top-right-radius: 7px;
    }
    QTabBar::tab:selected { background: #1d3552; color: #67e8f9; font-weight: bold; }
    QTabBar::tab:hover:!selected { background: #1a2c47; color: #dbeafe; }
    /* 镜头控制面板: 紧凑按钮, 单击类按键视觉更明确 */
    QGroupBox#ptzPanel QPushButton { padding: 2px 4px; font-weight: bold; }
    QGroupBox#ptzPanel QPushButton:pressed { background-color: #0e7490; border-color: #67e8f9; }
    /* 录像取证面板 */
    QGroupBox#recPanel QPushButton { padding: 4px 8px; font-weight: bold; }
    QGroupBox#recPanel QPushButton:disabled { color: #4b5f78; }
    QStatusBar { background-color: #0d1626; color: #93b4d4; border-top: 1px solid #1e3a5f; }
    QStatusBar QLabel { color: #22d3ee; font-weight: bold; padding: 0 8px; }
    QToolTip { background-color: #0f1a2b; color: #e2ecf8; border: 1px solid #22d3ee; }
    )";
    this->setStyleSheet(qss);

    // 视频画面底色(黑)保证对比度
    ui->labelThermal->setStyleSheet("background-color: #000000; color: #4b6b8a;");
    ui->labelVisible->setStyleSheet("background-color: #000000; color: #4b6b8a;");
}
