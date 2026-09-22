#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

class QLineEdit;

namespace oc {

class EditorSearch;
class EditorView;
class IconButton;
class SearchInput;

// Obsidian's in-file search bar (.document-search-container) above a markdown editor: "Find..." with
// the "i / n" counter, previous / next / find all / close, and in replace mode a "Replace..." row
// with replace / replace all. Keys while it has focus: F3 / Mod+G next, Shift+F3 / Mod+Shift+G
// previous, Enter next (or replace + next in the replace input), Shift+Enter previous, Alt+Enter find
// all, Mod+Alt+Enter replace all, Tab switches inputs, Escape closes (selecting the current match).
class EditorSearchBar : public QWidget {
    Q_OBJECT
public:
    explicit EditorSearchBar(EditorView* editor, QWidget* parent = nullptr);
    ~EditorSearchBar() override;

    void open(bool replace); // prefilled with the editor's selection
    void close();
    bool isReplaceMode() const { return m_replace; }
    bool isOpen() const { return !isHidden(); }

    EditorSearch* search() const { return m_search; }
    SearchInput* findInput() const { return m_find; }
    QLineEdit* replaceInput() const { return m_replaceInput; }
    QString countText() const;

    void findNext();
    void findPrevious();

signals:
    void closed();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void onQueryChanged();
    void updateCount();
    bool handleKey(QKeyEvent* e);

    QPointer<EditorView> m_editor;
    EditorSearch* m_search;
    SearchInput* m_find;
    QLineEdit* m_replaceInput;
    QWidget* m_replaceRow;
    IconButton* m_prev;
    IconButton* m_next;
    IconButton* m_all;
    IconButton* m_close;
    IconButton* m_replaceOne;
    IconButton* m_replaceAll;
    QTimer m_countTimer;
    bool m_replace = false;
};

} // namespace oc
