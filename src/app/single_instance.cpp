#include "app/single_instance.h"

#include "core/config/clone_paths.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>

namespace oc {

SingleInstance::SingleInstance(QObject* parent)
    : QObject(parent)
{
}

SingleInstance::~SingleInstance() = default;

QString SingleInstance::serverName()
{
    const QByteArray key = ClonePaths::configRoot().toUtf8();
    return QStringLiteral("obsidian-cpp-") + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
}

bool SingleInstance::sendToRunning(const QJsonObject& request, int timeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(timeoutMs))
        return false;
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(timeoutMs))
        return false;
    // The running instance answers "ok" once it has taken the request.
    if (!socket.waitForReadyRead(timeoutMs))
        return false;
    return socket.readLine().trimmed() == "ok";
}

bool SingleInstance::listen()
{
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(serverName())) {
        // A socket left behind by a crashed instance (nobody answered sendToRunning()).
        QLocalServer::removeServer(serverName());
        if (!m_server->listen(serverName()))
            return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                if (!s->canReadLine())
                    return;
                const QJsonObject request = QJsonDocument::fromJson(s->readLine()).object();
                s->write("ok\n");
                s->flush();
                s->disconnectFromServer();
                emit requestReceived(request);
            });
        }
    });
    return true;
}

} // namespace oc
