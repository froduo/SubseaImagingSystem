#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include "mainwindow.h"
#include "configmanager.h"
#include "logger.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("SubseaImagingSystem");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Subsea");

    // Initialize logger
    Logger::instance().setLogLevel(Logger::Debug);
    LOG_INFO("=== Subsea Imaging System Starting ===");

    // Load configuration - try multiple paths
    QString configPath;
    QStringList searchPaths = {
        QCoreApplication::applicationDirPath() + "/../config/default.ini",
        QDir::homePath() + "/project/Subsea Imaging Inspection and Evidence System/config/default.ini",
        QDir::homePath() + "/project/SubseaImagingSystem/config/default.ini"
    };
    for (const QString &path : searchPaths) {
        if (QFileInfo::exists(path)) {
            configPath = path;
            break;
        }
    }
    if (configPath.isEmpty()) {
        configPath = searchPaths.first();
        LOG_WARN(QString("No config file found, using default: %1").arg(configPath));
    }
    ConfigManager::instance().loadConfig(configPath);
    LOG_INFO(QString("Config loaded from: %1").arg(configPath));

    // Create and show main window
    MainWindow w;
    w.show();

    LOG_INFO("Main window shown, entering event loop");
    int ret = app.exec();

    LOG_INFO(QString("Application exiting with code: %1").arg(ret));
    return ret;
}
