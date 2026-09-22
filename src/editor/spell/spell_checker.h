#pragma once

#include "render/layout/decoration.h"

#include <QHash>
#include <QObject>
#include <QSet>
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
// frontmatter are skipped. "Add to dictionary" goes to the clone's own custom dictionary
// (~/.config/obsidian-cpp/Custom Dictionary.txt, one word per line, like Electron's), never to the
// system or KDE personal dictionaries.
class SpellChecker : public QObject {
    Q_OBJECT
public:
    static SpellChecker& instance();
    ~SpellChecker() override;

    bool available() const;          // a dictionary for the language exists
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }
    QString language() const;
    void setLanguage(const QString& lang); // e.g. "en_US"

    bool isMisspelled(const QString& word);
    QStringList suggestions(const QString& word, int max = 8);
    void addToDictionary(const QString& word); // persisted
    void ignoreWord(const QString& word);      // this session only
    QStringList customWords() const;
    // Tests: use another custom dictionary file (reloads it).
    void setCustomDictionaryPath(const QString& path);

    // Append SpellError marks for `line` of `doc` to `d`. skipCol: a column whose word is left alone
    // (the word being typed, like Chromium does), -1 for none.
    void decorateLine(const md::DocSource& doc, int line, LineDecoration& d, int skipCol = -1);
    // Word boundaries around a document position (for the context menu).
    static bool wordAt(QStringView lineText, int col, int* from, int* to);

signals:
    void dictionaryChanged(); // a word was added/ignored: editors re-decorate

private:
    SpellChecker();
    void loadCustom();
    std::unique_ptr<Sonnet::Speller> m_speller;
    QHash<QString, bool> m_cache;
    QSet<QString> m_custom;
    QString m_customPath;
    bool m_enabled = true;
};

} // namespace oc
