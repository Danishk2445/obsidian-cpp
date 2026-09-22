#pragma once

// Helpers for the vault tests. Every test works in a QTemporaryDir (never the real vault).

#include "core/vault/vault.h"
#include "core/vault/vault_private.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QTest>

#include <gtest/gtest.h>

#include <functional>
#include <ostream>

// Readable gtest failure messages for Qt strings.
inline void PrintTo(const QString& s, std::ostream* os) { *os << '"' << s.toStdString() << '"'; }

namespace vt {

inline bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(bytes) == bytes.size();
}

inline QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

inline bool waitFor(const std::function<bool()>& pred, int timeoutMs = 10000)
{
    return QTest::qWaitFor(pred, timeoutMs);
}

struct Ev {
    QString kind;
    QString path;
    QString old;
    bool external = false;
};

class SignalLog : public QObject {
public:
    explicit SignalLog(oc::Vault* v)
    {
        connect(v, &oc::Vault::created, this,
                [this](oc::TAbstractFile* f, bool ext) { evs.append({QStringLiteral("created"), f->path(), {}, ext}); });
        connect(v, &oc::Vault::modified, this,
                [this](oc::TFile* f, bool ext) { evs.append({QStringLiteral("modified"), f->path(), {}, ext}); });
        connect(v, &oc::Vault::deleted, this,
                [this](oc::TAbstractFile* f, bool ext) { evs.append({QStringLiteral("deleted"), f->path(), {}, ext}); });
        connect(v, &oc::Vault::renamed, this, [this](oc::TAbstractFile* f, const QString& old, bool ext) {
            evs.append({QStringLiteral("renamed"), f->path(), old, ext});
        });
        connect(v, &oc::Vault::onlineChanged, this, [this](bool on) { online.append(on); });
    }

    int count(const QString& kind) const
    {
        int n = 0;
        for (const Ev& e : evs)
            n += e.kind == kind;
        return n;
    }
    int count(const QString& kind, bool external) const
    {
        int n = 0;
        for (const Ev& e : evs)
            n += e.kind == kind && e.external == external;
        return n;
    }
    int externalCount() const
    {
        int n = 0;
        for (const Ev& e : evs)
            n += e.external;
        return n;
    }
    bool has(const QString& kind, const QString& path, bool external) const
    {
        for (const Ev& e : evs)
            if (e.kind == kind && e.path == path && e.external == external)
                return true;
        return false;
    }
    bool hasRename(const QString& from, const QString& to, bool external) const
    {
        for (const Ev& e : evs)
            if (e.kind == u"renamed" && e.old == from && e.path == to && e.external == external)
                return true;
        return false;
    }
    std::string dump() const
    {
        QString s;
        for (const Ev& e : evs)
            s += e.kind + u' ' + (e.old.isEmpty() ? QString() : QString(e.old + QStringLiteral(" -> "))) + e.path
                 + (e.external ? QStringLiteral(" [ext]") : QStringLiteral(" [own]")) + u'\n';
        for (bool on : online)
            s += QStringLiteral("online=") + (on ? QStringLiteral("true") : QStringLiteral("false")) + u'\n';
        return s.toStdString();
    }
    void clear()
    {
        evs.clear();
        online.clear();
    }

    QList<Ev> evs;
    QList<bool> online;
};

inline QStringList sortedPaths(const QList<oc::TFile*>& files)
{
    QStringList out;
    for (const oc::TFile* f : files)
        out.append(f->path());
    out.sort();
    return out;
}

inline QStringList sortedPaths(const QList<oc::TFolder*>& folders)
{
    QStringList out;
    for (const oc::TFolder* f : folders)
        out.append(f->path());
    out.sort();
    return out;
}

} // namespace vt
