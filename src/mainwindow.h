#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QTimer>
#include <QThread>
#include <QProcess>
#include <QImage>
#include <QMutex>
#include <QStringList>
#include <QPainter>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>

#include "heatsourcedetector.h"
#include "anglecalculator.h"

// Forward declarations
class ThermalCamera;
class VisibleCamera;
class IRLaser;
class HeatSourceDetector;
class AngleCalculator;
class TcpServer;

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    // 用于: 串口下拉框点击刷新、可见光画面滚轮缩放与拖拽
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // Camera slots
    void onThermalFrameReady(const QImage &frame);
    void onVisibleFrameReady(const QImage &frame);
    void onThermalConnectionChanged(bool connected);
    void onVisibleConnectionChanged(bool connected);
    void onThermalFpsUpdated(double fps);
    void onVisibleFpsUpdated(double fps);

    // Detection & angle
    void onHeatSourcesDetected(const QVector<HeatSource> &sources);
    void onAngleCalculated(const AngleDifference &angle);

    // TCP
    void onTcpClientConnected(const QString &address);
    void onTcpClientDisconnected(const QString &address);
    void onTcpDataReceived(const QString &data);
    void handleTcpCommand(const QString &line);   // 解析客户端控制指令(调焦/聚焦/光圈/云台)

    // UI actions
    void on_btnConnectThermal_clicked();
    void on_btnDisconnectThermal_clicked();
    void on_btnConnectVisible_clicked();
    void on_btnDisconnectVisible_clicked();
    void on_sliderExposure_valueChanged(int value);
    void on_sliderGain_valueChanged(int value);
    void on_sliderBrightness_valueChanged(int value);
    void on_sliderLaserZoom_valueChanged(int value);
    void on_btnStartTcp_clicked();
    void on_btnStopTcp_clicked();
    void on_btnConnectLaser_clicked();
    void on_btnDisconnectLaser_clicked();

    // 新增控件槽
    void on_comboDetectMode_currentIndexChanged(int index);
    void on_spinMinTemp_valueChanged(double value);
    void on_spinMinArea_valueChanged(int value);
    void on_checkGrid_toggled(bool checked);
    void onZoomPositionChanged(int absoluteZoom);   // 变焦位置回读
    void onStreamChannelChanged(int index);         // 可见光拉取码流切换(主/子/第三码流)
    void onDecoderChanged(const QString &decoder);  // 采集端上报实际解码链路(硬解/软解)

    void updateDetection();

