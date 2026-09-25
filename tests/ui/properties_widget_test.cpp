// The properties editor in a markdown view (offscreen): widgets per type, editing values through one
// undoable transaction each, lists, checkbox, type changes (types.json in the clone config), Mod+;,
// keyboard navigation, Reading view, invalid YAML, the frontmatter hidden behind the widget, links,
// and screenshots (only with OC_SHOT_DIR).
#include "app_test_support.h"
#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/properties/property_types.h"
#include "render/theme/theme.h"
#include "text/codec.h"
#include "ui/views/markdown/markdown_commands.h"
#include "ui/views/markdown/properties/properties_commands.h"
#include "ui/views/markdown/properties/properties_style.h"
#include "ui/views/markdown/properties/properties_widget.h"
#include "ui/views/markdown/properties/property_suggest.h"

#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>

using namespace oc;

namespace {

const QByteArray kNote = "---\n"
                         "title: Hello world\n"
                         "count: 3\n"
                         "done: false\n"
                         "due: 2024-03-15\n"
                         "when: 2024-03-15T10:30\n"
                         "tags:\n"
                         "  - project/alpha\n"
                         "  - misc\n"
                         "aliases:\n"
                         "  - Greeting\n"
                         "related: \"[[Other]]\"\n"
                         "list:\n"
                         "  - one\n"
                         "  - two\n"
                         "meta:\n"
                         "  author: me\n"
                         "---\n"
                         "# Heading\n"
                         "\n"
                         "Body text.\n";

class PropertiesTest : public test::AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {
            {"Note.md", kNote},
            {"Other.md", "---\ntitle: Other note\nstatus: draft\n---\nOther body\n"},
            {"Crlf.md", "\xEF\xBB\xBF---\r\ntitle: Windows\r\n# comment\r\nlist: [a, b]\r\n---\r\nText\r\n"},
            {"Bad.md", "---\na: [unclosed\n---\nBody\n"},
            {"Plain.md", "No properties here.\n"},
        };
    }
    void settle() { QTest::qWait(20); }
    QString text(MarkdownView* mv) const { return mv->document()->text().toString(); }
    PropertiesWidget* props(MarkdownView* mv) const { return mv->properties(); }
    void typeInto(QWidget* w, const QString& s)
    {
        w->setFocus(Qt::OtherFocusReason);
        QTest::keyClicks(w, s);
    }
};

} // namespace

TEST_F(PropertiesTest, WidgetsFollowTheTypes)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    PropertiesWidget* p = props(mv);
    ASSERT_TRUE(p->isVisible());
    EXPECT_EQ(p->keys(), QStringList({"title", "count", "done", "due", "when", "tags", "aliases", "related", "list", "meta"}));
    EXPECT_EQ(p->row("title")->widgetType(), "text");
    EXPECT_EQ(p->row("count")->widgetType(), "number");
    EXPECT_EQ(p->row("done")->widgetType(), "checkbox");
    EXPECT_EQ(p->row("due")->widgetType(), "date");
    EXPECT_EQ(p->row("when")->widgetType(), "datetime");
    EXPECT_EQ(p->row("tags")->widgetType(), "tags");
    EXPECT_EQ(p->row("aliases")->widgetType(), "aliases");
    EXPECT_EQ(p->row("related")->widgetType(), "text");
    EXPECT_EQ(p->row("list")->widgetType(), "multitext");
    EXPECT_EQ(p->row("meta")->widgetType(), "unknown");
    auto* meta = qobject_cast<UnknownPropertyEditor*>(p->row("meta")->editor());
    ASSERT_TRUE(meta);
    EXPECT_EQ(meta->text(), "{\"author\":\"me\"}");
    auto* related = qobject_cast<TextPropertyEditor*>(p->row("related")->editor());
    ASSERT_TRUE(related);
    EXPECT_TRUE(related->showsLink());
    auto* list = qobject_cast<MultiSelectPropertyEditor*>(p->row("list")->editor());
    ASSERT_TRUE(list);
    EXPECT_EQ(list->values(), QStringList({"one", "two"}));
    auto* date = qobject_cast<DatePropertyEditor*>(p->row("due")->editor());
    ASSERT_TRUE(date);
    EXPECT_EQ(date->formatted(), "2024-03-15");
}

