#include "editor/spell/spell_checker.h"

#include "markdown/doc_source.h"

#include <Sonnet/Speller>

#include <QRegularExpression>
#include <QTextBoundaryFinder>

namespace oc {

SpellChecker& SpellChecker::instance()
{
    static SpellChecker s;
    return s;
}

SpellChecker::SpellChecker()
    : m_speller(std::make_unique<Sonnet::Speller>(QStringLiteral("en_US")))
{
}

bool SpellChecker::available() const { return m_speller && m_speller->isValid(); }
QString SpellChecker::language() const { return m_speller ? m_speller->language() : QString(); }

void SpellChecker::setLanguage(const QString& lang)
{
    m_speller->setLanguage(lang);
    m_cache.clear();
}

bool SpellChecker::isMisspelled(const QString& word)
{
    if (!available())
        return false;
    auto it = m_cache.constFind(word);
    if (it != m_cache.cend())
        return *it;
    const bool bad = m_speller->isMisspelled(word);
    if (m_cache.size() > 50000)
        m_cache.clear();
    m_cache.insert(word, bad);
    return bad;
}

QStringList SpellChecker::suggestions(const QString& word, int max)
{
    if (!available())
        return {};
    QStringList s = m_speller->suggest(word);
    return s.mid(0, max);
}

void SpellChecker::addToDictionary(const QString& word)
{
    if (available())
        m_speller->addToPersonal(word);
    m_cache.insert(word, false);
}

void SpellChecker::ignoreWord(const QString& word)
{
    if (available())
        m_speller->addToSession(word);
    m_cache.insert(word, false);
}

bool SpellChecker::wordAt(QStringView text, int col, int* from, int* to)
{
    const QString s = text.toString();
    if (col < 0 || col > s.size())
        return false;
    int a = col, b = col;
    auto isWordChar = [](QChar c) { return c.isLetter() || c == u'\'' || c == u'’'; };
    while (a > 0 && isWordChar(s[a - 1]))
        --a;
    while (b < s.size() && isWordChar(s[b]))
        ++b;
    if (b <= a)
        return false;
    *from = a;
    *to = b;
    return true;
}

void SpellChecker::decorateLine(const md::DocSource& doc, int line, LineDecoration& d)
{
    if (!m_enabled || !available())
        return;
    using R = md::LineRole;
    const md::LineRole role = doc.syntax().role(line);
    if (role == R::FenceBody || role == R::FenceOpen || role == R::FenceClose || role == R::IndentedCode || role == R::MathBody
        || role == R::MathOpen || role == R::MathClose || role == R::MathOneLine || role == R::FrontmatterBody || role == R::FrontmatterOpen
        || role == R::FrontmatterClose || role == R::Html || role == R::CommentBody)
        return;
    const QString s = doc.text().line(line);
    // Regions to skip: inline code, wikilinks/embeds, markdown link targets, URLs, tags, math, HTML tags.
    static const QRegularExpression skipRe(QStringLiteral(
        R"(`[^`]*`|\[\[[^\]]*\]\]|\]\([^)]*\)|<[^>]+>|https?://\S+|www\.\S+|\S+@\S+\.\S+|(?:^|\s)#[^\s#]+|\$[^$]+\$|%%.*?%%)"));
    QList<QPair<int, int>> skip;
    for (auto it = skipRe.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        skip.push_back({int(m.capturedStart()), int(m.capturedEnd())});
    }
    auto skipped = [&](int a, int b) {
        for (const auto& [x, y] : skip)
            if (a < y && b > x)
                return true;
        return false;
    };
    QTextBoundaryFinder bf(QTextBoundaryFinder::Word, s);
    int start = 0;
    while (bf.toNextBoundary() >= 0) {
        const int end = int(bf.position());
        if (bf.boundaryReasons() & QTextBoundaryFinder::EndOfItem) {
            const QString word = s.mid(start, end - start);
            bool hasLetter = false, hasDigit = false;
            for (QChar c : word) {
                hasLetter |= c.isLetter();
                hasDigit |= c.isDigit();
            }
            if (hasLetter && !hasDigit && word.size() > 1 && !skipped(start, end) && !(word.size() > 1 && word == word.toUpper())
                && isMisspelled(word))
                d.marks.push_back({start, end, StyleId::SpellError});
        }
        start = end;
    }
}

} // namespace oc
