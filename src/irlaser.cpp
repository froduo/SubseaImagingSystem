#include "irlaser.h"
#include "logger.h"
#include <QDateTime>
#include <QThread>

// ============================================================
// IR Laser Module - IR4W850B-T01
// Protocol: Pelco_D (custom extension)
// Frame format: FF addr cmd1 cmd2 data1 data2 checksum
// checksum = (addr + cmd1 + cmd2 + data1 + data2) & 0xFF
//
// Command table (from hardware documentation):
//   开启激光器:     cmd1=0x01, cmd2=0x01, data1=1,   data2=0
//   关闭激光器:     cmd1=0x01, cmd2=0x01, data1=0,   data2=0
//   调节亮度增大:   cmd1=0x01, cmd2=0x02, data1=0,   data2=0
//   调节亮度减小:   cmd1=0x01, cmd2=0x02, data1=1,   data2=0
//   设置亮度:       cmd1=0x01, cmd2=0x03, data1=亮度(0-255), data2=0
//   调节角度减小:   cmd1=0x01, cmd2=0x04, data1=0,   data2=步幅
//   调节角度增大:   cmd1=0x01, cmd2=0x04, data1=1,   data2=步幅
//   设置角度位置:   cmd1=0x01, cmd2=0x05, data1-2=位置(0x0000-0x4000)
//   电机复位:       cmd1=0x01, cmd2=0x06, data1=0,   data2=0
//   设置角度(度):   cmd1=0x08, cmd2=0x01, data1-2=角度*100
//   变倍+:          cmd1=0x00, cmd2=0x20, data1=0,   data2=速度(0=默认)
//   变倍-:          cmd1=0x00, cmd2=0x40, data1=0,   data2=速度
//   停止:           cmd1=0x00, cmd2=0x00, data1=0,   data2=0
//   查询亮度:       cmd1=0x02, cmd2=0x03, data1=0,   data2=0
//   查询角度:       cmd1=0x09, cmd2=0x01, data1=0,   data2=0
// ============================================================

IRLaser::IRLaser(QObject *parent)
    : QObject(parent)
{
    m_serial = new QSerialPort(this);

    connect(m_serial, &QSerialPort::readyRead, this, [this]() {
        const QByteArray data = m_serial->readAll();
        if (data.isEmpty()) return;
        m_rxBuffer.append(data);
        LOG_DEBUG(QString("IR Laser RX 原始字节: %1").arg(QString(data.toHex(' '))));
        processRxBuffer();
    });

    connect(m_serial, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error != QSerialPort::NoError) {
            QString errStr = m_serial->errorString();
            LOG_ERROR(QString("Serial port error: %1").arg(errStr));
            emit errorOccurred(errStr);
        }
    });
}

IRLaser::~IRLaser()
{
    close();
}

bool IRLaser::open(const QString &portName, int baudRate)
{
    if (m_serial->isOpen()) {
        m_serial->close();
    }

    m_serial->setPortName(portName);
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        LOG_ERROR(QString("Failed to open serial port: %1 - %2")
                      .arg(portName, m_serial->errorString()));
        emit errorOccurred(m_serial->errorString());
        emit connectionChanged(false);
        return false;
    }

    LOG_INFO(QString("IR Laser serial port opened: %1 @ %2bps")
                 .arg(portName).arg(baudRate));
    emit connectionChanged(true);
    return true;
}

void IRLaser::close()
{
    // 仅在确实处于打开状态时才关闭并通知。
    // 原实现无论是否打开都会 emit connectionChanged(false),
    // 在 MainWindow 析构(ui 已 delete)阶段会被回调访问已销毁的控件, 导致退出崩溃。
    if (!m_serial->isOpen()) return;

    m_serial->close();
    LOG_INFO("IR Laser serial port closed");
    emit connectionChanged(false);
}

bool IRLaser::isOpen() const
{
    return m_serial->isOpen();
}

