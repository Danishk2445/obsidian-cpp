#include "app/scenario_runner.h"

#include "core/commands/command_registry.h"
#include "core/commands/hotkey.h"
#include "core/documents/document.h"
#include "core/util/busy.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>

namespace oc {

namespace {
MarkdownView* activeMarkdown(MainWindow* w)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    WorkspaceLeaf* leaf = ws ? ws->activeRootLeaf() : nullptr;
    return leaf ? qobject_cast<MarkdownView*>(leaf->view()) : nullptr;
}

QString unescape(const QString& s)
{
    QString out;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] == u'\\' && i + 1 < s.size()) {
            const QChar c = s[++i];
            out += c == u'n' ? QChar(u'\n') : c == u't' ? QChar(u'\t') : c;
        } else {
            out += s[i];
        }
    }
    return out;
}
} // namespace

ScenarioRunner::ScenarioRunner(MainWindow* window, const QString& scriptPath, QObject* parent)
    : QObject(parent)
    , m_window(window)
    , m_dir(QFileInfo(scriptPath).absolutePath())
{
    QFile f(scriptPath);
    if (f.open(QIODevice::ReadOnly))
        m_lines = QString::fromUtf8(f.readAll()).split(u'\n');
    else
        qCritical("oc: cannot read script %s", qPrintable(scriptPath));
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, &ScenarioRunner::step);
}

void ScenarioRunner::start()
{
    m_clock.start();
    m_timer.start();
}

void ScenarioRunner::step()
{
    const qint64 now = m_clock.elapsed();
    if (Busy::idle()) {
        if (m_quietSince < 0)
            m_quietSince = now;
    } else {
        m_quietSince = -1;
    }
    const bool settled = (m_quietSince >= 0 && now - m_quietSince >= 100) || now - m_quietSince > 20000;
    if (!settled || now < m_waitUntil)
        return;
    while (m_next < m_lines.size()) {
        const QString line = m_lines[m_next++].trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        if (line == u"quit") {
            finish(0);
            return;
        }
        if (!run(line)) {
            qCritical("oc: script line %d failed: %s", m_next, qPrintable(line));
            finish(3);
            return;
        }
        m_quietSince = -1; // wait for the effects of this command
        return;
    }
    finish(0);
}

void ScenarioRunner::finish(int code)
{
    m_timer.stop();
    QApplication::exit(code);
}

QWidget* ScenarioRunner::keyTarget() const
{
    if (QWidget* f = QApplication::focusWidget())
        return f;
    if (m_window && m_window->focusWidget())
        return m_window->focusWidget();
    if (MarkdownView* mv = activeMarkdown(m_window))
        return mv->editor();
    return m_window;
}

void ScenarioRunner::sendKey(int key, Qt::KeyboardModifiers mods, const QString& text)
{
    QWidget* target = keyTarget();
    if (!target)
        return;
    QPointer<QWidget> guard = target;
    QKeyEvent so(QEvent::ShortcutOverride, key, mods, text);
    so.ignore();
    QApplication::sendEvent(target, &so);
    if (!guard)
        return;
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(target, &press);
    if (!guard)
        return;
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QApplication::sendEvent(target, &release);
}

