#include "core/plugins/note_plugins.h"

#include "core/time/moment_format.h"

#include <QRegularExpression>
#include <QStringList>

namespace oc::plugins {

using namespace Qt::StringLiterals;

QString applyTemplate(QString content, const QHash<QString, QString>& vars, const QString& dateFormat, const QString& timeFormat,
                      const QDateTime& now)
{
    for (auto it = vars.cbegin(); it != vars.cend(); ++it) {
        const QRegularExpression re(u"\\{\\{"_s + QRegularExpression::escape(it.key()) + u"\\}\\}"_s, QRegularExpression::CaseInsensitiveOption);
        content.replace(re, it.value());
    }
    static const QRegularExpression dt(u"\\{\\{(date|time)(?::(.*?))?\\}\\}"_s, QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype last = 0;
    for (auto it = dt.globalMatch(content); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(content).mid(last, m.capturedStart() - last);
        QString fmt = m.captured(2);
        if (!m.hasCaptured(2) || fmt.isEmpty()) // "{{date:}}" behaves like moment().format("") = ISO; Obsidian's regex gives ""
            fmt = m.captured(1).compare(u"date"_s, Qt::CaseInsensitive) == 0 ? (dateFormat.isEmpty() ? u"YYYY-MM-DD"_s : dateFormat)
                                                                              : (timeFormat.isEmpty() ? u"HH:mm"_s : timeFormat);
        out += formatMoment(now, fmt);
        last = m.capturedEnd();
    }
    out += QStringView(content).mid(last);
    return out;
}

TemplateParts splitFrontmatter(const QString& content)
{
    TemplateParts p;
    p.body = content;
    static const QRegularExpression open(u"^---[ \\t]*\\r?\\n"_s);
    const QRegularExpressionMatch m = open.match(content);
    if (!m.hasMatch())
        return p;
    static const QRegularExpression close(u"(^|\\n)---[ \\t]*(\\r?\\n|$)"_s);
    const QRegularExpressionMatch c = close.match(content, m.capturedEnd() - 1);
    if (!c.hasMatch())
        return p;
    const qsizetype yamlStart = m.capturedEnd();
    const qsizetype yamlEnd = c.capturedStart() + c.capturedLength(1);
    p.hasFrontmatter = true;
    p.frontmatter = yamlStart <= yamlEnd ? content.mid(yamlStart, yamlEnd - yamlStart) : QString();
    if (p.frontmatter.endsWith(u'\n'))
        p.frontmatter.chop(1);
    p.body = content.mid(c.capturedEnd());
    return p;
}

namespace {
// top-level keys of a YAML mapping, with the lines that belong to each
QList<QPair<QString, QString>> yamlEntries(const QString& yaml)
{
    QList<QPair<QString, QString>> out;
    static const QRegularExpression key(u"^([^\\s#:][^:]*):"_s);
    for (const QString& line : yaml.split(u'\n')) {
        const QRegularExpressionMatch m = key.match(line);
        if (m.hasMatch())
            out.append({m.captured(1).trimmed(), line});
        else if (!out.isEmpty())
            out.last().second += u'\n' + line;
    }
    return out;
}
} // namespace

QString mergeFrontmatter(const QString& noteText, const QString& templateYaml)
{
    const QList<QPair<QString, QString>> add = yamlEntries(templateYaml);
    if (add.isEmpty())
        return noteText;
    const TemplateParts note = splitFrontmatter(noteText);
    if (!note.hasFrontmatter) {
        QStringList lines;
        for (const auto& [k, l] : add)
            lines << l;
        return u"---\n"_s + lines.join(u'\n') + u"\n---\n"_s + noteText;
    }
    QSet<QString> have;
    for (const auto& [k, l] : yamlEntries(note.frontmatter))
        have.insert(k.toLower());
    QString yaml = note.frontmatter;
    for (const auto& [k, l] : add) {
        if (have.contains(k.toLower()))
            continue;
        if (!yaml.isEmpty())
            yaml += u'\n';
        yaml += l;
    }
    return u"---\n"_s + yaml + u"\n---\n"_s + note.body;
}

DailyNotesOptions DailyNotesOptions::fromJson(const QJsonObject& o)
{
    DailyNotesOptions d;
    d.format = o.value(u"format"_s).toString();
    d.folder = o.value(u"folder"_s).toString();
    d.templatePath = o.value(u"template"_s).toString();
    return d;
}

QString DailyNotesOptions::effectiveFormat() const { return format.isEmpty() ? u"YYYY-MM-DD"_s : format; }

QString dailyNoteName(const DailyNotesOptions& o, const QDateTime& when) { return formatMoment(when, o.effectiveFormat()).trimmed(); }

std::optional<QDateTime> dailyNoteDate(const QString& notePath, const QString& folderPath, const DailyNotesOptions& o)
{
    if (!notePath.endsWith(u".md"_s, Qt::CaseInsensitive))
        return std::nullopt;
    QString rel = notePath;
    if (!folderPath.isEmpty()) {
        const QString prefix = folderPath + u'/';
        if (!notePath.startsWith(prefix))
            return std::nullopt;
        rel = notePath.mid(prefix.size());
    }
    rel.chop(3);
    return parseMomentStrict(rel, o.effectiveFormat());
}

std::optional<QString> uniqueNoteName(const QString& format, const QString& suffix, const QDateTime& now,
                                      const std::function<bool(const QString&)>& taken)
{
    const QString fmt = format.isEmpty() ? u"YYYYMMDDHHmm"_s : format;
    auto withSuffix = [&](const QString& id) { return suffix.isEmpty() ? id : id + u' ' + suffix; };
    const QString first = formatMoment(now, fmt);
    if (!taken(withSuffix(first)))
        return withSuffix(first);
    // the smallest unit the format shows: minute, hour, day, week, month, year
    const std::function<QDateTime(const QDateTime&, int)> steps[] = {
        [](const QDateTime& d, int n) { return d.addSecs(60LL * n); },
        [](const QDateTime& d, int n) { return d.addSecs(3600LL * n); },
        [](const QDateTime& d, int n) { return d.addDays(n); },
        [](const QDateTime& d, int n) { return d.addDays(7LL * n); },
        [](const QDateTime& d, int n) { return d.addMonths(n); },
        [](const QDateTime& d, int n) { return d.addYears(n); },
    };
    // (like Obsidian: the unit is chosen against the suffixed id, and bumped ids drop the suffix)
    for (const auto& step : steps) {
        if (formatMoment(step(now, 1), fmt) == withSuffix(first))
            continue;
        for (int n = 1; n < 100000; ++n) {
            const QString id = formatMoment(step(now, n), fmt);
            if (!taken(id))
                return id;
        }
        return std::nullopt;
    }
    return std::nullopt; // the format doesn't depend on the time
}

QString withMdExtension(const QString& path)
{
    const QString name = path.section(u'/', -1);
    const qsizetype dot = name.lastIndexOf(u'.');
    return dot > 0 && dot < name.size() - 1 ? path : path + u".md"_s;
}

} // namespace oc::plugins