QByteArray IRLaser::buildPelcoD(quint8 cmd1, quint8 cmd2, quint8 data1, quint8 data2)
{
    QByteArray frame;
    frame.append(static_cast<char>(0xFF));           // Sync byte
    frame.append(static_cast<char>(m_address));      // Address
    frame.append(static_cast<char>(cmd1));           // Command 1
    frame.append(static_cast<char>(cmd2));           // Command 2
    frame.append(static_cast<char>(data1));          // Data 1
    frame.append(static_cast<char>(data2));          // Data 2

    // Checksum = (address + cmd1 + cmd2 + data1 + data2) & 0xFF
    quint8 checksum = static_cast<quint8>(
        (m_address + cmd1 + cmd2 + data1 + data2) & 0xFF);
    frame.append(static_cast<char>(checksum));

    return frame;
}

void IRLaser::sendCommand(quint8 cmd1, quint8 cmd2, quint8 data1, quint8 data2)
{
    if (!m_serial->isOpen()) {
        LOG_WARN("Cannot send command: serial port not open");
        return;
    }

    QByteArray cmd = buildPelcoD(cmd1, cmd2, data1, data2);
    m_lastTxFrame = cmd;

    // ---- RS-485 半双工方向控制 ----
    // 绿联等"USB转RS-485"适配器多为 FT232R + 485 收发器, 由 RTS 控制 DE/RE。
    // 若不操作 RTS, 收发器长期处于接收态: 数据实际没有发上总线, 因此永远收不到应答。
    const bool txLevel   = !m_rs485RtsActiveLow;
    const bool idleLevel =  m_rs485RtsActiveLow;
    if (m_rs485Rts) m_serial->setRequestToSend(txLevel);

    qint64 written = m_serial->write(cmd);
    m_serial->waitForBytesWritten(100);
    if (m_rs485Rts) {
        m_serial->flush();                       // 等待字节真正移出(等价 tcdrain)
        m_serial->setRequestToSend(idleLevel);   // 切回收态, 准备接收应答
    }
    m_lastTxEndMs = QDateTime::currentMSecsSinceEpoch();

    LOG_DEBUG(QString("Pelco_D TX [%1]: %2")
                  .arg(written)
                  .arg(QString(cmd.toHex(' '))));
}

// ============================================================
// 变倍控制
// ============================================================

void IRLaser::zoomIn()
{
    // 变倍+: cmd1=0x00, cmd2=0x20, data1=0, data2=速度(0=默认)
    sendCommand(0x00, 0x20, 0x00, 0x00);
    LOG_INFO("IR Laser: Zoom In (Tele) - cmd1=0x00 cmd2=0x20");
}

void IRLaser::zoomOut()
{
    // 变倍-: cmd1=0x00, cmd2=0x40, data1=0, data2=速度(0=默认)
    sendCommand(0x00, 0x40, 0x00, 0x00);
    LOG_INFO("IR Laser: Zoom Out (Wide) - cmd1=0x00 cmd2=0x40");
}

void IRLaser::zoomStop()
{
    // 停止: cmd1=0x00, cmd2=0x00, data1=0, data2=0
    sendCommand(0x00, 0x00, 0x00, 0x00);
    LOG_INFO("IR Laser: Zoom Stop");
}

void IRLaser::setZoomPosition(int pos)
{
    // pos: 0-100, map to angle position 0x0000-0x4000
    // 使用设置角度位置(行程): cmd1=0x01, cmd2=0x05, data1-2=位置(0x0000-0x4000)
    pos = qBound(0, pos, 100);
    quint16 position = static_cast<quint16>(pos * 0x4000 / 100);
    quint8 data1 = static_cast<quint8>((position >> 8) & 0xFF);
    quint8 data2 = static_cast<quint8>(position & 0xFF);
    sendCommand(0x01, 0x05, data1, data2);
    m_currentZoom = pos;
    emit zoomChanged(pos);
    LOG_INFO(QString("IR Laser: 设置光斑/角度行程 滑条%1 -> 行程0x%2")
                 .arg(pos).arg(position, 4, 16, QChar('0')));
    // 250ms 后回读实际行程位置, 验证指令是否被执行
    QTimer::singleShot(250, this, [this]() { queryPosition(); });
}

