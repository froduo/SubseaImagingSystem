#ifndef IRLASER_H
#define IRLASER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QTimer>

class IRLaser : public QObject
{
    Q_OBJECT

public:
    explicit IRLaser(QObject *parent = nullptr);
    ~IRLaser();

    // 连接/断开
    bool open(const QString &portName, int baudRate = 9600);
    void close();
    bool isOpen() const;

    // 串口参数
    void setAddress(int addr) { m_address = qBound(1, addr, 254); }
    int  address() const { return m_address; }
    // RS-485 方向控制: FT232 类"USB转RS-485"适配器用 RTS 控制收发方向,
    // 若不操作 RTS 则收发器常处于接收态 -> 数据发不出去且无任何应答。
    void setRs485RtsEnabled(bool on) { m_rs485Rts = on; }
    bool rs485RtsEnabled() const { return m_rs485Rts; }
    void setRs485RtsActiveLow(bool on) { m_rs485RtsActiveLow = on; }
    bool rs485RtsActiveLow() const { return m_rs485RtsActiveLow; }

    // 变焦控制 (光斑大小, 变倍 0x00/0x20|0x40)
    void zoomIn();               // 变倍+（光斑变小）
    void zoomOut();              // 变倍-（光斑变大）
    void zoomStop();             // 停止变倍
    void setZoomPosition(int pos); // 设置出光角度行程位置 (0-100) — 见协议 0x01,0x05

    // 激光器开关 (0x01,0x01)
    void laserSwitch(bool on);

    // 出光角度步进/绝对设置 (0x01,0x04 / 0x08,0x01)
    void angleStep(int dir);         // dir>0 增大角度
    void setAngleDeg(double deg);    // 单位 0.01 度

    // 电机复位 (0x01,0x06)
    void motorReset();

    // 串口自检: 依次下发查询指令并统计应答, 用于排查接线/波特率
    void selfTest();
    int rxFrameCount() const { return m_rxFrameCount; }
    qint64 lastRxMs() const { return m_lastRxMs; }

    // 亮度控制
    void brightnessUp();
    void brightnessDown();
    void brightnessStop();
    void setBrightness(int level);  // 0-100

    // 聚焦控制
    void focusNear();
    void focusFar();
    void focusStop();

    // 预置位
    void setPreset(int presetId);
    void callPreset(int presetId);

    // 自动调光
    void setAutoDimming(bool enable);

    // 状态查询(依据《激光器控制协议》查询指令, 应答帧格式与发送帧一致: FF addr c1 c2 d1 d2 sum)
    void queryStatus();          // 依次查询: 开关/亮度/行程位置/出光角度/风扇
    void queryVersion();         // cmd1=0x05 cmd2=0x10 (程序版本号)
    // 设备参数修改(慎用: 改完需按新参数重新连接)
    void setDeviceAddress(int addr);   // cmd1=0x03 cmd2=0x11 (1~254)
    void setDeviceBaudRate(int index); // cmd1=0x03 cmd2=0x13 (0:1200 .. 3:9600 .. 7:115200)
    void queryLaserSwitch();     // cmd1=0x02 cmd2=0x01
    void queryBrightness();      // cmd1=0x02 cmd2=0x03
    void queryPosition();        // cmd1=0x02 cmd2=0x05 (行程位置 0x0000~0x4000)
    void queryAngle();           // cmd1=0x09 cmd2=0x01 (出光角度, 单位0.01度)
    void queryFan();             // cmd1=0x02 cmd2=0x0F

    // 最近一次回读值
    bool laserOn() const { return m_laserOn; }
    bool fanOn() const { return m_fanOn; }
    int brightnessRaw() const { return m_brightnessRaw; }   // 0~255
    int positionRaw() const { return m_positionRaw; }       // 0~0x4000
    double angleDeg() const { return m_angleDeg; }          // 度
    bool hasFeedback() const { return m_hasFeedback; }

signals:
    void connectionChanged(bool connected);
    void statusReceived(const QString &status);
    void errorOccurred(const QString &error);
    void zoomChanged(int position);
    void brightnessChanged(int level);
    // 设备回读(应答)信号
    void laserSwitchChanged(bool on);
    void fanChanged(bool on);
    void brightnessValueChanged(int raw);     // 0~255
    void positionValueChanged(int raw);       // 0~0x4000
    void angleValueChanged(double deg);       // 度
    void rawFrameReceived(const QString &hex);
    void selfTestFinished(int txCount, int rxCount);

private:
    // Pelco_D 协议帧构建
    QByteArray buildPelcoD(quint8 cmd1, quint8 cmd2, quint8 data1, quint8 data2);
    void sendCommand(quint8 cmd1, quint8 cmd2, quint8 data1, quint8 data2);
    void processRxBuffer();                       // 从接收缓冲中提取完整帧
    void handleFrame(const QByteArray &frame);    // 解析应答帧并发出信号

    QSerialPort *m_serial = nullptr;
    int m_address = 0x01;
    int m_currentZoom = 0;
    int m_currentBrightness = 0;

    QByteArray m_rxBuffer;
    bool m_laserOn = false;
    bool m_fanOn = false;
    int m_brightnessRaw = -1;
    int m_positionRaw = -1;
    double m_angleDeg = -1.0;
    bool m_hasFeedback = false;

    bool m_rs485Rts = true;           // 发送时拉 RTS 控制 RS-485 方向
    bool m_rs485RtsActiveLow = false; // 部分电路反相
    QByteArray m_lastTxFrame;         // 用于过滤半双工回显
    qint64 m_lastTxEndMs = 0;         // 本次发送结束的时刻(判定回显的时间窗)
    qint64 m_lastRxMs = 0;            // 最近一次收到有效帧的时间
    int m_rxFrameCount = 0;           // 累计收到的有效帧数
};

#endif // IRLASER_H