TEST_F(PropertiesTest, FrontmatterIsHiddenAndTheCursorStaysInTheBody)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv->isFrontmatterHidden());
    EditorView* e = mv->editor();
    e->layout().ensureLaidOut(0, 2000);
    EXPECT_EQ(e->layout().lineHeight(0), 0);
    EXPECT_EQ(e->layout().lineHeight(17), 0);
    EXPECT_GT(e->layout().lineHeight(18), 0);
    e->setCursorPosition(0);
    const Text& t = mv->document()->text();
    EXPECT_EQ(e->cursorPosition(), t.lineStart(18));
    e->selectAll();
    EXPECT_EQ(e->selection().main().from(), t.lineStart(18));
    // Source mode shows the YAML
    mv->setMode(EditorView::Mode::Source);
    EXPECT_FALSE(mv->isFrontmatterHidden());
    EXPECT_FALSE(props(mv)->isVisible());
    mv->setMode(EditorView::Mode::LivePreview);
    EXPECT_TRUE(mv->isFrontmatterHidden());
}

TEST_F(PropertiesTest, EditTextValueIsOneUndoableTransaction)
{
    MarkdownView* mv = open("Note.md");
    const QString before = text(mv);
    PropertiesWidget* p = props(mv);
    auto* ed = qobject_cast<TextPropertyEditor*>(p->row("title")->editor());
    ASSERT_TRUE(ed);
    const int depth = mv->document()->history().undoDepth();
    ed->focusEditor(QStringLiteral("both"));
    QTest::keyClicks(ed->input(), "New: title");
    QTest::keyClick(ed->input(), Qt::Key_Return);
    settle();
    QString expected = before;
    expected.replace("title: Hello world", "title: \"New: title\"");
    EXPECT_EQ(text(mv), expected);
    EXPECT_EQ(mv->document()->history().undoDepth(), depth + 1);
    EXPECT_TRUE(p->row("title")->hasFocus()); // Enter commits and leaves the value
    ASSERT_TRUE(mv->editor()->undo());
    EXPECT_EQ(text(mv), before);
    EXPECT_EQ(qobject_cast<TextPropertyEditor*>(p->row("title")->editor())->text(), "Hello world");
    ASSERT_TRUE(mv->editor()->redo());
    EXPECT_EQ(text(mv), expected);
}

TEST_F(PropertiesTest, EmptyingATextValueStoresNull)
{
    MarkdownView* mv = open("Note.md");
    auto* ed = qobject_cast<TextPropertyEditor*>(props(mv)->row("title")->editor());
    ed->commitText(QString());
    EXPECT_TRUE(text(mv).contains("\ntitle:\ncount: 3\n"));
}

TEST_F(PropertiesTest, ToggleCheckbox)
{
    MarkdownView* mv = open("Note.md");
    auto* cb = qobject_cast<CheckboxPropertyEditor*>(props(mv)->row("done")->editor());
    ASSERT_TRUE(cb);
    EXPECT_FALSE(cb->isChecked());
    QTest::mouseClick(cb, Qt::LeftButton, {}, cb->boxRect().center());
    EXPECT_TRUE(text(mv).contains("\ndone: true\n"));
    QTest::keyClick(cb, Qt::Key_Space);
    EXPECT_TRUE(text(mv).contains("\ndone: false\n"));
    ASSERT_TRUE(mv->editor()->undo());
    EXPECT_TRUE(text(mv).contains("\ndone: true\n"));
}

TEST_F(PropertiesTest, AddAndRemoveListItems)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    auto* list = qobject_cast<MultiSelectPropertyEditor*>(p->row("list")->editor());
    ASSERT_TRUE(list);
    typeInto(list->input(), "three");
    QTest::keyClick(list->input(), Qt::Key_Return);
    EXPECT_TRUE(text(mv).contains("list:\n  - one\n  - two\n  - three\nmeta:"));
    list = qobject_cast<MultiSelectPropertyEditor*>(p->row("list")->editor());
    ASSERT_EQ(list->pillCount(), 3);
    // the remove button of the first pill
    PropertyPill* first = list->pill(0);
    QTest::mouseClick(first, Qt::LeftButton, {}, first->removeRect().center());
    QTRY_VERIFY(text(mv).contains("list:\n  - two\n  - three\nmeta:"));
    // Backspace on a focused pill removes it
    list = qobject_cast<MultiSelectPropertyEditor*>(p->row("list")->editor());
    list->focusPill(1);
    QTest::keyClick(list->pill(1), Qt::Key_Backspace);
    EXPECT_TRUE(text(mv).contains("list:\n  - two\nmeta:"));
    // removing every item stores null
    list = qobject_cast<MultiSelectPropertyEditor*>(p->row("list")->editor());
    list->removeValue(0);
    EXPECT_TRUE(text(mv).contains("\nlist:\nmeta:"));
}