// ============================================================
// 亮度控制
// ============================================================

void IRLaser::brightnessUp()
{
    // 调节亮度增大: cmd1=0x01, cmd2=0x02, data1=0, data2=0
    sendCommand(0x01, 0x02, 0x00, 0x00);
    LOG_INFO("IR Laser: Brightness Up - cmd1=0x01 cmd2=0x02 data1=0");
}

void IRLaser::brightnessDown()
{
    // 调节亮度减小: cmd1=0x01, cmd2=0x02, data1=1, data2=0
    sendCommand(0x01, 0x02, 0x01, 0x00);
    LOG_INFO("IR Laser: Brightness Down - cmd1=0x01 cmd2=0x02 data1=1");
}

void IRLaser::brightnessStop()
{
    // 停止: cmd1=0x00, cmd2=0x00, data1=0, data2=0
    sendCommand(0x00, 0x00, 0x00, 0x00);
    LOG_INFO("IR Laser: Brightness Stop");
}

void IRLaser::setBrightness(int level)
{
    // 设置亮度: cmd1=0x01, cmd2=0x03, data1=亮度值(0-255), data2=0
    level = qBound(0, level, 100);
    quint8 brightnessValue = static_cast<quint8>(level * 255 / 100);
    sendCommand(0x01, 0x03, brightnessValue, 0x00);
    m_currentBrightness = level;
    emit brightnessChanged(level);
    LOG_INFO(QString("IR Laser: 设置亮度 滑条%1 -> 原始值%2").arg(level).arg(brightnessValue));
    // 250ms 后回读实际亮度
    QTimer::singleShot(250, this, [this]() { queryBrightness(); });
}

// ============================================================
// 角度控制
// ============================================================

void IRLaser::focusNear()
{
    // 调节角度减小: cmd1=0x01, cmd2=0x04, data1=0, data2=步幅(0=默认)
    sendCommand(0x01, 0x04, 0x00, 0x00);
    LOG_INFO("IR Laser: Angle decrease (focus near) - cmd1=0x01 cmd2=0x04 data1=0");
}

void IRLaser::focusFar()
{
    // 调节角度增大: cmd1=0x01, cmd2=0x04, data1=1, data2=步幅(0=默认)
    sendCommand(0x01, 0x04, 0x01, 0x00);
    LOG_INFO("IR Laser: Angle increase (focus far) - cmd1=0x01 cmd2=0x04 data1=1");
}

void IRLaser::focusStop()
{
    // 停止: cmd1=0x00, cmd2=0x00, data1=0, data2=0
    sendCommand(0x00, 0x00, 0x00, 0x00);
    LOG_INFO("IR Laser: Angle stop");
}

void IRLaser::setPreset(int presetId)
{
    Q_UNUSED(presetId);
    LOG_WARN(QString("IR Laser: setPreset(%1) - not supported by IR4W850B-T01 hardware").arg(presetId));
}

void IRLaser::callPreset(int presetId)
{
    Q_UNUSED(presetId);
    LOG_WARN(QString("IR Laser: callPreset(%1) - not supported by IR4W850B-T01 hardware").arg(presetId));
}

void IRLaser::setAutoDimming(bool enable)
{
    Q_UNUSED(enable);
    LOG_WARN(QString("IR Laser: setAutoDimming(%1) - not supported by IR4W850B-T01 hardware")
                 .arg(enable ? "true" : "false"));
}

// ============================================================
// 依据《激光器控制协议》补全的指令
// ============================================================
void IRLaser::laserSwitch(bool on)
{
    // 开启/关闭激光器: cmd1=0x01, cmd2=0x01, data1=p(0关/1开), data2=0
    sendCommand(0x01, 0x01, on ? 0x01 : 0x00, 0x00);
    LOG_INFO(QString("IR Laser: 激光器%1 (cmd1=0x01 cmd2=0x01)")
                 .arg(on ? "开启" : "关闭"));
}

