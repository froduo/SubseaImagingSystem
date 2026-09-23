#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QDateTime>

class Logger : public QObject
{
    Q_OBJECT

public:
    enum Level {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3
    };

    static Logger& instance();

    void setLogFile(const QString &path);
    void setLogLevel(Level level);
    void log(Level level, const QString &module, const QString &message);

    static void debug(const QString &module, const QString &msg) { instance().log(Debug, module, msg); }
    static void info(const QString &module, const QString &msg)  { instance().log(Info, module, msg); }
    static void warn(const QString &module, const QString &msg)  { instance().log(Warning, module, msg); }
    static void error(const QString &module, const QString &msg) { instance().log(Error, module, msg); }

signals:
    void logMessage(const QString &formattedMessage);

private:
    Logger(QObject *parent = nullptr);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    QMutex m_mutex;
    QFile m_logFile;
    QTextStream m_stream;
    Level m_logLevel = Info;
};

// Convenience macros
#define LOG_DEBUG(msg) Logger::debug(Q_FUNC_INFO, msg)
#define LOG_INFO(msg)  Logger::info(Q_FUNC_INFO, msg)
#define LOG_WARN(msg)  Logger::warn(Q_FUNC_INFO, msg)
#define LOG_ERROR(msg) Logger::error(Q_FUNC_INFO, msg)

#endif // LOGGER_H
