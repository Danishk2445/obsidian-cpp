#pragma once
// Shared helpers for the vim tests: an offscreen EditorView on a real Document, driven by real key
// events (ShortcutOverride + KeyPress + KeyRelease) through the editor's key hook, like typing.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/markdown_commands.h"
#include "editor/vim/vim_controller.h"
#include "editor/vim/vim_keys.h"
#include "editor/vim/vim_panel.h"
#include "editor/vim/vim_registers.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <memory>

namespace oc::test {

// Sends one vim key as a real key event to `target`.
inline void sendVimKey(QWidget* target, const QString& key)
{
    int qtKey = 0;
    Qt::KeyboardModifiers mods;
    QString text;
    if (!vim::keyToQt(key, &qtKey, &mods, &text)) {
        ADD_FAILURE() << "unknown key " << key.toStdString();
        return;
    }
    QKeyEvent so(QEvent::ShortcutOverride, qtKey, mods, text);
    QApplication::sendEvent(target, &so);
    QKeyEvent press(QEvent::KeyPress, qtKey, mods, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, qtKey, mods, text);
    QApplication::sendEvent(target, &release);
}

struct VimHarness {
    QTemporaryDir dir;
    std::unique_ptr<LocalFileStorage> storage;
    std::shared_ptr<Document> doc;
    std::unique_ptr<EditorView> view;
    std::unique_ptr<VimController> vim;
    QString clipboard;
    int fileNo = 0;

    VimHarness()
    {
        storage = std::make_unique<LocalFileStorage>(dir.path());
        view = std::make_unique<EditorView>();
        view->resize(700, 500);
        view->setMode(EditorView::Mode::Source);
        view->show();
        view->activateWindow();
        view->setFocus();
        vim = std::make_unique<VimController>(view.get());
        vim::Clipboard cb;
        cb.read = [this] { return clipboard; };
        cb.write = [this](const QString& t) { clipboard = t; };
        VimController::setClipboard(cb);
    }
    ~VimHarness()
    {
        vim.reset();
        view->setDocument(nullptr);
    }

    // Opens `content` ('|' = cursor) in normal mode.
    void open(QString content)
    {
        const qsizetype cursor = std::max<qsizetype>(0, content.indexOf(u'|'));
        content.remove(cursor, content.indexOf(u'|') >= 0 ? 1 : 0);
        const QString name = QStringLiteral("note%1.md").arg(++fileNo);
        QFile f(dir.path() + u'/' + name);
        if (!f.open(QIODevice::WriteOnly))
            qFatal("fixture");
        f.write(content.toUtf8());
        f.close();
        view->setDocument(nullptr);
        doc = std::make_shared<Document>(storage.get(), name);
        if (!doc->load())
            qFatal("load");
        view->setDocument(doc);
        view->setCursorPosition(cursor, false);
        vim->documentChanged();
    }

    // Types keys in vim notation; keys go to the vim prompt while it is open.
    void keys(const QString& keys)
    {
        for (const QString& k : vim::splitKeys(keys)) {
            vim::VimPanel* panel = vim->panel();
            QWidget* target = panel && panel->isPromptOpen() ? static_cast<QWidget*>(panel->input()) : static_cast<QWidget*>(view.get());
            sendVimKey(target, k);
        }
    }

    QString text() const { return doc->text().toString(); }
    // The text with '|' at the main head and '^' at its anchor (when they differ).
    QString marked() const
    {
        QString t = text();
        const SelRange r = view->selection().main();
        if (r.anchor == r.head)
            return t.insert(r.head, u'|');
        if (r.anchor > r.head) {
            t.insert(r.anchor, u'^');
            return t.insert(r.head, u'|');
        }
        t.insert(r.head, u'|');
        return t.insert(r.anchor, u'^');
    }
    QString ranges() const
    {
        QStringList parts;
        for (const SelRange& r : view->selection().ranges())
            parts << QStringLiteral("%1:%2").arg(r.anchor).arg(r.head);
        return parts.join(u' ') + QStringLiteral(" main=%1").arg(view->selection().mainIndex());
    }
};

} // namespace oc::test
