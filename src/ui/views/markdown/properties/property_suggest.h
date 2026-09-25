#pragma once

#include "core/metadata/link_suggest.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/views/markdown/editor_suggest.h"

#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

namespace oc {

class SuggestPopup;
class VaultSession;

// The autocomplete popups of the property editor (Obsidian's .suggestion-container.mod-property-key /
// .mod-property-value), attached to a QLineEdit or a plain-text QTextEdit:
//   Keys   — the vault's property names not used by the note yet, with their type icons;
//   Values — the values the vault uses for this key, or "[[" link suggestions (files, aliases,
//            headings, blocks) when the text starts with "[[";
//   Tags   — the vault's tags.
// It opens when the input gets the focus or its text is edited, ↑/↓/PageUp/PageDown move the
// selection, Enter/Tab/click accept (Shift+Enter accepts the typed text as is), Esc closes.
class PropertySuggest : public QObject {
    Q_OBJECT
public:
    enum class Mode { Keys, Values, Tags };
    struct Source {
        std::function<VaultSession*()> session;
        std::function<QString()> sourcePath;
        std::function<QString()> key;                // Values: the property's key
        std::function<bool(const QString&)> exclude; // texts not to suggest (the note's keys, values already listed)
    };

    PropertySuggest(QWidget* input, Mode mode, Source source, QObject* parent = nullptr);
    ~PropertySuggest() override;

    bool isOpen() const { return m_open; }
    void refresh();
    void close();
    // key: "" (click), "Enter", "Tab", "#", "^" (like Obsidian's link suggest).
    bool choose(int index, const QString& key = {});
    SuggestPopup* popup() const { return m_popup; }
    const QList<Suggestion>& rows() const { return m_rows; }
    void setEnabled(bool on);

signals:
    // A suggestion was accepted: the input's new text; commit: the value is final (Obsidian closes the
    // editor), otherwise editing goes on with the cursor at `cursor`.
    void accepted(const QString& text, bool commit, int cursor);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QString text() const;
    int cursor() const;
    void reposition();
    void setOpen(bool open);
    void chooseLink(const LinkSuggestion& s, const QString& key);

    QPointer<QWidget> m_input;
    Mode m_mode;
    Source m_source;
    QPointer<SuggestPopup> m_popup;
    std::unique_ptr<LinkSuggestProvider> m_links;
    QList<Suggestion> m_rows;
    QStringList m_texts;              // Keys/Values/Tags: the text of each row
    QList<LinkSuggestion> m_linkItems;
    bool m_linkMode = false;
    QString m_linkQuery;
    bool m_open = false;
    bool m_enabled = true;
    bool m_settingText = false;
};

} // namespace oc
