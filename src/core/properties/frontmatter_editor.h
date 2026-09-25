#pragma once

// Minimal, byte-exact edits of a note's YAML frontmatter (the "Properties" of Obsidian).
//
// Obsidian rewrites the whole frontmatter with its YAML serializer whenever one property changes. We
// only rewrite what changed: the frontmatter body is split into entries (a top-level key with its value
// lines and the comment lines directly above it) and trivia (blank lines, detached comments); an edit
// replaces one entry's lines with Obsidian's serialization of the new value (keeping the key text, the
// spacing after the colon of inline values, an inline "# comment", the indentation of block lists and
// the untouched items of a list), and the result is diffed line by line against the original, so
// every other line keeps its bytes and its line terminator. New keys go after the last non-blank line
// of the frontmatter. Removing the last property removes the frontmatter block (Obsidian does too).
//
// Frontmatter the line scanner cannot map (anchors/aliases/tags, top-level flow maps, explicit keys)
// is still editable when the YAML is valid: edits then rewrite the whole block the way Obsidian does.
// Invalid YAML (or a non-mapping root, or duplicate keys) cannot be edited, like in Obsidian.

#include "text/changeset.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

namespace oc::props {

struct Property {
    QString key;
    QVariant value; // as md::parseFrontmatter(): QString / double / bool / QVariantList / QVariantMap / null
};

// The frontmatter lines of a text: "---" (optionally followed by blanks) on the first line and the next
// such line closes it (the rule of the markdown parser).
struct FrontmatterSpan {
    bool exists = false;
    int openLine = 0;
    int closeLine = -1;
};
FrontmatterSpan findFrontmatter(const Text& text);

class FrontmatterEditor {
public:
    enum class State { None, Valid, Invalid };

    explicit FrontmatterEditor(const Text& text);
    ~FrontmatterEditor();
    FrontmatterEditor(const FrontmatterEditor&) = delete;
    FrontmatterEditor& operator=(const FrontmatterEditor&) = delete;

    State state() const { return m_state; }
    bool exists() const { return m_span.exists; }
    bool isEditable() const { return m_state != State::Invalid; }
    QString error() const { return m_error; } // YAML error (Invalid)
    const FrontmatterSpan& span() const { return m_span; }
    // Entries map to source lines (false: edits rewrite the whole block).
    bool isMappable() const { return m_mappable; }
    // Line (in the text) of the first YAML syntax error, or -1 (for the editor's error marker).
    int errorLine() const { return m_errorLine; }
    // The property whose lines (leading comments, key line, value) hold `line` of the original text; ""
    // for other lines, unmappable frontmatter or once edited.
    QString keyAtLine(int line) const;

    // The properties in order (reflecting the edits made so far).
    QList<Property> properties() const;
    bool has(const QString& key) const;
    QVariant value(const QString& key) const;

    // Edits, applied in order to the editor's copy. All fail (return false) on invalid frontmatter.
    bool set(const QString& key, const QVariant& value);  // replace the value, or append a new key
    bool add(const QString& key, const QVariant& value);  // append; false if the key exists
    bool rename(const QString& from, const QString& to);  // the value's text is kept; false if `to` exists
    bool remove(const QString& key);                      // the last one removes the whole block
    bool reorder(const QStringList& keys);                // a permutation of the current keys
    // Obsidian's merge (pasting properties, merging renamed keys): lists are concatenated without
    // duplicates, maps merged recursively, otherwise a non-null value replaces an empty one or wins.
    bool merge(const QString& key, const QVariant& value);
    // Remove the frontmatter block (Obsidian's "Clear file properties").
    void clear();

    bool isModified() const { return m_modified; }
    // Changes turning the original text into the edited one ('\n'-joined coordinates of the text).
    QList<ChangeSpec> changes() const;
    // The edited text (inserted line breaks get `newEol`).
    Text apply(Eol newEol = Eol::LF) const;

    struct Item;

private:
    int entryIndex(const QString& key) const;
    bool ensureRewriteMode();
    void setEntryValue(Item& item, const QVariant& value);
    void insertEntry(const QString& key, const QVariant& value);

    Text m_text;
    FrontmatterSpan m_span;
    State m_state = State::None;
    QString m_error;
    int m_errorLine = -1;
    bool m_mappable = true;
    bool m_modified = false;
    bool m_cleared = false;
    QStringList m_original; // body lines as in the text
    QList<std::shared_ptr<Item>> m_items;
    QList<std::shared_ptr<Item>> m_unmapped; // !m_mappable: the parsed entries (no lines) until an edit
};

// Obsidian's CR() merge of one value into another (see FrontmatterEditor::merge); `existing`
// invalid = the key is missing.
QVariant mergePropertyValue(const QVariant& existing, const QVariant& incoming, bool* changed = nullptr);

} // namespace oc::props
