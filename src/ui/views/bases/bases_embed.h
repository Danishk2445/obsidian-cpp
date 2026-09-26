#pragma once

// Bases inside notes (Obsidian's iJ embed and the "base" code block post-processor):
//  - "![[Books.base]]" / "![[Books.base#View]]" (the "base" EmbedRegistry extension): the base's
//    first (or named) view with its toolbar; edits are written to the .base file, which the embed
//    re-reads when it changes;
//  - a ```base code block (EmbedRegistry::registerCodeBlockLanguage("base")): the base defined by
//    the block's YAML (leading tabs count as 4 spaces); edits rewrite the block's content;
// "this" is the note showing the embed. The embed is as tall as its content (header + the whole
// table / list / cards; no inner vertical scrolling, like Obsidian's auto-height embeds): the
// live widget reports its height and the note re-lays out (EmbedRegistry::notifyChanged). Before a
// live widget exists (first layout, Reading view, PDF export) the base is evaluated once to size
// and paint a static table.

#include "editor/embeds/embed_registry.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace oc {
class Document;
class VaultSession;
}

namespace oc::basesui {

class BasesController;
class TextIconButton;

// The sessions embeds read from (registerBases adds each window's session).
void addEmbedSession(VaultSession* session);
VaultSession* embedSessionFor(const QString& absPath); // the vault containing absPath; any for ""

class BaseEmbedProvider : public EmbedProvider {
public:
    QSizeF size(const EmbedRequest& request, qreal maxWidth) const override;
    void paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const override;
    QWidget* createWidget(const EmbedContext& context, QWidget* parent) const override;
    quint64 signature(const EmbedRequest& request) const override;
};

class BaseEmbedWidget : public QWidget {
    Q_OBJECT
public:
    BaseEmbedWidget(const EmbedContext& context, QWidget* parent = nullptr);
    ~BaseEmbedWidget() override;

    BasesController* controller() const { return m_controller; }
    const EmbedRequest& request() const { return m_ctx.request; }
    bool isCodeBlock() const { return !m_ctx.request.language.isEmpty(); }
    QString notePath() const;
    TextIconButton* actionButton() const { return m_action; }
    // Write pending edits now (the file / code block is rewritten 300 ms after the last edit).
    void flush();

protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void loadFile();
    void reportHeight();
    void writeBack();
    bool replaceCode(const QString& text);

    EmbedContext m_ctx;
    QString m_source;   // the code block's current content / the file's text
    QString m_vaultPath;
    QPointer<VaultSession> m_session;
    BasesController* m_controller;
    TextIconButton* m_action = nullptr;
    QTimer m_writeTimer;
    QString m_pending;
    QMetaObject::Connection m_modifiedConn;
};

// Registers the "base" extension and code block language with EmbedRegistry (idempotent).
void registerBaseEmbeds();

} // namespace oc::basesui