void IRLaser::angleStep(int dir)
{
    // 调节出光角度: cmd1=0x01, cmd2=0x04, data1=p(0减小/1增大), data2=幅度(0=默认步进)
    sendCommand(0x01, 0x04, dir > 0 ? 0x01 : 0x00, 0x00);
    LOG_INFO(QString("IR Laser: 出光角度%1 (cmd1=0x01 cmd2=0x04)")
                 .arg(dir > 0 ? "增大" : "减小"));
    QTimer::singleShot(200, this, [this]() { queryAngle(); });
}

void IRLaser::setAngleDeg(double deg)
{
    // 设置出光角度: cmd1=0x08, cmd2=0x01, data1-2=角度*100 (单位0.01度)
    int raw = qBound(0, qRound(deg * 100.0), 0xFFFF);
    sendCommand(0x08, 0x01, static_cast<quint8>((raw >> 8) & 0xFF),
                static_cast<quint8>(raw & 0xFF));
    LOG_INFO(QString("IR Laser: 设置出光角度 %1° (raw=%2)").arg(deg, 0, 'f', 2).arg(raw));
    QTimer::singleShot(300, this, [this]() { queryAngle(); });
}

void IRLaser::motorReset()
{
    // 电机复位: cmd1=0x01, cmd2=0x06, data1=0, data2=0
    sendCommand(0x01, 0x06, 0x00, 0x00);
    LOG_INFO("IR Laser: 电机复位 (行程初始化, 停在最大出光角度)");
}

void IRLaser::selfTest()
{
    if (!m_serial->isOpen()) {
        LOG_ERROR("IR Laser: 串口未打开, 无法自检");
        emit errorOccurred(QStringLiteral("串口未打开"));
        emit selfTestFinished(0, 0);
        return;
    }

    LOG_INFO(QString("IR Laser: === 串口自检开始 === 端口=%1 波特率=%2 RS485_RTS=%3 地址=%4")
                 .arg(m_serial->portName()).arg(m_serial->baudRate())
                 .arg(m_rs485Rts ? (m_rs485RtsActiveLow ? "on(反相)" : "on") : "off")
                 .arg(m_address));

    struct Q { quint8 c1; quint8 c2; const char *name; };
    const Q qs[] = { {0x02,0x01,"开关"}, {0x02,0x03,"亮度"}, {0x02,0x05,"行程位置"},
                     {0x02,0x0F,"风扇"}, {0x09,0x01,"出光角度"} };

    int tx = 0, rx = 0;
    for (const Q &q : qs) {
        m_rxBuffer.clear();
        sendCommand(q.c1, q.c2, 0x00, 0x00);
        ++tx;

        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        bool got = false;
        while (QDateTime::currentMSecsSinceEpoch() - t0 < 300) {
            if (m_serial->waitForReadyRead(60)) {
                const QByteArray d = m_serial->readAll();
                if (!d.isEmpty()) {
                    ++rx;
                    got = true;
                    LOG_INFO(QString("  自检[%1] 应答: %2").arg(q.name, QString(d.toHex(' '))));
                    m_rxBuffer.append(d);
                    processRxBuffer();
                }
                break;
            }
        }
        if (!got) LOG_WARN(QString("  自检[%1] 无应答").arg(q.name));
        QThread::msleep(50);
    }

    if (rx == 0) {
        LOG_ERROR("IR Laser: 自检结束 —— 全部无应答。请检查: "
                  "①RS-485 A/B(T/R+ / T/R-)是否接反 ②适配器是否处于 RS-422 四线模式"
                  "③波特率/地址是否与激光器一致 ④激光器是否上电");
    } else {
        LOG_INFO(QString("IR Laser: 自检结束, 发出 %1 帧, 收到 %2 次应答").arg(tx).arg(rx));
    }
    emit selfTestFinished(tx, rx);
}