TEST_F(PropertiesTest, TagsAndAliases)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    auto* tags = qobject_cast<MultiSelectPropertyEditor*>(p->row("tags")->editor());
    ASSERT_TRUE(tags);
    EXPECT_FALSE(tags->addValue("not a tag!"));
    EXPECT_FALSE(tags->addValue("#misc")); // a duplicate (without '#')
    EXPECT_TRUE(tags->addValue("#new/tag"));
    EXPECT_TRUE(text(mv).contains("tags:\n  - project/alpha\n  - misc\n  - \"#new/tag\"\naliases:"));
    auto* aliases = qobject_cast<MultiSelectPropertyEditor*>(p->row("aliases")->editor());
    EXPECT_FALSE(aliases->addValue("Greeting"));
    EXPECT_TRUE(aliases->addValue("  Hi there "));
    EXPECT_TRUE(text(mv).contains("aliases:\n  - Greeting\n  - Hi there\nrelated:"));
}

TEST_F(PropertiesTest, NumberDateAndDatetime)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    auto* num = qobject_cast<NumberPropertyEditor*>(p->row("count")->editor());
    ASSERT_TRUE(num);
    num->focusEditor(QStringLiteral("both"));
    QTest::keyClicks(num->input(), "4.50");
    QTest::keyClick(num->input(), Qt::Key_Return);
    EXPECT_TRUE(text(mv).contains("\ncount: 4.5\n"));
    auto* due = qobject_cast<DatePropertyEditor*>(p->row("due")->editor());
    due->input()->setDate(QDate(2025, 1, 2));
    due->commit();
    EXPECT_TRUE(text(mv).contains("\ndue: 2025-01-02\n"));
    auto* when = qobject_cast<DatePropertyEditor*>(p->row("when")->editor());
    when->input()->setDateTime(QDateTime(QDate(2025, 1, 2), QTime(8, 5)));
    when->commit();
    EXPECT_TRUE(text(mv).contains("\nwhen: 2025-01-02T08:05:00\n"));
}

TEST_F(PropertiesTest, ChangeTypeWritesTypesJsonNotTheNote)
{
    MarkdownView* mv = open("Note.md");
    const QString before = text(mv);
    PropertiesWidget* p = props(mv);
    PropertyRow* r = p->row("title");
    QMenu menu;
    r->populateMenu(&menu);
    QMenu* sub = menu.actions().first()->menu();
    ASSERT_TRUE(sub);
    QStringList titles;
    for (QAction* a : sub->actions())
        titles << a->text();
    EXPECT_EQ(titles, QStringList({"Automatic (Text)", "Checkbox", "Date", "Date & time", "List", "Number", "Text"}));
    for (QAction* a : sub->actions())
        if (a->text() == "List")
            a->trigger();
    EXPECT_EQ(p->row("title")->widgetType(), "multitext");
    EXPECT_FALSE(p->row("title")->hasTypeWarning());
    EXPECT_EQ(text(mv), before); // the note is untouched until the value is edited
    session->config()->flush();
    const QJsonObject types = QJsonDocument::fromJson(test::readFile(session->config()->dir() + "/types.json")).object();
    EXPECT_EQ(types.value("types").toObject().value("title").toString(), "multitext");
    EXPECT_FALSE(QFile::exists(root + "/.obsidian/types.json"));
    // a number shown as text: mismatch warning, the number widget stays until "Display as"
    p->row("count")->changeType(QStringLiteral("checkbox"));
    EXPECT_TRUE(p->row("count")->hasTypeWarning());
    EXPECT_EQ(p->row("count")->widgetType(), "number");
    p->row("count")->changeType(QString()); // back to automatic
    EXPECT_FALSE(p->row("count")->hasTypeWarning());
    EXPECT_EQ(PropertyTypeManager::of(session.get())->assignedType("count"), "");
}

