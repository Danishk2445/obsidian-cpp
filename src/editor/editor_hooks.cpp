#include "editor/editor_hooks.h"

namespace oc {

EditorHooks& EditorHooks::global()
{
    static EditorHooks h;
    return h;
}

int EditorHooks::addInputHandler(InputHandler h)
{
    m_entries.push_back({m_next, std::move(h), {}});
    return m_next++;
}

int EditorHooks::addPasteHandler(PasteHandler h)
{
    m_entries.push_back({m_next, {}, std::move(h)});
    return m_next++;
}

void EditorHooks::remove(int id)
{
    m_entries.removeIf([id](const Entry& e) { return e.id == id; });
}

bool EditorHooks::runInput(EditorView& view, qsizetype from, qsizetype to, const QString& text) const
{
    const QList<Entry> entries = m_entries; // handlers may unregister themselves
    for (const Entry& e : entries)
        if (e.input && e.input(view, from, to, text))
            return true;
    return false;
}

bool EditorHooks::runPaste(EditorView& view, const QMimeData& data) const
{
    const QList<Entry> entries = m_entries;
    for (const Entry& e : entries)
        if (e.paste && e.paste(view, data))
            return true;
    return false;
}

} // namespace oc
