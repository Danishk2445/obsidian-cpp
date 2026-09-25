#pragma once

#include <QJsonObject>
#include <QObject>

class QLocalServer;

namespace oc {

// One interactive process per clone configuration, like Obsidian: a second launch hands its request
// ({"vault", "open", "mode"}) to the running process over a local socket and exits. The socket name
// derives from the clone's config root, so runs with isolated XDG dirs (tests, headless) never talk
// to the user's instance.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    static QString serverName();
    // True when a running instance accepted `request` (the caller should exit).
    static bool sendToRunning(const QJsonObject& request, int timeoutMs = 1500);
    // Start answering later launches; false if the name could not be claimed.
    bool listen();

signals:
    void requestReceived(const QJsonObject& request);

private:
    QLocalServer* m_server = nullptr;
};

} // namespace oc
