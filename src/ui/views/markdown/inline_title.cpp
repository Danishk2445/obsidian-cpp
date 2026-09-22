#include "ui/views/markdown/inline_title.h"

#include "render/theme/theme.h"

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QTextBlock>

namespace oc {

InlineTitle::InlineTitle(QWidget* parent)
    : QTextEdit(parent)
{
    setObjectName(QStringLiteral("ocInlineTitle"));
    setFrameShape(QFrame::NoFrame);
    setAcceptRichText(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setTabChangesFocus(true);
    setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAutoFillBackground(false);
    document()->setDocumentMargin(0);
    setProperty("ocCapturesKeys", false);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, &InlineTitle::applyStyle);
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { updateGeometry(); });
    applyStyle();
}

void InlineTitle::applyStyle()
{
    const ThemePtr t = currentTheme();
    // .inline-title: --inline-title-size/-weight/-line-height/-color (= the h1 values), letter-spacing -0.015em
    const double base = t->px(QStringLiteral("--font-text-size"), -1, 16);
    const double size = t->px(QStringLiteral("--inline-title-size"), base, base * 1.618);
    QFont f = t->textFont(size);
    f.setWeight(QFont::Weight(int(t->number(QStringLiteral("--inline-title-weight"), 700))));
    f.setLetterSpacing(QFont::AbsoluteSpacing, -0.015 * size);
    m_marginBottom = int(std::round(t->px(QStringLiteral("--inline-title-margin-bottom"), size, size * 0.5)));
    document()->setDefaultFont(f);
    setFont(f);
    QPalette pal = palette();
    pal.setColor(QPalette::Text, t->color(QStringLiteral("--inline-title-color"), t->color(QStringLiteral("--text-normal"))));
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Highlight, t->color(QStringLiteral("--text-selection")));
    pal.setColor(QPalette::HighlightedText, t->color(QStringLiteral("--text-normal")));
    setPalette(pal);
    setStyleSheet(QStringLiteral("QTextEdit#ocInlineTitle { background: transparent; border: none; padding: 0; }"));
    // line-height 1.2 on every block
    const QString text = toPlainText();
    QTextCursor c(document());
    c.select(QTextCursor::Document);
    QTextBlockFormat bf;
    bf.setLineHeight(t->number(QStringLiteral("--inline-title-line-height"), 1.2) * 100, QTextBlockFormat::ProportionalHeight);
    c.mergeBlockFormat(bf);
    updateGeometry();
    update();
}

int InlineTitle::bottomMargin() const { return m_marginBottom; }

void InlineTitle::setTitle(const QString& title)
{
    m_title = title;
    if (toPlainText() != title) {
        const QSignalBlocker b(this);
        setPlainText(title);
        applyStyle();
    }
}

QString InlineTitle::editedText() const { return toPlainText().trimmed(); }

void InlineTitle::setEditable(bool editable)
{
    m_editable = editable;
    setReadOnly(!editable);
    setTextInteractionFlags(editable ? Qt::TextEditorInteraction : Qt::TextSelectableByMouse);
    setCursor(editable ? Qt::IBeamCursor : Qt::ArrowCursor);
}

int InlineTitle::heightForWidth(int w) const
{
    QTextDocument* doc = document()->clone();
    doc->setTextWidth(std::max(10, w));
    const int h = int(std::ceil(doc->size().height()));
    delete doc;
    return std::max(h, fontMetrics().height());
}

QSize InlineTitle::sizeHint() const { return {400, heightForWidth(width() > 0 ? width() : 700)}; }

void InlineTitle::beginRename(bool selectAll)
{
    setFocus(Qt::OtherFocusReason);
    QTextCursor c = textCursor();
    if (selectAll)
        c.select(QTextCursor::Document);
    else
        c.movePosition(QTextCursor::End);
    setTextCursor(c);
}

void InlineTitle::commit()
{
    const QString text = editedText();
    if (!m_editable || text == m_title)
        return;
    if (text.isEmpty()) {
        setTitle(m_title); // Obsidian refuses an empty name
        return;
    }
    emit renameRequested(text);
}

void InlineTitle::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        commit();
        emit leaveToEditor();
        return;
    case Qt::Key_Escape:
        setTitle(m_title);
        emit leaveToEditor();
        return;
    case Qt::Key_Down:
        if (textCursor().atEnd()) {
            emit leaveToEditor();
            return;
        }
        break;
    default:
        break;
    }
    QTextEdit::keyPressEvent(e);
}

void InlineTitle::focusOutEvent(QFocusEvent* e)
{
    QTextEdit::focusOutEvent(e);
    commit();
}

void InlineTitle::insertFromMimeData(const QMimeData* source)
{
    QString text = source->text();
    text.replace(QLatin1Char('\n'), QLatin1Char(' ')).remove(QLatin1Char('\r'));
    insertPlainText(text);
}

} // namespace oc
