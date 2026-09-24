#pragma once

// Widgets of the search view (internal to src/ui/navigation; exposed for tests).

#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"

#include <QAbstractButton>
#include <QList>
#include <QWidget>

namespace oc {

// .clickable-icon that shows .is-active like Obsidian: --icon-color-active on --background-modifier-active-hover.
class ActiveIconButton : public IconButton {
    Q_OBJECT
public:
    explicit ActiveIconButton(const QString& icon, const QString& tooltip = {}, QWidget* parent = nullptr);
    void setActiveBackground(bool on) { m_activeBg = on; update(); }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    bool m_activeBg = true;
};

// The global search input (.global-search-input-container): SearchInput plus the "Match case" toggle as an
// .input-right-decorator, which moves left of the clear button while there is text.
class GlobalSearchInput : public SearchInput {
    Q_OBJECT
public:
    explicit GlobalSearchInput(QWidget* parent = nullptr);
    ActiveIconButton* matchCaseButton() const { return m_matchCase; }

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void adjust();
    ActiveIconButton* m_matchCase;
};

// .search-info-container: the "Explain search terms" text, nested items indented with a left border.
class ExplainPanel : public QWidget {
    Q_OBJECT
public:
    explicit ExplainPanel(QWidget* parent = nullptr);
    void setText(const QString& text); // explainSearchQuery() output: 4 spaces per nesting level
    QString text() const { return m_text; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override { return {200, heightForWidth(width() > 0 ? width() : 280)}; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    struct Line {
        int level = 0;
        QString text;
    };
    QString m_text;
    QList<Line> m_lines;
};

// The sort order <select class="dropdown"> of .search-results-info: right-aligned muted text with the up/down
// chevrons; opens a menu of the options.
class SortDropdown : public QWidget {
    Q_OBJECT
public:
    explicit SortDropdown(QWidget* parent = nullptr);
    void setOptions(const QStringList& labels);
    void setCurrentIndex(int index);
    int currentIndex() const { return m_index; }
    QString currentText() const { return m_labels.value(m_index); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {40, 24}; }

signals:
    void activated(int index);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QStringList m_labels;
    int m_index = 0;
    bool m_hover = false;
};

} // namespace oc
