#include "configmanager.h"
#include "logger.h"
#include <QFileInfo>
#include <QDir>

ConfigManager& ConfigManager::instance()
{
    static ConfigManager inst;
    return inst;
}

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
{
}

bool ConfigManager::loadConfig(const QString &filePath)
{
    QMutexLocker locker(&m_mutex);
    m_filePath = filePath;

    if (!QFileInfo::exists(filePath)) {
        LOG_WARN(QString("Config file not found: %1, creating default").arg(filePath));
        QDir dir = QFileInfo(filePath).absoluteDir();
        if (!dir.exists()) {
            dir.mkpath(".");
        }
    }

    if (m_settings) {
        delete m_settings;
    }
    m_settings = new QSettings(filePath, QSettings::IniFormat, this);

    LOG_INFO(QString("Config loaded from: %1").arg(filePath));
    emit configChanged();
    return true;
}

bool ConfigManager::saveConfig(const QString &filePath)
{
    QMutexLocker locker(&m_mutex);
    QString path = filePath.isEmpty() ? m_filePath : filePath;
    if (m_settings) {
        m_settings->sync();
        LOG_INFO(QString("Config saved to: %1").arg(path));
        return true;
    }
    return false;
}

// Network
// 热像仪 HM-TD2069N-18D (声波成像仪)
QString ConfigManager::thermalCameraIp() const   { return getValue("network/thermal_camera_ip", "192.168.10.211").toString(); }
int ConfigManager::thermalCameraPort() const      { return getValue("network/thermal_camera_port", 80).toInt(); }
QString ConfigManager::thermalCameraUser() const  { return getValue("network/thermal_camera_user", "admin").toString(); }
QString ConfigManager::thermalCameraPass() const  { return getValue("network/thermal_camera_pass", "cisdi135").toString(); }
QString ConfigManager::thermalRtspUrl() const     { return getValue("thermal/rtsp_url", "rtsp://admin:cisdi135@192.168.10.211:554/Streaming/Channels/101").toString(); }

// 可见光一体化摄像机 iDS-2ZMN2312S
QString ConfigManager::visibleCameraIp() const   { return getValue("network/visible_camera_ip", "192.168.10.212").toString(); }
int ConfigManager::visibleCameraPort() const      { return getValue("network/visible_camera_port", 80).toInt(); }
QString ConfigManager::visibleCameraUser() const  { return getValue("network/visible_camera_user", "admin").toString(); }
QString ConfigManager::visibleCameraPass() const  { return getValue("network/visible_camera_pass", "cisdi135").toString(); }
QString ConfigManager::visibleRtspUrl() const     { return getValue("visible/rtsp_url", "rtsp://admin:cisdi135@192.168.10.212:554/Streaming/Channels/103").toString(); }
// 录像取证默认使用主码流(4K): 录像为 -c:v copy 直拷, 不解码, 因此不占用本机解码能力
QString ConfigManager::visibleRecordRtspUrl() const
{
    return getValue("visible/record_rtsp_url",
                    "rtsp://admin:cisdi135@192.168.10.212:554/Streaming/Channels/101").toString();
}

int ConfigManager::tcpServerPort() const          { return getValue("network/tcp_server_port", 8888).toInt(); }

// Serial
QString ConfigManager::serialPortName() const     { return getValue("serial/port_name", "/dev/ttyUSB0").toString(); }
int ConfigManager::serialBaudRate() const         { return getValue("serial/baud_rate", 9600).toInt(); }
int ConfigManager::serialDataBits() const         { return getValue("serial/data_bits", 8).toInt(); }
QString ConfigManager::serialParity() const       { return getValue("serial/parity", "none").toString(); }
int ConfigManager::serialStopBits() const         { return getValue("serial/stop_bits", 1).toInt(); }
bool ConfigManager::serialRs485Rts() const        { return getValue("serial/rs485_rts", true).toBool(); }
bool ConfigManager::serialRs485RtsActiveLow() const { return getValue("serial/rs485_rts_active_low", false).toBool(); }

// Detection
double ConfigManager::tempThresholdDelta() const  { return getValue("detection/temp_threshold_delta", 10.0).toDouble(); }
int ConfigManager::minArea() const                { return getValue("detection/min_area", 50).toInt(); }
int ConfigManager::maxSources() const             { return getValue("detection/max_sources", 5).toInt(); }
bool ConfigManager::detectionEnabled() const      { return getValue("detection/enable_detection", true).toBool(); }

// Camera
// 默认取证目录放在当前用户主目录下(避免默认写到其他用户 Home 而不可写导致录像失败)
QString ConfigManager::savePath() const           { return getValue("camera/save_path", QDir::homePath() + "/evidence").toString(); }
QString ConfigManager::imageFormat() const        { return getValue("camera/image_format", "jpg").toString(); }
int ConfigManager::imageQuality() const           { return getValue("camera/image_quality", 95).toInt(); }

// Thermal FOV
double ConfigManager::hfov() const                { return getValue("thermal/hfov", 24.2).toDouble(); }
double ConfigManager::vfov() const                { return getValue("thermal/vfov", 19.4).toDouble(); }
int ConfigManager::imageWidth() const             { return getValue("thermal/image_width", 640).toInt(); }
int ConfigManager::imageHeight() const            { return getValue("thermal/image_height", 512).toInt(); }

// Laser
int ConfigManager::defaultZoom() const            { return getValue("laser/default_zoom", 30).toInt(); }
int ConfigManager::defaultBrightness() const      { return getValue("laser/default_brightness", 80).toInt(); }
bool ConfigManager::laserAutoMode() const         { return getValue("laser/auto_mode", true).toBool(); }

QVariant ConfigManager::getValue(const QString &key, const QVariant &defaultValue) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return defaultValue;
    QVariant val = m_settings->value(key);
    return val.isValid() ? val : defaultValue;
}

void ConfigManager::setValue(const QString &key, const QVariant &value)
{
    QMutexLocker locker(&m_mutex);
    if (m_settings) {
        m_settings->setValue(key, value);
    }
}