bool ScenarioRunner::run(const QString& line)
{
    const qsizetype sp = line.indexOf(u' ');
    const QString cmd = sp < 0 ? line : line.left(sp);
    const QString arg = sp < 0 ? QString() : line.mid(sp + 1);
    if (cmd == u"wait") {
        m_waitUntil = m_clock.elapsed() + arg.toInt();
        return true;
    }
    if (cmd == u"open") {
        const QStringList parts = arg.split(u' ');
        VaultSession* s = m_window ? m_window->session() : nullptr;
        TFile* f = s && s->vault() ? s->vault()->file(parts.value(0)) : nullptr;
        if (!f)
            return false;
        m_window->workspace()->openFile(f, Workspace::NewLeaf::No);
        if (MarkdownView* mv = activeMarkdown(m_window)) {
            const QString mode = parts.value(1);
            if (mode == u"source")
                mv->setMode(EditorView::Mode::Source);
            else if (mode == u"reading")
                mv->setMode(EditorView::Mode::Reading);
            else if (mode == u"live")
                mv->setMode(EditorView::Mode::LivePreview);
            mv->editor()->setFocus();
        }
        return true;
    }
    if (cmd == u"cursor") {
        MarkdownView* mv = activeMarkdown(m_window);
        if (!mv || !mv->editor()->document())
            return false;
        const Text& t = mv->editor()->document()->text();
        if (arg == u"end") {
            mv->editor()->setCursorPosition(t.length());
        } else {
            const QStringList p = arg.split(u' ');
            const int l = std::clamp(p.value(0).toInt(), 0, t.lineCount() - 1);
            const qsizetype col = p.value(1) == u"end" ? t.lineLength(l) : std::min<qsizetype>(p.value(1).toInt(), t.lineLength(l));
            mv->editor()->setCursorPosition(t.lineStart(l) + col);
        }
        mv->editor()->setFocus();
        return true;
    }
    if (cmd == u"type") {
        for (const QChar c : unescape(arg)) {
            if (c == u'\n')
                sendKey(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
            else if (c == u'\t')
                sendKey(Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
            else
                sendKey(c.unicode() < 0x7f ? int(c.toUpper().unicode()) : int(Qt::Key_unknown), Qt::NoModifier, QString(c)); // Qt key codes = uppercase ASCII
        }
        return true;
    }
    if (cmd == u"click") {
        QWidget* target = nullptr;
        for (QWidget* w : m_window->findChildren<QWidget*>(arg))
            if (w->isVisible())
                target = w;
        if (!target)
            return false;
        const QPointF local = QRectF(target->rect()).center();
        const QPointF global = target->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &release);
        return true;
    }
    if (cmd == u"key") {
        const Hotkey hk = Hotkey::parse(arg);
        const QKeySequence seq = hk.toSequence();
        if (!hk.isValid() || seq.isEmpty())
            return false;
        const QKeyCombination kc = seq[0];
        QString text;
        if (kc.key() == Qt::Key_Return || kc.key() == Qt::Key_Enter)
            text = QStringLiteral("\r");
        else if (kc.key() == Qt::Key_Tab)
            text = QStringLiteral("\t");
        else if (kc.key() == Qt::Key_Escape)
            text = QString(QChar(0x1b));
        sendKey(kc.key(), kc.keyboardModifiers(), text);
        return true;
    }
    if (cmd == u"command") {
        VaultSession* s = m_window ? m_window->session() : nullptr;
        return s && s->commands() && s->commands()->execute(arg);
    }
    if (cmd == u"screenshot") {
        if (!m_window)
            return false;
        const QString out = QFileInfo(arg).isAbsolute() ? arg : m_dir + u'/' + arg;
        m_window->repaint();
        return m_window->grab().save(out);
    }
    if (cmd == u"expect-line") {
        MarkdownView* mv = activeMarkdown(m_window);
        const qsizetype sp2 = arg.indexOf(u' ');
        if (!mv || !mv->editor()->document())
            return false;
        const Text& t = mv->editor()->document()->text();
        int l = (sp2 < 0 ? arg : arg.left(sp2)).toInt();
        if (l < 0)
            l += t.lineCount(); // -1 = last line
        const QString want = sp2 < 0 ? QString() : unescape(arg.mid(sp2 + 1));
        const QString got = l >= 0 && l < t.lineCount() ? t.line(l) : QString();
        if (got != want) {
            qCritical("oc: expect-line %d: got \"%s\", want \"%s\"", l, qPrintable(got), qPrintable(want));
            return false;
        }
        return true;
    }
    return false;
}

} // namespace oc
