#include "ui/views/markdown/properties/properties_commands.h"

#include "core/commands/command_registry.h"
#include "core/documents/document.h"
#include "core/properties/frontmatter_editor.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/markdown/properties/properties_widget.h"

namespace oc {

namespace {

Command viewCommand(const char* id, const QString& name, const char* icon, std::function<bool(MarkdownView&, bool checking)> run,
                    QList<Hotkey> hotkeys = {})
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    c.defaultHotkeys = std::move(hotkeys);
    c.editorCheckCallback = [run = std::move(run)](bool checking, EditorContext& ec) {
        auto* view = qobject_cast<MarkdownView*>(ec.view);
        if (!view || !view->document())
            return false;
        return run(*view, checking);
    };
    return c;
}

} // namespace

void registerPropertyCommands(CommandRegistry& r)
{
    r.add(viewCommand("markdown:add-metadata-property", QObject::tr("Add file property"), "plus-circle",
                      [](MarkdownView& v, bool checking) {
                          if (!v.canShowProperties())
                              return false;
                          if (!checking)
                              v.addProperty();
                          return true;
                      },
                      {Hotkey::parse(QStringLiteral("Mod+;"))}));
    r.add(viewCommand("markdown:add-alias", QObject::tr("Add alias"), "forward", [](MarkdownView& v, bool checking) {
        if (!v.canShowProperties())
            return false;
        if (!checking)
            v.addProperty(QStringLiteral("aliases"));
        return true;
    }));
    r.add(viewCommand("markdown:clear-metadata-properties", QObject::tr("Clear file properties"), "package-x", [](MarkdownView& v, bool checking) {
        if (v.document()->isReadOnly())
            return false;
        if (!checking) {
            // Obsidian clears the block whatever it contains (even invalid YAML): edit the text directly.
            Document* doc = v.document();
            props::FrontmatterEditor ed(doc->text());
            ed.clear();
            const QList<ChangeSpec> specs = ed.changes();
            if (!specs.isEmpty()) {
                Transaction tr;
                tr.changes = ChangeSet::of(specs, doc->text().length());
                tr.userEvent = QStringLiteral("input.properties");
                tr.scrollIntoView = false;
                doc->dispatch(tr, v.editor(), v.editor()->selection());
            }
        }
        return true;
    }));
    r.add(viewCommand("editor:toggle-fold-properties", QObject::tr("Toggle fold properties in current file"), "diff",
                      [](MarkdownView& v, bool checking) {
                          PropertiesWidget* p = v.properties();
                          if (!p || !p->isVisible() || p->isInvalid())
                              return false;
                          if (!checking)
                              p->setCollapsed(!p->isCollapsed());
                          return true;
                      }));
}

} // namespace oc