TEST_F(PropertiesTest, ModSemicolonAddsAProperty)
{
    CommandRegistry registry;
    registerMarkdownCommands(registry, window->workspace());
    registerPropertyCommands(registry);
    HotkeyDispatcher dispatcher(&registry);
    dispatcher.setWindow(window);
    qApp->installEventFilter(&dispatcher);
    MarkdownView* mv = open("Plain.md");
    PropertiesWidget* p = props(mv);
    EXPECT_FALSE(p->isVisible());
    EXPECT_TRUE(registry.commandsFor(Hotkey::parse("Mod+;")).contains("markdown:add-metadata-property"));
    QTest::keyClick(mv->editor(), Qt::Key_Semicolon, Qt::ControlModifier);
    ASSERT_EQ(p->propertyCount(), 1);
    EXPECT_TRUE(p->isVisible());
    PropertyRow* pending = p->rowAt(0);
    EXPECT_TRUE(pending->keyInput()->hasFocus());
    EXPECT_EQ(text(mv), "No properties here.\n"); // nothing written before the key is named
    QTest::keyClicks(pending->keyInput(), "status");
    QTest::keyClick(pending->keyInput(), Qt::Key_Return);
    EXPECT_EQ(text(mv), "---\nstatus:\n---\nNo properties here.\n");
    QTRY_VERIFY(p->row("status") && p->row("status")->editor()->hasFocusWithin());
    // "Add alias"
    ASSERT_TRUE(registry.execute("markdown:add-alias"));
    EXPECT_EQ(text(mv), "---\nstatus:\naliases:\n---\nNo properties here.\n");
    EXPECT_EQ(p->row("aliases")->widgetType(), "aliases");
    // "Clear file properties" removes the block
    ASSERT_TRUE(registry.execute("markdown:clear-metadata-properties"));
    EXPECT_EQ(text(mv), "No properties here.\n");
    qApp->removeEventFilter(&dispatcher);
}

TEST_F(PropertiesTest, EscapeOnANewPropertyDropsIt)
{
    MarkdownView* mv = open("Plain.md");
    mv->addProperty();
    PropertiesWidget* p = props(mv);
    ASSERT_EQ(p->propertyCount(), 1);
    QTest::keyClick(p->rowAt(0)->keyInput(), Qt::Key_Escape); // closes the key suggestions
    if (p->propertyCount() == 1)
        QTest::keyClick(p->rowAt(0)->keyInput(), Qt::Key_Escape);
    EXPECT_EQ(p->propertyCount(), 0);
    EXPECT_FALSE(p->isVisible());
    EXPECT_EQ(text(mv), "No properties here.\n");
}

TEST_F(PropertiesTest, RenameAndDuplicateKeys)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    QLineEdit* key = p->row("count")->keyInput();
    key->setFocus();
    key->selectAll();
    QTest::keyClicks(key, "total");
    QTest::keyClick(key, Qt::Key_Return);
    EXPECT_TRUE(text(mv).contains("\ntotal: 3\ndone: false\n"));
    ASSERT_TRUE(p->row("total"));
    // a duplicate name is refused and the key reverts
    QLineEdit* key2 = p->row("total")->keyInput();
    key2->setFocus();
    key2->selectAll();
    QTest::keyClicks(key2, "Title");
    EXPECT_FALSE(p->row("total")->commitKey("Title"));
    EXPECT_TRUE(text(mv).contains("\ntotal: 3\n"));
}

TEST_F(PropertiesTest, RemoveFromTheMenuAndWithDelete)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    QMenu menu;
    p->row("meta")->populateMenu(&menu);
    QAction* remove = nullptr;
    for (QAction* a : menu.actions())
        if (a->text() == "Remove")
            remove = a;
    ASSERT_TRUE(remove);
    remove->trigger();
    EXPECT_FALSE(text(mv).contains("meta:"));
    EXPECT_FALSE(text(mv).contains("author"));
    p->focusProperty("when");
    ASSERT_TRUE(p->row("when")->hasFocus());
    QTest::keyClick(p->row("when"), Qt::Key_Delete);
    EXPECT_FALSE(text(mv).contains("when:"));
    EXPECT_TRUE(p->row("due")->hasFocus()); // Obsidian focuses the property before the removed one
}

