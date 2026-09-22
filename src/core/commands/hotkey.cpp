#include "core/commands/hotkey.h"

#include <QHash>
#include <QJsonArray>
#include <QKeyEvent>

namespace oc {

namespace {
struct KeyName {
    int qt;
    const char* name;
};
// Qt key <-> KeyboardEvent.key names for non-character keys.
const KeyName kNames[] = {
    {Qt::Key_Up, "ArrowUp"},       {Qt::Key_Down, "ArrowDown"},   {Qt::Key_Left, "ArrowLeft"},  {Qt::Key_Right, "ArrowRight"},
    {Qt::Key_Return, "Enter"},     {Qt::Key_Enter, "Enter"},      {Qt::Key_Tab, "Tab"},         {Qt::Key_Backtab, "Tab"},
    {Qt::Key_Escape, "Escape"},    {Qt::Key_Backspace, "Backspace"}, {Qt::Key_Delete, "Delete"}, {Qt::Key_Insert, "Insert"},
    {Qt::Key_Home, "Home"},        {Qt::Key_End, "End"},          {Qt::Key_PageUp, "PageUp"},   {Qt::Key_PageDown, "PageDown"},
    {Qt::Key_Space, " "},          {Qt::Key_F1, "F1"},            {Qt::Key_F2, "F2"},           {Qt::Key_F3, "F3"},
    {Qt::Key_F4, "F4"},            {Qt::Key_F5, "F5"},            {Qt::Key_F6, "F6"},           {Qt::Key_F7, "F7"},
    {Qt::Key_F8, "F8"},            {Qt::Key_F9, "F9"},            {Qt::Key_F10, "F10"},         {Qt::Key_F11, "F11"},
    {Qt::Key_F12, "F12"},
};

QString nameForQtKey(int key)
{
    for (const KeyName& k : kNames)
        if (k.qt == key)
            return QString::fromLatin1(k.name);
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return QString(QChar(char16_t('A' + (key - Qt::Key_A))));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return QString(QChar(char16_t('0' + (key - Qt::Key_0))));
    // Punctuation: Qt keys equal their unshifted Latin-1 codes for US layouts.
    switch (key) {
    case Qt::Key_Comma: return QStringLiteral(",");
    case Qt::Key_Period: return QStringLiteral(".");
    case Qt::Key_Slash: return QStringLiteral("/");
    case Qt::Key_Semicolon: return QStringLiteral(";");
    case Qt::Key_Apostrophe: return QStringLiteral("'");
    case Qt::Key_BracketLeft: return QStringLiteral("[");
    case Qt::Key_BracketRight: return QStringLiteral("]");
    case Qt::Key_Backslash: return QStringLiteral("\\");
    case Qt::Key_Minus: return QStringLiteral("-");
    case Qt::Key_Equal: return QStringLiteral("=");
    case Qt::Key_QuoteLeft: return QStringLiteral("`");
    // shifted variants reported by some layouts
    case Qt::Key_Less: return QStringLiteral(",");
    case Qt::Key_Greater: return QStringLiteral(".");
    case Qt::Key_Question: return QStringLiteral("/");
    case Qt::Key_Colon: return QStringLiteral(";");
    case Qt::Key_BraceLeft: return QStringLiteral("[");
    case Qt::Key_BraceRight: return QStringLiteral("]");
    case Qt::Key_Plus: return QStringLiteral("=");
    case Qt::Key_Underscore: return QStringLiteral("-");
    default: break;
    }
    if (key >= 0x20 && key < 0x7f)
        return QString(QChar(char16_t(key))).toUpper();
    return {};
}

int qtKeyForName(const QString& name)
{
    for (const KeyName& k : kNames)
        if (name == QLatin1String(k.name))
            return k.qt;
    if (name.size() == 1) {
        const QChar c = name[0].toUpper();
        if (c >= u'A' && c <= u'Z')
            return Qt::Key_A + (c.unicode() - 'A');
        if (c >= u'0' && c <= u'9')
            return Qt::Key_0 + (c.unicode() - '0');
        return c.unicode();
    }
    return 0;
}

QString normalizeKey(const QString& k)
{
    if (k.size() == 1)
        return k.toUpper();
    // tolerate lowercase / Obsidian-legacy names
    static const QHash<QString, QString> alias = {{QStringLiteral("arrowup"), QStringLiteral("ArrowUp")},
                                                   {QStringLiteral("arrowdown"), QStringLiteral("ArrowDown")},
                                                   {QStringLiteral("arrowleft"), QStringLiteral("ArrowLeft")},
                                                   {QStringLiteral("arrowright"), QStringLiteral("ArrowRight")},
                                                   {QStringLiteral("enter"), QStringLiteral("Enter")},
                                                   {QStringLiteral("return"), QStringLiteral("Enter")},
                                                   {QStringLiteral("esc"), QStringLiteral("Escape")},
                                                   {QStringLiteral("escape"), QStringLiteral("Escape")},
                                                   {QStringLiteral("tab"), QStringLiteral("Tab")},
                                                   {QStringLiteral("space"), QStringLiteral(" ")},
                                                   {QStringLiteral("delete"), QStringLiteral("Delete")},
                                                   {QStringLiteral("backspace"), QStringLiteral("Backspace")},
                                                   {QStringLiteral("pageup"), QStringLiteral("PageUp")},
                                                   {QStringLiteral("pagedown"), QStringLiteral("PageDown")},
                                                   {QStringLiteral("home"), QStringLiteral("Home")},
                                                   {QStringLiteral("end"), QStringLiteral("End")},
                                                   {QStringLiteral("up"), QStringLiteral("ArrowUp")},
                                                   {QStringLiteral("down"), QStringLiteral("ArrowDown")},
                                                   {QStringLiteral("left"), QStringLiteral("ArrowLeft")},
                                                   {QStringLiteral("right"), QStringLiteral("ArrowRight")}};
    const QString lower = k.toLower();
    if (alias.contains(lower))
        return alias.value(lower);
    if (lower.startsWith(u'f') && lower.size() <= 3)
        return k.toUpper();
    return k;
}
} // namespace

Hotkey Hotkey::fromJson(const QJsonObject& o)
{
    Hotkey h;
    for (const QJsonValue& m : o.value(QStringLiteral("modifiers")).toArray()) {
        const QString s = m.toString();
        if (s == u"Mod" || s == u"Ctrl")
            h.mod = true;
        else if (s == u"Shift")
            h.shift = true;
        else if (s == u"Alt")
            h.alt = true;
        else if (s == u"Meta")
            h.meta = true;
    }
    h.key = normalizeKey(o.value(QStringLiteral("key")).toString());
    return h;
}

QJsonObject Hotkey::toJson() const
{
    QJsonArray mods;
    if (mod)
        mods.append(QStringLiteral("Mod"));
    if (alt)
        mods.append(QStringLiteral("Alt"));
    if (meta)
        mods.append(QStringLiteral("Meta"));
    if (shift)
        mods.append(QStringLiteral("Shift"));
    return {{QStringLiteral("modifiers"), mods}, {QStringLiteral("key"), key}};
}

Hotkey Hotkey::fromEvent(const QKeyEvent* e)
{
    Hotkey h;
    const Qt::KeyboardModifiers m = e->modifiers();
    h.mod = m & Qt::ControlModifier;
    h.shift = m & Qt::ShiftModifier;
    h.alt = m & Qt::AltModifier;
    h.meta = m & Qt::MetaModifier;
    int key = e->key();
    // With Shift held, Qt reports the shifted symbol; map back via the unshifted key name.
    h.key = nameForQtKey(key);
    return h;
}

Hotkey Hotkey::parse(const QString& spec)
{
    Hotkey h;
    const QStringList parts = spec.split(QLatin1Char('+'), Qt::KeepEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        QString p = parts[i].trimmed();
        if (p.isEmpty() && i + 1 == parts.size()) // "Mod++" -> key '+'
            p = QStringLiteral("+");
        if (p == u"Mod" || p == u"Ctrl")
            h.mod = true;
        else if (p == u"Shift")
            h.shift = true;
        else if (p == u"Alt")
            h.alt = true;
        else if (p == u"Meta")
            h.meta = true;
        else if (!p.isEmpty())
            h.key = normalizeKey(p);
    }
    return h;
}

QString Hotkey::toString() const
{
    QStringList parts;
    if (mod)
        parts << QStringLiteral("Ctrl");
    if (meta)
        parts << QStringLiteral("Win");
    if (alt)
        parts << QStringLiteral("Alt");
    if (shift)
        parts << QStringLiteral("Shift");
    QString k = key;
    if (k == u"ArrowUp")
        k = QStringLiteral("↑");
    else if (k == u"ArrowDown")
        k = QStringLiteral("↓");
    else if (k == u"ArrowLeft")
        k = QStringLiteral("←");
    else if (k == u"ArrowRight")
        k = QStringLiteral("→");
    else if (k == u" ")
        k = QStringLiteral("Space");
    parts << k;
    return parts.join(QStringLiteral(" + "));
}

QKeySequence Hotkey::toSequence() const
{
    int k = qtKeyForName(key);
    Qt::KeyboardModifiers m;
    if (mod)
        m |= Qt::ControlModifier;
    if (shift)
        m |= Qt::ShiftModifier;
    if (alt)
        m |= Qt::AltModifier;
    if (meta)
        m |= Qt::MetaModifier;
    return QKeySequence(QKeyCombination(m, Qt::Key(k)));
}

size_t qHash(const Hotkey& h, size_t seed) { return qHashMulti(seed, h.mod, h.shift, h.alt, h.meta, h.key); }

} // namespace oc