// ============================================================
// 状态回读: 依据《激光器控制协议》
//   应答帧格式与发送帧一致: FF addr cmd1 cmd2 data1 data2 sum
//   sum = (addr + cmd1 + cmd2 + data1 + data2) & 0xFF
// ============================================================
void IRLaser::processRxBuffer()
{
    // 帧长固定 7 字节, 以 0xFF 为帧头; 逐字节搜索并校验校验和
    while (m_rxBuffer.size() >= 7) {
        const int head = m_rxBuffer.indexOf(static_cast<char>(0xFF));
        if (head < 0) {
            LOG_DEBUG(QString("IR Laser RX: 丢弃无帧头数据 %1 字节")
                          .arg(m_rxBuffer.size()));
            m_rxBuffer.clear();
            return;
        }
        if (head > 0) {
            LOG_DEBUG(QString("IR Laser RX: 跳过帧头前 %1 字节").arg(head));
            m_rxBuffer.remove(0, head);
        }
        if (m_rxBuffer.size() < 7) return;

        QByteArray frame = m_rxBuffer.left(7);
        const quint8 addr = static_cast<quint8>(frame.at(1));
        const quint8 c1 = static_cast<quint8>(frame.at(2));
        const quint8 c2 = static_cast<quint8>(frame.at(3));
        const quint8 d1 = static_cast<quint8>(frame.at(4));
        const quint8 d2 = static_cast<quint8>(frame.at(5));
        const quint8 sum = static_cast<quint8>(frame.at(6));
        const quint8 expect = static_cast<quint8>((addr + c1 + c2 + d1 + d2) & 0xFF);

        if (sum != expect) {
            LOG_DEBUG(QString("IR Laser RX: 校验和错误 (收到 %1, 期望 %2), 丢弃 1 字节")
                          .arg(sum, 2, 16, QChar('0')).arg(expect, 2, 16, QChar('0')));
            m_rxBuffer.remove(0, 1);   // 可能是帧头误判, 跳过一个字节继续找
            continue;
        }

        // ---- 过滤半双工本地回显(带时间窗) ----
        // 注意: 部分应答与查询帧完全相同(如"开关=关"应答 02 01 00 00 与查询帧一致),
        // 故不能仅凭"内容相同"丢弃; 本地回显几乎紧跟在发送结束之后, 因此只丢弃
        // 发送结束后 10ms 内到达的同内容帧, 更晚到达的一律视为真实应答。
        const qint64 sinceTx = QDateTime::currentMSecsSinceEpoch() - m_lastTxEndMs;
        if (!m_lastTxFrame.isEmpty() && frame == m_lastTxFrame && sinceTx <= 10) {
            LOG_DEBUG(QString("IR Laser RX: 忽略本地回显帧(发送后 %1ms) %2")
                          .arg(sinceTx).arg(QString(frame.toHex(' '))));
            m_rxBuffer.remove(0, 7);
            continue;
        }

        m_rxBuffer.remove(0, 7);
        m_hasFeedback = true;
        m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
        m_rxFrameCount++;
        emit rawFrameReceived(QString(frame.toHex(' ')));
        handleFrame(frame);
    }
}