TEST_F(PropertiesTest, KeyboardNavigation)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    p->focusPropertyAt(0);
    ASSERT_TRUE(p->rowAt(0)->hasFocus());
    QTest::keyClick(p->rowAt(0), Qt::Key_Down);
    EXPECT_TRUE(p->rowAt(1)->hasFocus());
    QTest::keyClick(p->rowAt(1), Qt::Key_J);
    EXPECT_TRUE(p->rowAt(2)->hasFocus());
    QTest::keyClick(p->rowAt(2), Qt::Key_K);
    EXPECT_TRUE(p->rowAt(1)->hasFocus());
    QTest::keyClick(p->rowAt(1), Qt::Key_Up);
    QTest::keyClick(p->rowAt(0), Qt::Key_Up);
    EXPECT_TRUE(p->headingWidget()->hasFocus());
    QTest::keyClick(p->headingWidget(), Qt::Key_Down);
    EXPECT_TRUE(p->rowAt(0)->hasFocus());
    QTest::keyClick(p->rowAt(0), Qt::Key_Right);
    EXPECT_TRUE(p->rowAt(0)->editor()->hasFocusWithin());
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape); // closes the value suggestions first
    if (!p->rowAt(0)->hasFocus())
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    EXPECT_TRUE(p->rowAt(0)->hasFocus());
    QTest::keyClick(p->rowAt(0), Qt::Key_Left);
    EXPECT_TRUE(p->rowAt(0)->keyInput()->hasFocus());
    // Tab: key -> value -> next property ("count": a number, no suggestions)
    p->rowAt(1)->focusKey();
    p->rowAt(1)->keySuggest()->close();
    QTest::keyClick(p->rowAt(1)->keyInput(), Qt::Key_Tab);
    EXPECT_TRUE(p->rowAt(1)->editor()->hasFocusWithin());
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    EXPECT_TRUE(p->rowAt(2)->hasFocus());
    QTest::keyClick(p->rowAt(2), Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_TRUE(p->rowAt(1)->hasFocus());
    // the last property -> "Add property" -> the text
    p->focusPropertyAt(-1);
    QTest::keyClick(p->rowAt(p->propertyCount() - 1), Qt::Key_Down);
    EXPECT_TRUE(p->addButton()->hasFocus());
    QTest::keyClick(p->addButton(), Qt::Key_Down);
    EXPECT_TRUE(mv->editor()->hasFocus());
    EXPECT_EQ(mv->document()->text().lineAt(mv->editor()->cursorPosition()), 18);
    // ↑ on the first line of the text comes back to the properties
    QTest::keyClick(mv->editor(), Qt::Key_Up);
    EXPECT_TRUE(p->addButton()->hasFocus());
    // collapse / expand with ←/→ on the heading
    p->headingWidget()->setFocus();
    QTest::keyClick(p->headingWidget(), Qt::Key_Left);
    EXPECT_TRUE(p->isCollapsed());
    EXPECT_FALSE(p->rowAt(0)->isVisible());
    QTest::keyClick(p->headingWidget(), Qt::Key_Right);
    EXPECT_FALSE(p->isCollapsed());
}

