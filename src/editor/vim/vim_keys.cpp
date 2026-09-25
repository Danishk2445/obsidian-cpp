#include "editor/vim/vim_keys.h"

#include <QHash>
#include <QKeyEvent>

namespace oc::vim {

namespace {
struct Special {
    int qt;
    const char* name;
};
// Qt key <-> the name CodeMirror-vim uses (KeyboardEvent.key through its specialKey table).
constexpr Special kSpecial[] = {
    {Qt::Key_Return, "CR"},       {Qt::Key_Enter, "CR"},      {Qt::Key_Backspace, "BS"},    {Qt::Key_Delete, "Del"},
    {Qt::Key_Escape, "Esc"},      {Qt::Key_Insert, "Ins"},    {Qt::Key_Left, "Left"},       {Qt::Key_Right, "Right"},
    {Qt::Key_Up, "Up"},           {Qt::Key_Down, "Down"},     {Qt::Key_Space, "Space"},     {Qt::Key_Tab, "Tab"},
    {Qt::Key_Backtab, "Tab"},     {Qt::Key_Home, "Home"},     {Qt::Key_End, "End"},         {Qt::Key_PageUp, "PageUp"},
    {Qt::Key_PageDown, "PageDown"},
};

bool isModifierKey(int k)
{
    switch (k) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_Mode_switch:
        return true;
    default:
        return k >= Qt::Key_Dead_Grave && k <= Qt::Key_Dead_Longsolidusoverlay;
    }
}
} // namespace

QString keyFromEvent(const QKeyEvent* e)
{
    const int k = e->key();
    if (isModifierKey(k))
        return {};
    const Qt::KeyboardModifiers mods = e->modifiers();
    bool ctrl = mods & Qt::ControlModifier;
    bool alt = mods & Qt::AltModifier;
    const bool meta = mods & Qt::MetaModifier;
    const bool shift = (mods & Qt::ShiftModifier) || k == Qt::Key_Backtab;

    QString key;
    for (const Special& s : kSpecial)
        if (s.qt == k)
            key = QLatin1String(s.name);
    if (key.isEmpty() && k >= Qt::Key_F1 && k <= Qt::Key_F35)
        key = u'F' + QString::number(k - Qt::Key_F1 + 1);
    if (key.isEmpty()) {
        const QString text = e->text();
        const bool printable = !text.isEmpty() && text.at(0).isPrint();
        if (printable && ctrl && alt) // AltGr on some platforms
            ctrl = alt = false;
        if (printable && !ctrl && !meta) {
            key = text;
        } else if (k >= Qt::Key_A && k <= Qt::Key_Z) {
            key = QChar(char16_t(shift ? k : k - Qt::Key_A + 'a'));
        } else if (k > 0x20 && k < 0x7f) {
            key = QChar(char16_t(k));
        } else if (printable) {
            key = text;
        } else if (k > 0x7f && k < 0x110000 && QChar::isPrint(char32_t(k))) {
            const char32_t cp = char32_t(k);
            key = QString::fromUcs4(&cp, 1).toLower();
        }
    }
    if (key.isEmpty())
        return {};
    QString name;
    if (ctrl)
        name += QLatin1String("C-");
    if (alt)
        name += QLatin1String("A-");
    if (meta)
        name += QLatin1String("M-");
    if ((!name.isEmpty() || key.size() > 1) && shift)
        name += QLatin1String("S-");
    name += key;
    if (name.size() > 1 && !(name.size() == 2 && name.at(0).isHighSurrogate()))
        name = u'<' + name + u'>';
    return name;
}

