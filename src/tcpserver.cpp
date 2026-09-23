#include "tcpserver.h"
#include "logger.h"
#include <QDateTime>

TcpServer::TcpServer(QObject *parent)
    : QObject(parent)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection,
            this, &TcpServer::onNewConnection);
}

TcpServer::~TcpServer()
{
    stop();
}

bool TcpServer::start(quint16 port)
{
    if (m_server->isListening()) {
        m_server->close();
    }

    if (!m_server->listen(QHostAddress::Any, port)) {
        QString err = QString("TCP Server failed to listen on port %1: %2")
                          .arg(port).arg(m_server->errorString());
        LOG_ERROR(err);
        emit errorOccurred(err);
        return false;
    }

    LOG_INFO(QString("TCP Server listening on port %1").arg(port));
    return true;
}

void TcpServer::stop()
{
    for (auto client : m_clients) {
        client->disconnectFromHost();
    }
    m_clients.clear();
    m_server->close();
    LOG_INFO("TCP Server stopped");
}

bool TcpServer::isListening() const
{
    return m_server->isListening();
}

int TcpServer::clientCount() const
{
    return m_clients.size();
}

void TcpServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *client = m_server->nextPendingConnection();
        QString addr = QString("%1:%2")
                           .arg(client->peerAddress().toString())
                           .arg(client->peerPort());

        m_clients.append(client);
        connect(client, &QTcpSocket::disconnected, this, &TcpServer::onClientDisconnected);
        connect(client, &QTcpSocket::readyRead, this, &TcpServer::onClientDataReady);

        LOG_INFO(QString("TCP client connected: %1 (total: %2)")
                     .arg(addr).arg(m_clients.size()));
        emit clientConnected(addr);

        // Send welcome message
        QJsonObject welcome;
        welcome["type"] = "welcome";
        welcome["message"] = "SubseaImagingSystem TCP Server";
        welcome["timestamp"] = QDateTime::currentMSecsSinceEpoch();
        client->write(QJsonDocument(welcome).toJson(QJsonDocument::Compact) + "\n");
    }
}

void TcpServer::onClientDisconnected()
{
    QTcpSocket *client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    QString addr = QString("%1:%2")
                       .arg(client->peerAddress().toString())
                       .arg(client->peerPort());

    m_clients.removeOne(client);
    client->deleteLater();

    LOG_INFO(QString("TCP client disconnected: %1 (remaining: %2)")
                 .arg(addr).arg(m_clients.size()));
    emit clientDisconnected(addr);
}

void TcpServer::onClientDataReady()
{
    QTcpSocket *client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    QByteArray data = client->readAll();
    QString msg = QString::fromUtf8(data).trimmed();
    LOG_DEBUG(QString("TCP RX: %1").arg(msg));
    emit dataReceived(msg);
}

void TcpServer::sendAngleData(const AngleDifference &angle, float maxTemp,
                               int sourceX, int sourceY)
{
    // 手工构造 JSON 文本: 数值统一保留两位小数。
    // (JSON 允许 1.20 这类字面量, 数值解析不受影响, 但文本上固定两位小数)
    const QString line = QString(
        "{\"type\":\"angle_data\",\"timestamp\":%1,\"delta_x\":%2,\"delta_y\":%3,"
        "\"distance\":%4,\"valid\":%5,\"max_temp\":%6,\"source_x\":%7,\"source_y\":%8}")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QString::number(static_cast<double>(angle.deltaX), 'f', 2))
        .arg(QString::number(static_cast<double>(angle.deltaY), 'f', 2))
        .arg(QString::number(static_cast<double>(angle.distance), 'f', 2))
        .arg(angle.valid ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(QString::number(static_cast<double>(maxTemp), 'f', 2))
        .arg(sourceX)
        .arg(sourceY);
    sendText(line);
}

void TcpServer::sendText(const QString &line)
{
    broadcastMessage(line.toUtf8() + "\n");
}

void TcpServer::onAngleCalculated(const AngleDifference &angle)
{
    sendAngleData(angle, angle.maxTemp, angle.sourceX, angle.sourceY);
}

void TcpServer::sendJson(const QJsonObject &obj)
{
    broadcastMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
}

void TcpServer::broadcastMessage(const QByteArray &message)
{
    for (auto client : m_clients) {
        if (client->state() == QAbstractSocket::ConnectedState) {
            client->write(message);
        }
    }
}