private:
    void setupUi_connections();
    void createExtraUi();                  // 创建 热源检测分组 / TCP地址标签 / TCP交互日志 / 分辨率标签
    void setupControlTabs();               // 把四个功能分组放入控制面板的四个 Tab 页
    void setupVisibleControlPanel();       // 可见光: 拉取码流选择 + 云台/镜头按钮面板(替代变焦滑条)
    void setupLaserControlPanel();         // 红外激光: 开关/光斑±/角度±/亮度±/自检(代码构建)
    void onLaserCommandSent();             // 下发激光指令后启动"无应答"检测
    void setupVideoTitles();               // 两路画面左上角标题(热源监测图像 / 可见光监测图像)
    void repositionVideoTitles();          // 画面尺寸变化时重新摆放标题
    void applyTechStyle();                 // 科技风深色主题(深色调色板 + 全局QSS)
    void refreshSerialPorts();             // 刷新可用串口列表
    void autoConnectDevices();             // 启动后自动连接可见光/热像仪/红外激光
    void updateAngleDisplay(const AngleDifference &angle);
    void updateStatusBar();
    void updateDeviceStatusLabels();       // 刷新设备连接状态标签(绿=已连接 / 红=未连接)
    void updateResolutionLabels();
    void appendTcpLog(const QString &line);  // TCP 客户端交互日志追加到 TCP 分组内文本框
    void renderThermal();                  // 重绘热像仪画面(网格/中心十字/标记框/状态)
    void renderVisible();                  // 重绘可见光画面(支持缩放/拖拽)
    QString localIpString() const;         // 本机用于对外服务的IP

    // ---- 可见光: 变焦/光圈数值显示 + 录像取证 ----
    void updateZoomValueLabel(int pos);    // 刷新变焦按钮旁的实际倍率
    void updateIrisValueLabel();           // 刷新光圈按钮旁的当前光圈值
    void startRecording();                 // 开始录像取证
    void stopRecording();                  // 结束录像取证
    void chooseRecordPath();               // 选择录像保存路径
    void setRecordingUi(bool recording);   // 同步录像按钮可用状态
    void applyStreamProfile(int channel);  // 按码流设定预览分辨率(4K→显示区尺寸 GPU缩放)并刷新录像提示
    QSize previewOutputSize() const;       // 预览输出尺寸=显示区尺寸(GPU 缩放), 使界面侧零缩放
    void updateRecordStreamLabel();        // 刷新"录像码流 / 分辨率"提示(跟随所选码流)
    void updatePreviewStrategyLabel();     // 刷新"拉流/预览/录像"分辨率策略说明

    // ---- 可见光: 相机侧图像参数(Web 端"图像"配置) ----
    void applyImageParam(int index);             // 下拉变更 -> 下发到相机
    void syncImageParamsUi(const QString &xml);  // 相机回读 -> 同步下拉显示

    // ---- 热像仪: 缩放/拖拽/鼠标探针(坐标 + 灰度 + 温度) ----
    void updateThermalProbe(int widgetX, int widgetY);   // 鼠标位置 -> 图像坐标 + 灰度 + 温度
    double thermalTempAt(int imgX, int imgY);            // 取该像素对应的温度值(°C)
    int thermalGrayAt(int imgX, int imgY);               // 取该像素原始灰度值(0~255; 越界/未连接返回 -1)
    QString ensureWritableDir(const QString &preferred) const;   // 选择可写目录(避免录像写入失败)

    // ---- 4K 主码流: 直接在本程序界面内显示(NVDEC 硬解 + GPU 缩放) ----
    void update4kHint(int channel);            // 按所选码流刷新 4K 显示方式提示

    Ui::MainWindow *ui;

    // Modules
    ThermalCamera *m_thermalCamera = nullptr;
    VisibleCamera *m_visibleCamera = nullptr;
    IRLaser *m_irLaser = nullptr;
    HeatSourceDetector *m_heatDetector = nullptr;
    AngleCalculator *m_angleCalculator = nullptr;
    TcpServer *m_tcpServer = nullptr;

    // State
    QImage m_lastThermalFrame;
    QImage m_lastVisibleFrame;
    QMutex m_frameMutex;
    double m_thermalFps = 0;
    double m_visibleFps = 0;
    bool m_thermalConnected = false;
    bool m_visibleConnected = false;

    // Detection timer
    QTimer *m_detectionTimer = nullptr;

    // ---- 新增 UI (代码构建) ----
    QGroupBox *m_groupDetect = nullptr;
    QLabel *m_labelDetectStatus = nullptr;
    QLabel *m_labelTcpAddress = nullptr;
    QComboBox *m_comboDetectMode = nullptr;
    QDoubleSpinBox *m_spinMinTemp = nullptr;
    QSpinBox *m_spinMinArea = nullptr;
    QCheckBox *m_checkGrid = nullptr;
    QPlainTextEdit *m_tcpLogView = nullptr;     // TCP 客户端交互日志(TCP 分组内文本框)
    QPlainTextEdit *m_tcpExampleView = nullptr; // TCP 交互示例(输出格式 + 控制指令)
    QLabel *m_labelThermalRes = nullptr;
    QLabel *m_labelVisibleRes = nullptr;

    // ---- 设备连接状态标签(绿=已连接 / 红=未连接) ----
    QLabel *m_labelStatusThermal = nullptr;     // 状态栏: 热像仪(热源检测)
    QLabel *m_labelStatusVisible = nullptr;     // 状态栏: 可见光
    QLabel *m_labelStatusLaser   = nullptr;     // 状态栏: 红外激光
    QLabel *m_labelStatusTcp     = nullptr;     // 状态栏: TCP 客户端数
    QLabel *m_labelThermalConn   = nullptr;     // 热源检测 Tab 内: 热像仪连接状态
    QLabel *m_labelVisibleConn   = nullptr;     // 可见光 Tab 内: 可见光连接状态
    QPushButton *m_btnLaserRefresh = nullptr;   // 激光状态手动回读
    QPushButton *m_btnLaserSelfTest = nullptr;  // 激光串口自检
    QLabel *m_labelLaserComms = nullptr;        // 激光通信状态(无应答提示)
    QLabel *m_labelLaserAngle = nullptr;        // 光斑角度(出光角度)回读
    QLabel *m_labelLaserBri = nullptr;          // 亮度回读
    QLabel *m_labelLaserSwitch = nullptr;       // 当前激光器开/关状态(应答回读, 绿/灰)
    QTimer *m_laserReplyTimer = nullptr;        // 下发后等待应答的超时定时器
    bool m_laserGotReply = false;               // 最近一次下发是否收到应答
    int m_laserBrightnessRaw = 128;             // 本地亮度档位(0-255)
    QTimer *m_laserStatusTimer = nullptr;       // 激光状态周期回读(3s)
    QTabWidget *m_controlTabs = nullptr;        // 控制面板: 热源检测/可见光控制/红外激光/TCP服务器
    QComboBox *m_comboStream = nullptr;         // 可见光拉取码流(主码流101/第三码流103/子码流102)
    QLabel    *m_label4kHint = nullptr;         // 4K 主码流提示(本界面内直接显示)
    QLabel *m_titleThermal = nullptr;           // 热像仪画面左上角标题
    QLabel *m_titleVisible = nullptr;           // 可见光画面左上角标题

    // ---- 可见光镜头控制: 变焦/光圈数值 + 录像取证 ----
    QLabel *m_labelZoomValue = nullptr;         // 变焦按钮旁: 实际光学倍率 (如 1.0×)
    QLabel *m_labelIrisValue = nullptr;         // 光圈按钮旁: 当前 IrisLevel
    QComboBox *m_comboIrcut = nullptr;          // 日夜转换: 白天(day)/夜晚(night)/自动(auto)
    QVector<QComboBox*> m_imageCombos;          // "图像参数"下拉集合(与下面两行一一对应)
    QStringList m_imageBlocks;                  // 对应 ISAPI 子块名(如 IrcutFilter/BLC/WDR)
    QStringList m_imageTags;                    // 对应 ISAPI 块内标签(如 IrcutFilterType)
    QPushButton *m_btnRecordStart = nullptr;    // 录像取证: 开始
    QPushButton *m_btnRecordStop = nullptr;     // 录像取证: 结束
    QPushButton *m_btnSelectSavePath = nullptr; // 录像取证: 选择保存路径
    QLabel *m_labelSavePath = nullptr;          // 录像保存路径显示
    QLabel *m_labelRecordInfo = nullptr;        // 录像码流与分辨率提示(随码流切换更新)
    QLabel *m_labelDecoderInfo = nullptr;       // 实际解码链路提示(硬解/软解 + 帧率)
    QLabel *m_labelPreviewInfo = nullptr;       // 预览策略提示(拉流/预览/录像 三者分辨率关系)
    // ---- 可见光预览策略 ----
    // 主码流 4K 默认按"4K 原生"预览(分辨率栏显示 3840x2160, 缩放可看 4K 细节);
    // 取消勾选则改由 GPU 缩放到显示区尺寸(满帧 25fps, 更省资源)。录像不受影响, 始终直拷所选码流。
    QCheckBox *m_checkPreviewNative = nullptr;  // "预览 4K 原生"(默认勾选)
    bool m_nativePreview = true;                // 是否按码流原生分辨率(4K)预览
    QSize m_currentPreviewSize{0, 0};           // 下发给采集端的 GPU 缩放输出尺寸(0,0=原生)
    QString m_recordSaveDir;                    // 录像保存目录
    bool m_recording = false;                   // 当前是否正在录像
    int m_currentZoomPos = 100;                 // 最近一次回读的变焦位置(10..230)
    QTimer *m_zoomMonitorTimer = nullptr;       // 变焦位置实时回读(1Hz)
    int m_lastZoomReported = -1;                // 上次显示的变焦位置(用于识别非预期变化)
    qint64 m_lastZoomActionMs = 0;              // 上次用户主动变焦的时间戳

    // ---- 热像仪: 缩放/拖拽 + 鼠标探针(坐标/灰度/温度) ----
    double m_thermalScale = 1.0;                // 热像仪画面缩放系数
    QPoint m_thermalOffset{0, 0};               // 热像仪画面平移
    QPoint m_thermalDragStart;
    bool m_thermalDragging = false;
    QLabel *m_labelProbe = nullptr;             // 显示鼠标坐标 + 灰度 + 温度
    bool m_hasProbe = false;                    // 是否已有有效探针位置
    QPoint m_probeWidgetPos{-1, -1};            // 探针在控件中的位置(用于叠加显示)
    int m_probeImgX = -1;                       // 探针在图像中的坐标
    int m_probeImgY = -1;
    int m_probeGray = -1;                       // 探针处原始灰度值(0~255)
    double m_probeTemp = 0.0;                   // 探针处温度(°C)
    qint64 m_lastProbeRenderMs = 0;             // 探针重绘节流(鼠标移动会高频触发)

    QSize m_lastThermalResSize;                 // 分辨率标签节流
    QSize m_lastVisibleResSize;

    // ---- 热像仪叠加状态 ----
    bool m_gridEnabled = true;
    bool m_targetFound = false;
    HeatSource m_target = {};              // 当前选中的热源目标
    QVector<HeatSource> m_lastSources;     // 最近一次检测到的所有热源
    qint64 m_lastThermalRenderMs = 0;      // 热像仪重绘节流(时间戳)
    qint64 m_lastVisibleRenderMs = 0;      // 可见光重绘节流(时间戳)
    QVector<float> m_tempData;             // 检测用临时温度缓冲(复用, 避免每帧 new/delete)
    int m_lastLogTargetX = -99999;         // 上次记录日志的目标位置(避免刷屏)
    int m_lastLogTargetY = -99999;
    qint64 m_lastTargetLogMs = 0;

    // ---- 可见光缩放/拖拽状态 ----
    double m_visibleScale = 1.0;
    QPoint m_visibleOffset{0, 0};
    QPoint m_visibleDragStart;
    bool m_visibleDragging = false;
};

#endif // MAINWINDOW_H
