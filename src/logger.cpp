#include "logger.h"
#include <QDir>
#include <QCoreApplication>

Logger& Logger::instance()
{
    static Logger inst;
    return inst;
}

Logger::Logger(QObject *parent)
    : QObject(parent)
{
    // Default log file in application directory
    QString logPath = QCoreApplication::applicationDirPath() + "/app.log";
    setLogFile(logPath);
}

Logger::~Logger()
{
    QMutexLocker locker(&m_mutex);
    if (m_logFile.isOpen()) {
        m_stream.flush();
        m_logFile.close();
    }
}

void Logger::setLogFile(const QString &path)
{
    QMutexLocker locker(&m_mutex);
    if (m_logFile.isOpen()) {
        m_stream.flush();
        m_logFile.close();
    }
    QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    m_logFile.setFileName(path);
    m_logFile.open(QIODevice::WriteOnly | QIODevice::Append);
    m_stream.setDevice(&m_logFile);
}

void Logger::setLogLevel(Level level)
{
    m_logLevel = level;
}

void Logger::log(Level level, const QString &module, const QString &message)
{
    if (level < m_logLevel) return;

    static const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString formatted = QString("[%1] [%2] %3: %4")
                            .arg(timestamp)
                            .arg(levelStr[level])
                            .arg(module)
                            .arg(message);

    QMutexLocker locker(&m_mutex);
    if (m_logFile.isOpen()) {
        m_stream << formatted << endl;
        m_stream.flush();
    }

    emit logMessage(formatted);
    fprintf(stderr, "%s\n", formatted.toLocal8Bit().constData());
}
