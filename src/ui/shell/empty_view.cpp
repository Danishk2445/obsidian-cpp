#include "ui/shell/empty_view.h"

#include "render/theme/theme.h"

#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace oc {

EmptyView::EmptyView(QWidget* parent)
    : QWidget(parent)
{
    const ThemePtr t = currentTheme();
    auto* v = new QVBoxLayout(this);
    v->addStretch(2);
    auto* title = new QLabel(tr("No file is open"), this);
    QFont tf = t->interfaceFont(t->px(QStringLiteral("--font-ui-large"), -1, 20));
    tf.setWeight(QFont::DemiBold);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);
    title->setProperty("ocText", QStringLiteral("muted"));
    v->addWidget(title);
    v->addSpacing(18);
    const QColor accent = t->color(QStringLiteral("--text-accent"));
    auto addAction = [&](const QString& id, const QString& text, const QString& hotkey) {
        auto* l = new QLabel(this);
        l->setTextFormat(Qt::RichText);
        l->setText(QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">%3%4</a>")
                       .arg(id, accent.name(), text.toHtmlEscaped(), hotkey.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(hotkey)));
        l->setAlignment(Qt::AlignCenter);
        l->setFont(t->interfaceFont(t->px(QStringLiteral("--font-ui-medium"), -1, 15)));
        l->setCursor(Qt::PointingHandCursor);
        connect(l, &QLabel::linkActivated, this, &EmptyView::actionTriggered);
        v->addWidget(l);
        v->addSpacing(6);
    };
    addAction(QStringLiteral("new-file"), tr("Create new note"), QStringLiteral("Ctrl + N"));
    addAction(QStringLiteral("open-switcher"), tr("Go to file"), QStringLiteral("Ctrl + O"));
    addAction(QStringLiteral("recent"), tr("See recent files"), QStringLiteral("Ctrl + O"));
    addAction(QStringLiteral("close"), tr("Close"), QString());
    v->addStretch(3);
}

void EmptyView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-primary")));
}

} // namespace oc