QStringList splitKeys(const QString& keys)
{
    QStringList out;
    for (qsizetype i = 0; i < keys.size();) {
        if (keys.at(i) == u'<') {
            const qsizetype close = keys.indexOf(u'>', i + 1);
            if (close > i + 1) {
                const QString inner = keys.mid(i + 1, close - i - 1);
                // (?:[CSMA]-)*(?:\w+|.)
                qsizetype p = 0;
                while (p + 1 < inner.size() && QStringLiteral("CSMA").contains(inner.at(p)) && inner.at(p + 1) == u'-')
                    p += 2;
                const QString rest = inner.mid(p);
                bool ok = rest.size() == 1;
                if (!ok && !rest.isEmpty()) {
                    ok = true;
                    for (QChar c : rest)
                        ok = ok && (c.isLetterOrNumber() || c == u'_');
                }
                if (ok) {
                    if (inner.compare(QLatin1String("lt"), Qt::CaseInsensitive) == 0)
                        out << QStringLiteral("<");
                    else
                        out << keys.mid(i, close - i + 1);
                    i = close + 1;
                    continue;
                }
            }
        }
        if (keys.at(i).isHighSurrogate() && i + 1 < keys.size()) {
            out << keys.mid(i, 2);
            i += 2;
            continue;
        }
        out << QString(keys.at(i));
        ++i;
    }
    return out;
}

bool keyToQt(const QString& key, int* qtKey, Qt::KeyboardModifiers* mods, QString* text)
{
    *mods = Qt::NoModifier;
    *text = QString();
    *qtKey = 0;
    if (key.isEmpty())
        return false;
    QString name = key;
    if (key.size() > 2 && key.startsWith(u'<') && key.endsWith(u'>')) {
        name = key.mid(1, key.size() - 2);
        while (name.size() > 2 && name.at(1) == u'-') {
            switch (name.at(0).unicode()) {
            case 'C':
                *mods |= Qt::ControlModifier;
                break;
            case 'A':
                *mods |= Qt::AltModifier;
                break;
            case 'M':
                *mods |= Qt::MetaModifier;
                break;
            case 'S':
                *mods |= Qt::ShiftModifier;
                break;
            default:
                return false;
            }
            name = name.mid(2);
        }
        for (const Special& s : kSpecial) {
            if (name == QLatin1String(s.name) && s.qt != Qt::Key_Enter && s.qt != Qt::Key_Backtab) {
                *qtKey = s.qt;
                if (s.qt == Qt::Key_Tab && (*mods & Qt::ShiftModifier))
                    *qtKey = Qt::Key_Backtab;
                static const QHash<int, QString> texts = {{Qt::Key_Return, QStringLiteral("\r")}, {Qt::Key_Backspace, QStringLiteral("\b")},
                                                          {Qt::Key_Delete, QStringLiteral("\x7f")}, {Qt::Key_Escape, QStringLiteral("\x1b")},
                                                          {Qt::Key_Space, QStringLiteral(" ")},    {Qt::Key_Tab, QStringLiteral("\t")}};
                if (!(*mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
                    *text = texts.value(s.qt);
                return true;
            }
        }
        if (name.size() >= 2 && name.at(0) == u'F' && name.mid(1).toInt() > 0) {
            *qtKey = Qt::Key_F1 + name.mid(1).toInt() - 1;
            return true;
        }
        if (name.size() != 1 && !(name.size() == 2 && name.at(0).isHighSurrogate()))
            return false;
    }
    const QChar c = name.at(0);
    if (c.isLetter() && c.unicode() < 0x80) {
        *qtKey = Qt::Key_A + (c.toUpper().unicode() - 'A');
        if (c.isUpper())
            *mods |= Qt::ShiftModifier;
    } else if (c.unicode() < 0x80) {
        *qtKey = c.unicode();
    } else {
        *qtKey = int(name.toUcs4().value(0));
        const QString up = name.toUpper();
        if (up.size() == name.size())
            *qtKey = int(up.toUcs4().value(0));
    }
    if (*mods & Qt::ControlModifier) {
        if (c.isLetter() && c.unicode() < 0x80)
            *text = QChar(char16_t(c.toLower().unicode() - 'a' + 1));
        else if (c == u'[')
            *text = QStringLiteral("\x1b");
    } else if (!(*mods & (Qt::AltModifier | Qt::MetaModifier))) {
        *text = name;
    }
    return true;
}

} // namespace oc::vim
