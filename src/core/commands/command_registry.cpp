#include "core/commands/command_registry.h"

#include <QApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

#include <algorithm>

namespace oc {

CommandRegistry::CommandRegistry(QObject* parent)
    : QObject(parent)
{
}

void CommandRegistry::add(Command cmd)
{
    const QString id = cmd.id;
    if (!m_commands.contains(id))
        m_order.push_back(id);
    m_commands.insert(id, std::move(cmd));
    rebuildIndex();
    emit commandsChanged();
}

void CommandRegistry::remove(const QString& id)
{
    m_commands.remove(id);
    m_order.removeAll(id);
    rebuildIndex();
    emit commandsChanged();
}

const Command* CommandRegistry::find(const QString& id) const
{
    auto it = m_commands.constFind(id);
    return it == m_commands.cend() ? nullptr : &*it;
}

QList<const Command*> CommandRegistry::all() const
{
    QList<const Command*> out;
    for (const QString& id : m_order)
        out.push_back(&*m_commands.constFind(id));
    std::sort(out.begin(), out.end(), [](const Command* a, const Command* b) { return a->name.localeAwareCompare(b->name) < 0; });
    return out;
}

bool CommandRegistry::isAvailable(const QString& id) const
{
    const Command* c = find(id);
    if (!c || (m_filter && !m_filter(*c)))
        return false;
    if (c->callback)
        return true;
    if (c->checkCallback)
        return c->checkCallback(true);
    if (c->editorCallback || c->editorCheckCallback) {
        EditorContext ctx = activeEditor();
        if (!ctx.editor)
            return false;
        if (c->editorCheckCallback)
            return c->editorCheckCallback(true, ctx);
        return true;
    }
    return false;
}

bool CommandRegistry::execute(const QString& id)
{
    const Command* c = find(id);
    if (!c || !isAvailable(id))
        return false;
    const Command cmd = *c; // copy: the callback may modify the registry
    if (cmd.callback)
        cmd.callback();
    else if (cmd.checkCallback)
        cmd.checkCallback(false);
    else {
        EditorContext ctx = activeEditor();
        if (cmd.editorCheckCallback)
            cmd.editorCheckCallback(false, ctx);
        else if (cmd.editorCallback)
            cmd.editorCallback(ctx);
    }
    m_recent.removeAll(id);
    m_recent.prepend(id);
    while (m_recent.size() > 50)
        m_recent.removeLast();
    emit executed(id);
    return true;
}

QList<Hotkey> CommandRegistry::hotkeys(const QString& id) const
{
    auto it = m_custom.constFind(id);
    if (it != m_custom.cend())
        return *it;
    const Command* c = find(id);
    return c ? c->defaultHotkeys : QList<Hotkey>{};
}

void CommandRegistry::setCustomHotkeys(const QString& id, const QList<Hotkey>& keys)
{
    m_custom.insert(id, keys);
    rebuildIndex();
    emit hotkeysChanged();
}

void CommandRegistry::resetHotkeys(const QString& id)
{
    m_custom.remove(id);
    rebuildIndex();
    emit hotkeysChanged();
}

QStringList CommandRegistry::commandsFor(const Hotkey& hk) const { return m_index.value(hk); }

void CommandRegistry::rebuildIndex()
{
    m_index.clear();
    for (const QString& id : m_order) {
        for (const Hotkey& hk : hotkeys(id))
            if (hk.isValid())
                m_index[hk].push_back(id);
    }
    // custom hotkeys for commands registered later are indexed when they appear
}

void CommandRegistry::loadHotkeys(const QJsonObject& json)
{
    m_custom.clear();
    for (auto it = json.begin(); it != json.end(); ++it) {
        QList<Hotkey> keys;
        for (const QJsonValue& v : it.value().toArray()) {
            const Hotkey h = Hotkey::fromJson(v.toObject());
            if (h.isValid())
                keys.push_back(h);
        }
        m_custom.insert(it.key(), keys);
    }
    rebuildIndex();
    emit hotkeysChanged();
}

QJsonObject CommandRegistry::saveHotkeys() const
{
    QJsonObject out;
    for (auto it = m_custom.cbegin(); it != m_custom.cend(); ++it) {
        QJsonArray arr;
        for (const Hotkey& h : *it)
            arr.append(h.toJson());
        out.insert(it.key(), arr);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

HotkeyDispatcher::HotkeyDispatcher(CommandRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
{
    qApp->installEventFilter(this);
}

namespace {
bool isTextInput(QObject* o)
{
    return qobject_cast<QLineEdit*>(o) || qobject_cast<QTextEdit*>(o) || qobject_cast<QPlainTextEdit*>(o);
}

bool insideCapturingOverlay(QWidget* w)
{
    for (QWidget* p = w; p; p = p->parentWidget())
        if (p->property("ocCapturesKeys").toBool())
            return true;
    return false;
}

// Standard text-editing chords that a focused text input should keep.
bool isTextEditingChord(const Hotkey& h)
{
    if (!h.mod && !h.alt && !h.meta)
        return true; // plain keys and Shift+key
    if (h.mod && !h.alt && !h.meta) {
        static const QStringList keys = {QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("V"), QStringLiteral("X"),
                                         QStringLiteral("Z"), QStringLiteral("Y"), QStringLiteral("ArrowLeft"), QStringLiteral("ArrowRight"),
                                         QStringLiteral("Backspace"), QStringLiteral("Delete"), QStringLiteral("Home"), QStringLiteral("End")};
        return keys.contains(h.key);
    }
    return false;
}
} // namespace

bool HotkeyDispatcher::tryDispatch(QObject* target, QKeyEvent* ke, bool execute)
{
    auto* w = qobject_cast<QWidget*>(target);
    if (!w)
        return false;
    if (m_window && w->window() != m_window)
        return false;
    if (insideCapturingOverlay(w))
        return false;
    // An open editor suggest popup owns the navigation keys of its editor.
    if (w->property("ocSuggestActive").toBool()) {
        switch (ke->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
        case Qt::Key_Escape:
            return false;
        default:
            break;
        }
    }
    const Hotkey hk = Hotkey::fromEvent(ke);
    if (!hk.isValid())
        return false;
    if (isTextInput(w) && isTextEditingChord(hk))
        return false;
    const QStringList ids = m_registry->commandsFor(hk);
    for (const QString& id : ids) {
        const Command* c = m_registry->find(id);
        if (!c)
            continue;
        if (ke->isAutoRepeat() && !c->repeatable)
            continue;
        if (!m_registry->isAvailable(id))
            continue;
        if (execute)
            m_registry->execute(id);
        return true;
    }
    return false;
}

bool HotkeyDispatcher::eventFilter(QObject* obj, QEvent* ev)
{
    if (!m_enabled)
        return false;
    if (ev->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (tryDispatch(obj, ke, false)) {
            ke->accept(); // we'll get a KeyPress for it
            return true;
        }
        return false;
    }
    if (ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (tryDispatch(obj, ke, true)) {
            ke->accept();
            return true;
        }
    }
    return false;
}

} // namespace oc
