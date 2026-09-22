#include "core/config/json_file.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace oc::JsonFile {

QJsonObject read(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.exists() ? f.errorString() : QString();
        return {};
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, pe.errorString());
        return {};
    }
    return doc.object();
}

namespace {
// QJsonDocument::Indented uses 4 spaces; Obsidian writes JSON.stringify(x, null, 2).
QByteArray toTwoSpaceJson(const QJsonObject& obj)
{
    const QByteArray four = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    QByteArray out;
    out.reserve(four.size());
    int i = 0;
    while (i < four.size()) {
        int spaces = 0;
        while (i + spaces < four.size() && four[i + spaces] == ' ')
            ++spaces;
        out.append(QByteArray(spaces / 2, ' '));
        i += spaces;
        const int nl = four.indexOf('\n', i);
        const int end = nl < 0 ? four.size() : nl + 1;
        out.append(four.constData() + i, end - i);
        i = end;
    }
    while (out.endsWith('\n'))
        out.chop(1);
    return out;
}
} // namespace

bool write(const QString& path, const QJsonObject& obj, QString* error)
{
    const QByteArray bytes = toTwoSpaceJson(obj);
    {
        QFile existing(path);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes)
            return true;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(bytes);
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace oc::JsonFile
