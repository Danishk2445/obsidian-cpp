#include "core/vault/ignore_rules.h"

namespace oc::vaultfs {

namespace {

bool isAsciiDigit(QChar c) { return c.unicode() >= u'0' && c.unicode() <= u'9'; }

bool isAsciiHex(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
}

} // namespace

bool isAtomicTempName(QStringView s)
{
    // Scan from the end: <hex>+ '.' <digits>+ '.' "tmp" '.' <at least one char>
    qsizetype i = s.size();
    const qsizetype hexEnd = i;
    while (i > 0 && isAsciiHex(s[i - 1]))
        --i;
    if (i == hexEnd || i == 0 || s[i - 1] != u'.')
        return false;
    --i;
    const qsizetype digitsEnd = i;
    while (i > 0 && isAsciiDigit(s[i - 1]))
        --i;
    if (i == digitsEnd || i == 0 || s[i - 1] != u'.')
        return false;
    --i;
    // s[0, i) must be ".+\.tmp"
    if (i < 5)
        return false;
    return s.sliced(i - 4, 4) == u".tmp";
}

bool isIgnoredName(QStringView name)
{
    if (name.isEmpty())
        return true;
    if (name.front() == u'.')
        return true; // dot files/folders, ".#*", our ".<name>.oc~*" temps
    if (name.back() == u'~')
        return true; // backup files
    if (name == u"4913")
        return true; // vim's "can I write here" probe
    if (name.endsWith(u".swp"))
        return true;
    return isAtomicTempName(name);
}

bool isIgnoredPath(QStringView path)
{
    if (path.isEmpty())
        return false; // the root
    qsizetype start = 0;
    while (start <= path.size()) {
        qsizetype end = path.indexOf(u'/', start);
        if (end < 0)
            end = path.size();
        if (isIgnoredName(path.sliced(start, end - start)))
            return true;
        start = end + 1;
    }
    return false;
}

bool normalizePath(const QString& in, QString* out, bool allowRoot)
{
    QString result;
    result.reserve(in.size());
    qsizetype start = 0;
    while (start < in.size()) {
        qsizetype end = in.indexOf(u'/', start);
        if (end < 0)
            end = in.size();
        const QStringView comp = QStringView(in).sliced(start, end - start);
        start = end + 1;
        if (comp.isEmpty())
            continue;
        if (comp == u"." || comp == u"..")
            return false;
        if (!result.isEmpty())
            result += u'/';
        result += comp;
    }
    if (result.isEmpty() && !allowRoot)
        return false;
    *out = result;
    return true;
}

void splitName(const QString& name, QString* basename, QString* extension)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot <= 0 || dot == name.size() - 1) {
        *basename = name;
        extension->clear();
        return;
    }
    *basename = name.left(dot);
    *extension = name.mid(dot + 1);
}

} // namespace oc::vaultfs