void IRLaser::handleFrame(const QByteArray &frame)
{
    const quint8 c1 = static_cast<quint8>(frame.at(2));
    const quint8 c2 = static_cast<quint8>(frame.at(3));
    const quint8 d1 = static_cast<quint8>(frame.at(4));
    const quint8 d2 = static_cast<quint8>(frame.at(5));
    const QString hex = QString(frame.toHex(' '));

    if (c1 == 0x02 && c2 == 0x01) {
        m_laserOn = (d1 != 0);
        LOG_INFO(QString("激光器回读[开关]: %1   (原始帧 %2)")
                     .arg(m_laserOn ? "开启" : "关闭").arg(hex));
        emit laserSwitchChanged(m_laserOn);
    } else if (c1 == 0x02 && c2 == 0x03) {
        m_brightnessRaw = d1;
        LOG_INFO(QString("激光器回读[亮度]: %1/255   (原始帧 %2)").arg(d1).arg(hex));
        emit brightnessValueChanged(d1);
    } else if (c1 == 0x02 && c2 == 0x05) {
        m_positionRaw = (d1 << 8) | d2;
        LOG_INFO(QString("激光器回读[行程位置]: 0x%1 (%2/16384)   (原始帧 %3)")
                     .arg(m_positionRaw, 4, 16, QChar('0')).arg(m_positionRaw).arg(hex));
        emit positionValueChanged(m_positionRaw);
    } else if (c1 == 0x09 && c2 == 0x01) {
        m_angleDeg = ((d1 << 8) | d2) / 100.0;
        LOG_INFO(QString("激光器回读[出光角度]: %1°   (原始帧 %2)")
                     .arg(m_angleDeg, 0, 'f', 2).arg(hex));
        emit angleValueChanged(m_angleDeg);
    } else if (c1 == 0x02 && c2 == 0x0F) {
        m_fanOn = (d1 != 0);
        LOG_INFO(QString("激光器回读[风扇]: %1   (原始帧 %2)")
                     .arg(m_fanOn ? "开启" : "关闭").arg(hex));
        emit fanChanged(m_fanOn);
    } else if (c1 == 0x20 || c1 == 0x21 || c1 == 0x22) {
        LOG_INFO(QString("激光器回读[版本/型号]: c1=0x%1 d1=%2 d2=%3   (原始帧 %4)")
                     .arg(c1, 2, 16, QChar('0')).arg(d1).arg(d2).arg(hex));
    } else {
        LOG_INFO(QString("激光器应答(未解析): %1").arg(hex));
    }
    emit statusReceived(hex);
}

void IRLaser::queryVersion()     { sendCommand(0x05, 0x10, 0x01, 0x01); }

void IRLaser::setDeviceAddress(int addr)
{
    // 修改通讯地址: cmd1=0x03, cmd2=0x11, data1=0x00, data2=地址(1~254)
    addr = qBound(1, addr, 254);
    sendCommand(0x03, 0x11, 0x00, static_cast<quint8>(addr));
    m_address = addr;
    LOG_INFO(QString("IR Laser: 修改通讯地址 -> %1 (此后需按新地址通信)").arg(addr));
}

void IRLaser::setDeviceBaudRate(int index)
{
    // 修改通讯波特率: cmd1=0x03, cmd2=0x13, data1=0x00, data2=索引(0:1200 ... 7:115200)
    index = qBound(0, index, 7);
    sendCommand(0x03, 0x13, 0x00, static_cast<quint8>(index));
    LOG_INFO(QString("IR Laser: 修改通讯波特率索引 -> %1 (3=9600)").arg(index));
}

void IRLaser::queryLaserSwitch() { sendCommand(0x02, 0x01, 0x00, 0x00); }
void IRLaser::queryBrightness()  { sendCommand(0x02, 0x03, 0x00, 0x00); }
void IRLaser::queryPosition()    { sendCommand(0x02, 0x05, 0x00, 0x00); }
void IRLaser::queryAngle()       { sendCommand(0x09, 0x01, 0x00, 0x00); }
void IRLaser::queryFan()         { sendCommand(0x02, 0x0F, 0x00, 0x00); }

void IRLaser::queryStatus()
{
    if (!m_serial->isOpen()) {
        LOG_WARN("IR Laser: 串口未打开, 无法回读状态");
        return;
    }
    LOG_INFO("IR Laser: 开始状态回读 (开关/亮度/行程位置/出光角度/风扇)");

    // 依次下发查询命令, 间隔 120ms 避免丢帧
    QTimer::singleShot(0,   this, [this]() { queryLaserSwitch(); });
    QTimer::singleShot(120, this, [this]() { queryBrightness(); });
    QTimer::singleShot(240, this, [this]() { queryPosition(); });
    QTimer::singleShot(360, this, [this]() { queryAngle(); });
    QTimer::singleShot(480, this, [this]() { queryFan(); });
}
