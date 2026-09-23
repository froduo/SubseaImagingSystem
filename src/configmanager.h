#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <QMutex>

class ConfigManager : public QObject
{
    Q_OBJECT

public:
    static ConfigManager& instance();

    bool loadConfig(const QString &filePath);
    bool saveConfig(const QString &filePath = QString());

    // Network settings
    QString thermalCameraIp() const;
    int thermalCameraPort() const;
    QString thermalCameraUser() const;
    QString thermalCameraPass() const;
    QString thermalRtspUrl() const;

    QString visibleCameraIp() const;
    int visibleCameraPort() const;
    QString visibleCameraUser() const;
    QString visibleCameraPass() const;
    QString visibleRtspUrl() const;
    // 录像取证专用码流(默认主码流 4K): 预览用低码流保证流畅, 取证/录像用主码流保证清晰度
    QString visibleRecordRtspUrl() const;

    int tcpServerPort() const;

    // Serial settings
    QString serialPortName() const;
    int serialBaudRate() const;
    int serialDataBits() const;
    QString serialParity() const;
    int serialStopBits() const;
    // RS-485 方向控制(FT232 类"USB转RS-485"适配器用 RTS 控制收发方向)
    bool serialRs485Rts() const;
    bool serialRs485RtsActiveLow() const;

    // Detection settings
    double tempThresholdDelta() const;
    int minArea() const;
    int maxSources() const;
    bool detectionEnabled() const;

    // Camera settings
    QString savePath() const;
    QString imageFormat() const;
    int imageQuality() const;

    // Thermal FOV settings
    double hfov() const;
    double vfov() const;
    int imageWidth() const;
    int imageHeight() const;

    // Laser settings
    int defaultZoom() const;
    int defaultBrightness() const;
    bool laserAutoMode() const;

    // Generic get/set
    QVariant getValue(const QString &key, const QVariant &defaultValue = QVariant()) const;
    void setValue(const QString &key, const QVariant &value);

signals:
    void configChanged();

private:
    ConfigManager(QObject *parent = nullptr);
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    mutable QMutex m_mutex;
    QSettings *m_settings = nullptr;
    QString m_filePath;
};

#endif // CONFIGMANAGER_H