TEST_F(PropertiesTest, SelectionCopyAndPaste)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    p->focusProperty("title");
    QTest::keyClick(p->row("title"), Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(p->selectedRows().size(), 2);
    QTest::keyClick(p->row("count"), Qt::Key_C, Qt::ControlModifier);
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    ASSERT_TRUE(mime && mime->hasFormat("obsidian/properties"));
    EXPECT_EQ(mime->text(), "title: Hello world\ncount: 3\n");
    // paste into another note: new keys are appended, existing ones merged
    MarkdownView* other = open("Other.md");
    PropertiesWidget* po = props(other);
    po->focusPropertyAt(0);
    QTest::keyClick(po->rowAt(0), Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(text(other), "---\ntitle: Hello world\nstatus: draft\ncount: 3\n---\nOther body\n");
}

TEST_F(PropertiesTest, ReadingViewIsReadOnly)
{
    MarkdownView* mv = open("Note.md", EditorView::Mode::Reading);
    PropertiesWidget* p = props(mv);
    ASSERT_TRUE(p->isVisible());
    EXPECT_TRUE(p->isReadOnly());
    EXPECT_TRUE(mv->isFrontmatterHidden());
    EXPECT_FALSE(p->addButton()->isVisible());
    EXPECT_TRUE(p->row("title")->keyInput()->isReadOnly());
    const QString before = text(mv);
    auto* cb = qobject_cast<CheckboxPropertyEditor*>(p->row("done")->editor());
    QTest::mouseClick(cb, Qt::LeftButton, {}, cb->boxRect().center());
    EXPECT_FALSE(cb->isChecked());
    auto* list = qobject_cast<MultiSelectPropertyEditor*>(p->row("list")->editor());
    EXPECT_FALSE(list->addValue("x"));
    EXPECT_EQ(text(mv), before);
    EXPECT_FALSE(mv->canShowProperties());
}

TEST_F(PropertiesTest, InvalidYamlShowsTheErrorAndTheSource)
{
    MarkdownView* mv = open("Bad.md");
    PropertiesWidget* p = props(mv);
    ASSERT_TRUE(p->isVisible());
    EXPECT_TRUE(p->isInvalid());
    EXPECT_TRUE(p->errorBox()->isVisible());
    EXPECT_FALSE(mv->isFrontmatterHidden());
    p->addProperty("x"); // refused
    EXPECT_EQ(text(mv), "---\na: [unclosed\n---\nBody\n");
    // "Show source"
    emit p->showSourceRequested();
    EXPECT_EQ(mv->mode(), EditorView::Mode::Source);
    // fixing the YAML brings the properties back
    mv->setMode(EditorView::Mode::LivePreview);
    Transaction tr;
    const Text& t = mv->document()->text();
    tr.changes = ChangeSet::of(ChangeSpec{t.lineEnd(1), t.lineEnd(1), "]"}, t.length());
    mv->editor()->dispatch(tr);
    EXPECT_FALSE(p->isInvalid());
    EXPECT_EQ(p->keys(), QStringList({"a"}));
    EXPECT_TRUE(mv->isFrontmatterHidden());
}

TEST_F(PropertiesTest, PropertiesInDocumentSetting)
{
    MarkdownView* mv = open("Note.md");
    VaultConfig* cfg = session->vaultConfig();
    cfg->set("propertiesInDocument", "source");
    EXPECT_FALSE(props(mv)->isVisible());
    EXPECT_FALSE(mv->isFrontmatterHidden());
    cfg->set("propertiesInDocument", "hidden");
    EXPECT_FALSE(props(mv)->isVisible());
    EXPECT_TRUE(mv->isFrontmatterHidden());
    mv->setMode(EditorView::Mode::Reading);
    EXPECT_FALSE(props(mv)->isVisible());
    cfg->set("propertiesInDocument", "visible");
    EXPECT_TRUE(props(mv)->isVisible());
}

TEST_F(PropertiesTest, LinksOpenAndFrontmatterMatchesFocusTheProperty)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    auto* related = qobject_cast<TextPropertyEditor*>(p->row("related")->editor());
    ASSERT_TRUE(related->showsLink());
    QTest::mouseClick(related, Qt::LeftButton, {}, QPoint(propui::style().padX + 3, related->height() / 2));
    auto activePath = [this] {
        auto* v = qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view());
        return v && v->file() ? v->file()->path() : QString();
    };
    QTRY_COMPARE(activePath(), QString("Other.md"));
    // a search/backlink match inside the frontmatter focuses the property
    MarkdownView* back = open("Note.md");
    const Text& t = back->document()->text();
    const qsizetype at = t.lineStart(11) + 10;
    back->setEphemeralState(QJsonObject{{"match", QJsonObject{{"matches", QJsonArray{QJsonArray{at, at + 9}}}}}});
    EXPECT_TRUE(props(back)->row("related")->hasFocus());
}

