#pragma once

#include "ui/views/markdown/editor_suggest.h"

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <functional>

class QLineEdit;

namespace oc {

class OverlayLayer;
class SearchInput;
class SuggestionList;
class SuggestPopup;
class VaultSession;

// Obsidian's FolderSuggest for a text input (AbstractInputSuggest): while the input has focus, a
// popup under it lists the vault's folders fuzzy-matching the typed text, best first, at most 100
// ("/" too when includeRoot). ↑/↓ move the selection, Enter/Tab or a click fill the input with the
// folder's path (emitting chosen()), Esc, a click elsewhere or focus loss close it.
class FolderSuggest : public QObject {
    Q_OBJECT
public:
    FolderSuggest(QLineEdit* input, VaultSession* session, OverlayLayer* layer, bool includeRoot = false);
    ~FolderSuggest() override;

    // Folder paths matching `query` in display order.
    QStringList suggestions(const QString& query) const;

    void open();
    void close();
    bool isOpen() const;
    SuggestPopup* popup() const { return m_popup; }
    void choose(int index);

signals:
    void chosen(const QString& path);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void refresh();
    void reposition();

    QPointer<QLineEdit> m_input;
    QPointer<VaultSession> m_session;
    QPointer<OverlayLayer> m_layer;
    QPointer<SuggestPopup> m_popup;
    QStringList m_current;
    bool m_includeRoot;
};

// Obsidian 1.13's file combobox (the `file` setting control): a dropdown-like button (at most 250 px)
// showing the chosen file as "folder/ name" (folders muted, no extension) or the placeholder, with a
// clear button when set. Clicking opens a popup with a search input over the vault's files (fuzzy
// matched, filtered) — Enter or a click picks one.
class FilePicker : public QWidget {
    Q_OBJECT
public:
    FilePicker(VaultSession* session, OverlayLayer* layer, QWidget* parent = nullptr);
    ~FilePicker() override;

    void setFilter(std::function<bool(const QString& path)> filter) { m_filter = std::move(filter); }
    void setPlaceholder(const QString& text);
    QString value() const { return m_value; }
    void setValue(const QString& path); // programmatic: no signal
    void choose(const QString& path);   // a user choice ("" clears): emits valueChanged when it changes

    QStringList suggestions(const QString& query) const;
    void open();
    void close();
    bool isOpen() const;
    SearchInput* searchInput() const;
    SuggestionList* list() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void valueChanged(const QString& path);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRect clearRect() const;
    void refresh();

    QPointer<VaultSession> m_session;
    QPointer<OverlayLayer> m_layer;
    QPointer<QWidget> m_popup;
    SearchInput* m_search = nullptr;
    SuggestionList* m_list = nullptr;
    std::function<bool(const QString&)> m_filter;
    QString m_value;
    QString m_placeholder;
    QStringList m_current;
    bool m_hover = false;
};

} // namespace oc
