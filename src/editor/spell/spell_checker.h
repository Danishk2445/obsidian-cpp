#pragma once

#include "render/layout/decoration.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>

namespace Sonnet {
class Speller;
}

namespace oc {

namespace md {
class DocSource;
}

// Spellcheck for the editor (Obsidian: Editor > Spellcheck, languages from the system dictionaries).
// Words are checked lazily per visible line and cached; code, links, URLs, tags, math, HTML and
// frontmatter are skipped.
class SpellChecker {
public:
    static SpellChecker& instance();

    bool available() const;          // a dictionary for the language exists
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }
    QString language() const;
    void setLanguage(const QString& lang); // e.g. "en_US"

    bool isMisspelled(const QString& word);
    QStringList suggestions(const QString& word, int max = 8);
    void addToDictionary(const QString& word);
    void ignoreWord(const QString& word);

    // Append SpellError marks for `line` of `doc` to `d` (respecting replaces/marks already there).
    void decorateLine(const md::DocSource& doc, int line, LineDecoration& d);
    // Word boundaries around a document position (for the context menu).
    static bool wordAt(QStringView lineText, int col, int* from, int* to);

private:
    SpellChecker();
    std::unique_ptr<Sonnet::Speller> m_speller;
    QHash<QString, bool> m_cache;
    bool m_enabled = true;
};

} // namespace oc
