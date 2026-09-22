#pragma once

#include "markdown/syntax_tree.h"
#include "render/layout/decoration.h"
#include "render/layout/display_block.h"
#include "render/layout/style_sheet.h"

#include <QList>
#include <QTextLayout>

#include <memory>

namespace oc {

class LivePreviewDecorator;
class NestedRenderer;
namespace md {
class DocSource;
}

// Display math ($$ ... $$) rendered centred.
class MathBlockWidget : public BlockWidget {
public:
    MathBlockWidget(QString tex, qsizetype docStart, StyleSheetPtr styles);
    qreal layout(qreal width) override;
    void paint(QPainter* p, const QRectF& rect) override;
    quint64 signature() const override;
    qsizetype docPosAt(const QPointF&) const override { return m_docStart; }

private:
    QString m_tex;
    qsizetype m_docStart;
    StyleSheetPtr m_styles;
    qreal m_height = 0;
};

// A GFM table rendered like Obsidian's (bordered cells, bold header, inline markdown in cells).
class TableWidget : public BlockWidget {
public:
    TableWidget(const md::DocSource* doc, int tableNode, const LivePreviewDecorator* deco, StyleSheetPtr styles);
    qreal layout(qreal width) override;
    void paint(QPainter* p, const QRectF& rect) override;
    quint64 signature() const override { return m_signature; }
    qsizetype docPosAt(const QPointF& local) const override;
    QString linkAt(const QPointF& local) const override;

private:
    struct Cell {
        int line = 0;
        int docCol = 0;          // column of the cell text start in its line
        qsizetype docStart = 0;  // absolute doc offset of the cell text
        DisplayBlock block;
        std::shared_ptr<QTextLayout> layout;
        QRectF rect;             // relative to the widget
        bool header = false;
        Qt::Alignment align;
    };
    void layoutCell(Cell& c, qreal width);

    StyleSheetPtr m_styles;
    QList<QList<Cell>> m_rows;
    int m_columns = 0;
    quint64 m_signature = 0;
    qreal m_width = -1;
    qreal m_height = 0;
    qsizetype m_docStart = 0;
};

// A callout ("> [!type] Title") with its body rendered by a nested renderer.
class CalloutWidget : public BlockWidget {
public:
    CalloutWidget(const md::DocSource* doc, int calloutNode, const LivePreviewDecorator* deco, StyleSheetPtr styles, int depth);
    ~CalloutWidget() override;
    qreal layout(qreal width) override;
    void paint(QPainter* p, const QRectF& rect) override;
    quint64 signature() const override { return m_signature; }
    qsizetype docPosAt(const QPointF& local) const override;
    bool click(const QPointF& local, Qt::KeyboardModifiers mods) override;
    QString linkAt(const QPointF& local) const override;

    static QString colorToken(const QString& type);
    static QString iconName(const QString& type);

private:
    QString m_type;
    QString m_title;
    bool m_foldable = false;
    bool m_folded = false;
    int m_firstLine = 0;
    QList<int> m_bodyLines;      // source line of each nested line
    QList<int> m_bodyCols;       // column offset (after '>' markers) of each nested line
    const md::DocSource* m_doc;
    StyleSheetPtr m_styles;
    std::unique_ptr<NestedRenderer> m_body;
    quint64 m_signature = 0;
    qreal m_width = -1;
    qreal m_height = 0;
    qreal m_headerH = 0;
    QRectF m_bodyRect;
};

} // namespace oc
