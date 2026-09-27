#include "ui/shell/opening_behavior.h"

#include "core/commands/command_registry.h"
#include "core/documents/document.h"
#include "core/properties/frontmatter_editor.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QDir>
#include <QFileInfo>

namespace oc {

namespace {

// The active note has nothing but (optionally) frontmatter and blank lines.
bool activeNoteIsEmpty(Workspace* ws)
{
    auto* mv = ws->activeViewOfType<MarkdownView>();
    Document* doc = mv ? mv->document() : nullptr;
    if (!doc)
        return false;
    const Text& text = doc->text();
    const props::FrontmatterSpan fm = props::findFrontmatter(text);
    for (int i = fm.exists ? fm.closeLine + 1 : 0; i < text.lineCount(); ++i)
        if (!text.line(i).trimmed().isEmpty())
            return false;
    return true;
}

TFile* fileInsensitive(Vault* vault, const QString& path)
{
    if (TFile* f = vault->file(path))
        return f;
    for (TFile* f : vault->files())
        if (f->path().compare(path, Qt::CaseInsensitive) == 0)
            return f;
    return nullptr;
}

} // namespace

void runOpeningBehavior(MainWindow* window, const QString& behavior)
{
    VaultSession* s = window ? window->session() : nullptr;
    Workspace* ws = window ? window->workspace() : nullptr;
    if (!s || !ws || behavior.isEmpty())
        return;
    CommandRegistry* commands = s->commands();
    if (behavior == u"new") {
        if (commands && !activeNoteIsEmpty(ws))
            commands->execute(QStringLiteral("file-explorer:new-file-in-current-tab"));
    } else if (behavior == u"daily") {
        if (commands)
            commands->execute(QStringLiteral("daily-notes")); // unknown while the plugin is off
    } else if (behavior.startsWith(u"file:") && s->vault()) {
        QString path = QDir::cleanPath(behavior.mid(5).trimmed());
        while (path.startsWith(u'/'))
            path.remove(0, 1);
        if (path.isEmpty() || path == u".")
            return;
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".md");
        if (TFile* f = fileInsensitive(s->vault(), path))
            ws->openFile(f, Workspace::NewLeaf::No);
    }
}

} // namespace oc