TEST_F(PropertiesTest, KeyAndValueSuggestions)
{
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    mv->addProperty();
    PropertyRow* pending = p->rowAt(p->propertyCount() - 1);
    ASSERT_TRUE(pending->key().isEmpty());
    QTest::keyClicks(pending->keyInput(), "sta");
    PropertySuggest* s = pending->keySuggest();
    ASSERT_TRUE(s->isOpen());
    ASSERT_FALSE(s->rows().isEmpty());
    EXPECT_EQ(s->rows().first().title, "status"); // from Other.md
    QTest::keyClick(pending->keyInput(), Qt::Key_Return);
    EXPECT_TRUE(text(mv).contains("\nstatus:\n---\n"));
    QTRY_VERIFY(p->row("status") && p->row("status")->editor()->hasFocusWithin());
    auto* value = qobject_cast<TextPropertyEditor*>(p->row("status")->editor());
    ASSERT_TRUE(value);
    QTRY_VERIFY(value->suggest()->isOpen());
    EXPECT_EQ(value->suggest()->rows().first().title, "draft");
    ASSERT_TRUE(value->suggest()->choose(0));
    EXPECT_TRUE(text(mv).contains("\nstatus: draft\n---\n"));
    // "[[" suggests notes and completes a link
    auto* title = qobject_cast<TextPropertyEditor*>(p->row("title")->editor());
    title->focusEditor(QStringLiteral("both"));
    QTest::keyClicks(title->input(), "[[Oth");
    QTRY_VERIFY(title->suggest()->isOpen());
    EXPECT_EQ(title->suggest()->rows().first().title, "Other");
    QTest::keyClick(title->input(), Qt::Key_Return);
    EXPECT_TRUE(text(mv).contains("\ntitle: \"[[Other]]\"\n"));
}

TEST_F(PropertiesTest, CrlfAndBomSurviveEdits)
{
    MarkdownView* mv = open("Crlf.md");
    PropertiesWidget* p = props(mv);
    EXPECT_EQ(p->keys(), QStringList({"title", "list"}));
    auto* list = qobject_cast<MultiSelectPropertyEditor*>(p->row("list")->editor());
    ASSERT_TRUE(list->addValue("c"));
    auto* title = qobject_cast<TextPropertyEditor*>(p->row("title")->editor());
    title->commitText("Linux");
    ASSERT_TRUE(mv->save());
    EXPECT_EQ(test::readFile(root + "/Crlf.md"),
              QByteArray("\xEF\xBB\xBF---\r\ntitle: Linux\r\n# comment\r\nlist:\r\n  - a\r\n  - b\r\n  - c\r\n---\r\nText\r\n"));
}

TEST_F(PropertiesTest, NotesWithoutEditsAreNotWritten)
{
    const QByteArray before = test::readFile(root + "/Note.md");
    MarkdownView* mv = open("Note.md");
    PropertiesWidget* p = props(mv);
    // focus and leave every value without changing it
    for (int i = 0; i < p->propertyCount(); ++i) {
        p->rowAt(i)->focusValue();
        p->rowAt(i)->focusProperty();
    }
    mv->editor()->setFocus();
    settle();
    EXPECT_FALSE(mv->document()->isDirty());
    mv->save();
    EXPECT_EQ(test::readFile(root + "/Note.md"), before);
}

// ---- screenshots (OC_SHOT_DIR) ----------------------------------------------------------------------

TEST_F(PropertiesTest, Screenshots)
{
    const QString dir = qEnvironmentVariable("OC_SHOT_DIR");
    if (dir.isEmpty())
        GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
    QDir().mkpath(dir);
    window->resize(1200, 820);
    if (!ThemeManager::instance()) {
        static ThemeManager* created = new ThemeManager(qApp);
        created->setMode(ThemeManager::Mode::Dark);
    }
    ThemeManager* tm = ThemeManager::instance();
    for (const auto& [name, mode] : {std::pair{"dark", ThemeManager::Mode::Dark}, std::pair{"light", ThemeManager::Mode::Light}}) {
        tm->setMode(mode);
        tm->apply();
        for (const auto& [m, label] : {std::pair{EditorView::Mode::LivePreview, "lp"}, std::pair{EditorView::Mode::Reading, "reading"}}) {
            MarkdownView* mv = open("Note.md", m);
            if (m == EditorView::Mode::LivePreview)
                props(mv)->row("title")->focusProperty();
            QTest::qWait(150);
            window->grab().save(dir + QStringLiteral("/properties-%1-%2.png").arg(name, label));
        }
    }
    tm->setMode(ThemeManager::Mode::Dark);
    tm->apply();
}
