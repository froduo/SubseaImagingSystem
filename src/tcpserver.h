#ifndef TCPSERVER_H
#define TCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QList>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include "anglecalculator.h"

class TcpServer : public QObject
{
    Q_OBJECT

public:
    explicit TcpServer(QObject *parent = nullptr);
    ~TcpServer();

    bool start(quint16 port);
    void stop();
    bool isListening() const;
    int clientCount() const;

    // 发送角度数据
    void sendAngleData(const AngleDifference &angle, float maxTemp,
                       int sourceX, int sourceY);

    // 主动向所有客户端广播一条 JSON(用于命令应答等)
    void sendJson(const QJsonObject &obj);
    // 广播一行文本(可为手工格式化的 JSON —— 便于固定小数位, 如 1.20)
    void sendText(const QString &line);

signals:
    void clientConnected(const QString &address);
    void clientDisconnected(const QString &address);
    void errorOccurred(const QString &error);
    void dataReceived(const QString &data);

public slots:
    void onAngleCalculated(const AngleDifference &angle);

private slots:
    void onNewConnection();
    void onClientDisconnected();
    void onClientDataReady();

private:
    void broadcastMessage(const QByteArray &message);

    QTcpServer *m_server = nullptr;
    QList<QTcpSocket*> m_clients;
    int m_sendInterval = 100;  // 100ms = 10Hz
};

#endif // TCPSERVER_H
