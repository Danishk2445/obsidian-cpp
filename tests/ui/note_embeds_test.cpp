// Note embeds ("![[Note]]", "![[Note#Heading]]", "![[Note#Heading#Sub]]", "![[Note#^block]]"): the
// shown markdown (whole note without frontmatter + title, sections, blocks, list items, footnotes,
// "Unable to find"), nesting depth and cycles, "not created yet", Live Preview (hosted widget, its
// size, reveal on click / cursor), Reading view, links inside embeds (click, Mod-click, hover
// preview, the "Open link" button), scrolling tall embeds, live refresh from the open document and
// from disk (nested embeds included), PDF export, and screenshots (OC_SHOT_DIR).
#include "media_test_support.h"

#include "editor/embeds/note_embed.h"
#include "editor/export/pdf_exporter.h"
#include "editor/live_preview_decorator.h"
#include "core/metadata/link_resolver.h"
#include "ui/views/markdown/note_embed_view.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/widgets/icon_button.h"

#include <QDirIterator>
#include <QPdfDocument>
#include <QScrollBar>
#include <QSignalSpy>

using namespace oc;
using namespace oc::test;

namespace {

const QByteArray kTarget = "---\n"
                           "tags: [a]\n"
                           "title: Target\n"
                           "---\n"
                           "# Target\n"
                           "\n"
                           "Target intro paragraph.\n"
                           "\n"
                           "## Section A\n"
                           "\n"
                           "Alpha text with [[Other]] link.\n"
                           "\n"
                           "## Section B\n"
                           "\n"
                           "Bravo text.\n"
                           "\n"
                           "### Deep\n"
                           "\n"
                           "Deep text.\n"
                           "\n"
                           "## Section C\n"
                           "\n"
                           "Paragraph with block. ^blk\n"
                           "\n"
                           "- item one ^item\n"
                           "    - child of one\n"
                           "- item two\n"
                           "    - nested two ^sub\n"
                           "        - grandchild\n"
                           "\n"
                           "Footnote ref[^n].\n"
                           "\n"
                           "[^n]: The footnote text\n"
                           "    continues here.\n";

QByteArray tallNote()
{
    QByteArray s = "# Tall\n\n";
    for (int i = 0; i < 260; ++i)
        s += "Tall paragraph " + QByteArray::number(i) + " with a few words.\n\n";
    return s;
}

class NoteEmbedTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        QList<QPair<QString, QByteArray>> f{{"Target.md", kTarget},
                                            {"Other.md", "# Other\n\nOther body.\n"},
                                            {"Section.md", "Before.\n\n![[Target#Section A]]\n\nAfter.\n"},
                                            {"Whole.md", "Intro.\n\n![[Target]]\n\nOutro.\n"},
                                            {"Nested.md", "Nested intro.\n\n![[Target#Section A]]\n"},
                                            {"Host.md", "Host top.\n\n![[Nested]]\n\nHost end.\n"},
                                            {"Missing.md", "Start.\n\n![[Brand new]]\n\nEnd.\n"},
                                            {"Loop A.md", "Loop A text.\n\n![[Loop B]]\n"},
                                            {"Loop B.md", "Loop B text.\n\n![[Loop A]]\n"},
                                            {"Self.md", "# Self\n\nSelf text.\n\n![[Self]]\n"},
                                            {"Chain.md", "![[D1]]\n"},
                                            {"Tall.md", tallNote()},
                                            {"Tall host.md", "Above.\n\n![[Tall]]\n\nBelow.\n"}};
        for (int i = 1; i <= 7; ++i)
            f.append({QStringLiteral("D%1.md").arg(i), QByteArray("D") + QByteArray::number(i) + " text.\n\n![[D" + QByteArray::number(i + 1) + "]]\n"});
        f.append({QStringLiteral("D8.md"), QByteArray("D8 text.\n")});
        return f;
    }
    void SetUp() override
    {
        mediaThemes();
        AppFixture::SetUp();
        registerNoteEmbeds(session.get());
        preview = PagePreview::of(window);
        preview->setDelays(0, 50);
    }
    QString abs(const QString& path) const { return root + "/" + path; }
    static EmbedRequest request(const QString& absPath, const QString& target, const QString& subpath = {})
    {
        EmbedRequest r = EmbedRequest::fromLink(absPath, target, subpath);
        return r;
    }
    std::shared_ptr<NoteEmbedLayout> layoutOf(const QString& note, const QString& subpath = {}, qreal width = 700)
    {
        return NoteEmbeds::instance().layout(request(abs(note + ".md"), note, subpath), width);
    }
    static void pump(EditorView* e)
    {
        for (int i = 0; i < 3; ++i) {
            e->viewport()->repaint();
            QApplication::processEvents();
            if (HostedEmbeds* h = HostedEmbeds::of(e))
                h->sync();
        }
    }
    static QList<NoteEmbedWidget*> live(EditorView* e)
    {
        QList<NoteEmbedWidget*> out;
        for (QWidget* w : HostedEmbeds::of(e)->liveWidgets())
            if (auto* n = qobject_cast<NoteEmbedWidget*>(w))
                out << n;
        return out;
    }
    MarkdownView* openNote(const QString& path, EditorView::Mode mode)
    {
        MarkdownView* mv = open(path, mode);
        if (mv) {
            QApplication::processEvents();
            pump(mv->editor());
        }
        return mv;
    }
    // a point of the widget whose hit is a link to `linktext` (scans the widget)
    static std::optional<QPoint> linkPoint(NoteEmbedWidget* w, const QString& linktext)
    {
        for (int y = 2; y < w->height(); y += 3)
            for (int x = 2; x < w->width(); x += 4) {
                const NoteEmbedLayout::Hit h = w->hitAt(QPoint(x, y));
                if (h.type == NoteEmbedLayout::Hit::Type::Link && h.linktext == linktext)
                    return h.rect.center().toPoint();
            }
        return std::nullopt;
    }
    QString activePath() const
    {
        auto* v = qobject_cast<FileView*>(window->workspace()->activeRootLeaf()->view());
        return v && v->file() ? v->file()->path() : QString();
    }
    static void hoverAt(QWidget* w, const QPoint& p)
    {
        QCursor::setPos(w->mapToGlobal(p));
        QTest::mouseMove(w, p);
    }

    PagePreview* preview = nullptr;
};

} // namespace

TEST_F(NoteEmbedTest, WholeNoteHidesFrontmatterAndShowsTitle)
{
    const auto l = layoutOf("Target");
    EXPECT_EQ(l->kind(), NoteEmbedLayout::Kind::Note);
    EXPECT_EQ(l->title(), "Target");
    EXPECT_EQ(l->vaultPath(), "Target.md");
    EXPECT_TRUE(l->markdown().startsWith("# Target\n\nTarget intro paragraph.")) << l->markdown().toStdString();
    EXPECT_FALSE(l->markdown().contains("tags:"));
    EXPECT_TRUE(l->markdown().endsWith("    continues here."));
    EXPECT_EQ(l->size().width(), 700);
    EXPECT_GT(l->contentTop(), 20); // the title line
    EXPECT_NEAR(l->size().height(), l->contentTop() + l->contentHeight(), 1);
    EXPECT_FALSE(l->scrollable());
    // cached: the same layout again, nothing rebuilt
    const int builds = NoteEmbeds::instance().builds();
    EXPECT_EQ(layoutOf("Target"), l);
    EXPECT_EQ(NoteEmbeds::instance().builds(), builds);
}

TEST_F(NoteEmbedTest, HeadingSections)
{
    const auto b = layoutOf("Target", "#Section B");
    EXPECT_TRUE(b->subpathFound());
    EXPECT_EQ(b->title(), ""); // the heading is part of the content
    EXPECT_EQ(b->markdown(), "## Section B\n\nBravo text.\n\n### Deep\n\nDeep text.");
    EXPECT_EQ(b->contentTop(), 0);
    const auto deep = layoutOf("Target", "#Section B#Deep");
    EXPECT_EQ(deep->markdown(), "### Deep\n\nDeep text.");
    EXPECT_LT(deep->size().height(), b->size().height());
    // case / punctuation insensitive like Obsidian
    EXPECT_EQ(layoutOf("Target", "#section b")->markdown(), b->markdown());
    // the last heading runs to the end
    EXPECT_TRUE(layoutOf("Target", "#Section C")->markdown().endsWith("continues here."));
}

TEST_F(NoteEmbedTest, BlocksListItemsAndFootnotes)
{
    EXPECT_EQ(layoutOf("Target", "#^blk")->markdown(), "Paragraph with block. ^blk");
    EXPECT_EQ(layoutOf("Target", "#^item")->markdown(), "- item one ^item\n    - child of one");
    // a nested item is un-indented with its children
    EXPECT_EQ(layoutOf("Target", "#^sub")->markdown(), "- nested two ^sub\n    - grandchild");
    EXPECT_EQ(layoutOf("Target", "#[^n]")->markdown(), "The footnote text\ncontinues here.");
}

TEST_F(NoteEmbedTest, MissingSubpath)
{
    const auto l = layoutOf("Target", "#Nope");
    EXPECT_FALSE(l->subpathFound());
    EXPECT_EQ(l->markdown(), QString::fromUtf8("##### Unable to find “Nope” in Target"));
    EXPECT_GT(l->size().height(), 10);
    EXPECT_EQ(layoutOf("Target", "#^nope")->markdown(), QString::fromUtf8("##### Unable to find “^nope” in Target"));
}

TEST_F(NoteEmbedTest, DepthLimitAndCycles)
{
    NoteEmbeds& n = NoteEmbeds::instance();
    // D1 embeds D2 ... : rendered down to depth 5, D6 is the generic file box, D7 isn't reached
    const auto d1 = layoutOf("D1");
    EXPECT_EQ(d1->depth(), 1);
    auto kindAtDepth = [&](const QString& note, int depth) -> std::optional<NoteEmbedLayout::Kind> {
        for (const auto& l : n.cachedLayoutsOf(abs(note)))
            if (l->depth() == depth)
                return l->kind();
        return std::nullopt;
    };
    EXPECT_EQ(kindAtDepth("D5.md", 5), NoteEmbedLayout::Kind::Note);
    EXPECT_EQ(kindAtDepth("D6.md", 6), NoteEmbedLayout::Kind::Generic);
    EXPECT_TRUE(n.cachedLayoutsOf(abs("D7.md")).isEmpty());
    EXPECT_TRUE(d1->dependsOn(abs("D5.md")));
    // A -> B -> A: the second A is a cycle
    layoutOf("Loop A");
    EXPECT_EQ(kindAtDepth("Loop B.md", 2), NoteEmbedLayout::Kind::Note);
    EXPECT_EQ(kindAtDepth("Loop A.md", 3), NoteEmbedLayout::Kind::Generic);
    // a note embedding itself shows itself once
    layoutOf("Self");
    EXPECT_EQ(kindAtDepth("Self.md", 1), NoteEmbedLayout::Kind::Note);
    EXPECT_EQ(kindAtDepth("Self.md", 2), NoteEmbedLayout::Kind::Generic);
}

TEST_F(NoteEmbedTest, LinkEmbedWidgetRouting)
{
    auto resolve = [this](const QString& linkpath) -> QString {
        const QString dest = meta->resolver().resolve(LinkResolver::splitSubpath(linkpath).first, "Section.md");
        return dest.isEmpty() ? QString() : vault->absolutePath(dest);
    };
    auto widget = [&](const QString& target, const QString& subpath) {
        return std::dynamic_pointer_cast<EmbedWidget>(makeLinkEmbedWidget(resolve, target, subpath, -1, -1, 700));
    };
    auto w = widget("Target", "#Section A");
    ASSERT_TRUE(w);
    EXPECT_EQ(w->request().extension, "md");
    EXPECT_EQ(w->request().absPath, abs("Target.md"));
    EXPECT_TRUE(widget("Target.md", "")); // with the extension
    auto missing = widget("Brand new", "");
    ASSERT_TRUE(missing); // "not created yet"
    EXPECT_TRUE(missing->request().absPath.isEmpty());
    EXPECT_EQ(missing->request().linktext, "Brand new");
    EXPECT_FALSE(widget("missing.png", ""));  // images: the decorator's
    EXPECT_FALSE(widget("missing.pdf", ""));  // unresolved attachments stay links
    EXPECT_FALSE(widget("data.csv", ""));
    EXPECT_TRUE(widget("", "#Section A"));    // "![[#Heading]]": the note itself
}

TEST_F(NoteEmbedTest, LivePreviewHostsSizedWidget)
{
    MarkdownView* mv = openNote("Section.md", EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    NoteEmbedWidget* w = live(e).first();
    EXPECT_EQ(w->request().subpath, "#Section A");
    EXPECT_EQ(w->request().absPath, abs("Target.md"));
    const auto l = w->embedLayout();
    EXPECT_EQ(l->markdown(), "## Section A\n\nAlpha text with [[Other]] link.");
    EXPECT_NEAR(w->width(), e->contentWidth(), 1);
    EXPECT_EQ(w->height(), int(l->size().height()));
    EXPECT_NEAR(w->x(), e->contentLeft(), 1);
    EXPECT_TRUE(w->openButton()->isVisible());
    EXPECT_FALSE(w->scrollBar()->isVisible());
    // the "Open link" button sits at the top right (4px insets)
    EXPECT_EQ(w->openButton()->geometry(), QRect(w->width() - 28, 4, 24, 24));
}

TEST_F(NoteEmbedTest, RevealInLivePreview)
{
    MarkdownView* mv = openNote("Section.md", EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    const QString text = e->document()->text().toString();
    const qsizetype from = text.indexOf("![[");
    const qsizetype to = text.indexOf("]]") + 2;
    // the cursor on the embed shows its markdown
    e->setCursorPosition(from + 5);
    pump(e);
    EXPECT_TRUE(live(e).isEmpty());
    e->setCursorPosition(0);
    pump(e);
    ASSERT_EQ(live(e).size(), 1);
    // a click on the embed's content (not a link) selects its markdown, head at the start
    NoteEmbedWidget* w = live(e).first();
    const QPoint content(w->width() / 2, 6);
    ASSERT_EQ(w->hitAt(content).type, NoteEmbedLayout::Hit::Type::Content);
    QTest::mouseClick(w, Qt::LeftButton, {}, content);
    EXPECT_EQ(e->selection().main().anchor, to);
    EXPECT_EQ(e->selection().main().head, from);
    pump(e);
    EXPECT_TRUE(live(e).isEmpty());
    EXPECT_EQ(activePath(), "Section.md"); // nothing opened
}

TEST_F(NoteEmbedTest, ReadingViewAndLinks)
{
    MarkdownView* mv = openNote("Section.md", EditorView::Mode::Reading);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    NoteEmbedWidget* w = live(e).first();
    // a click on the content does nothing in Reading view
    const Selection before = e->selection();
    QTest::mouseClick(w, Qt::LeftButton, {}, QPoint(w->width() / 2, 6));
    EXPECT_EQ(e->selection(), before);
    EXPECT_EQ(activePath(), "Section.md");
    // the [[Other]] link inside the embed follows from the embedded note
    const std::optional<QPoint> link = linkPoint(w, "Other");
    ASSERT_TRUE(link);
    EXPECT_EQ(w->hitAt(*link).sourcePath, "Target.md");
    EXPECT_EQ(w->hitAt(*link).linkKind, 0);
    QTest::mouseClick(w, Qt::LeftButton, {}, *link);
    ASSERT_TRUE(QTest::qWaitFor([&] { return activePath() == "Other.md"; }, 2000));
}

TEST_F(NoteEmbedTest, ModClickOpensNewTabAndOpenButton)
{
    MarkdownView* mv = openNote("Section.md", EditorView::Mode::Reading);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    NoteEmbedWidget* w = live(e).first();
    const std::optional<QPoint> link = linkPoint(w, "Other");
    ASSERT_TRUE(link);
    WorkspaceLeaf* original = window->workspace()->activeRootLeaf();
    QTest::mouseClick(w, Qt::LeftButton, Qt::ControlModifier, *link);
    ASSERT_TRUE(QTest::qWaitFor([&] { return activePath() == "Other.md"; }, 2000));
    EXPECT_NE(window->workspace()->activeRootLeaf(), original); // a new tab
    // back to the note: the "Open link" button opens the embedded note at the heading
    window->workspace()->setActiveLeaf(original, true);
    QApplication::processEvents();
    pump(e);
    ASSERT_EQ(live(e).size(), 1);
    w = live(e).first();
    QTest::mouseClick(w->openButton(), Qt::LeftButton);
    ASSERT_TRUE(QTest::qWaitFor([&] { return activePath() == "Target.md"; }, 2000));
    auto* target = qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view());
    ASSERT_TRUE(target);
    const Text& t = target->document()->text();
    EXPECT_EQ(t.line(t.lineAt(target->editor()->cursorPosition())), "## Section A");
}

TEST_F(NoteEmbedTest, HoverPreviewOfLinkInsideEmbed)
{
    MarkdownView* mv = openNote("Section.md", EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    NoteEmbedWidget* w = live(e).first();
    const std::optional<QPoint> link = linkPoint(w, "Other");
    ASSERT_TRUE(link);
    // embeds are their own "preview" source: no Mod needed, even in Live Preview
    hoverAt(w, *link);
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->popovers().size() == 1; }, 2000));
    EXPECT_EQ(preview->popovers().first()->file()->path(), "Other.md");
    EXPECT_EQ(w->cursor().shape(), Qt::PointingHandCursor);
    hoverAt(e->viewport(), QPoint(5, e->viewport()->height() - 5));
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->popovers().isEmpty(); }, 2000));
}

TEST_F(NoteEmbedTest, NotCreatedYetBoxCreatesTheNote)
{
    MarkdownView* mv = openNote("Missing.md", EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    NoteEmbedWidget* w = live(e).first();
    EXPECT_EQ(w->embedLayout()->kind(), NoteEmbedLayout::Kind::Missing);
    EXPECT_FALSE(w->openButton()->isVisible());
    EXPECT_EQ(w->hitAt(w->rect().center()).type, NoteEmbedLayout::Hit::Type::Box);
    EXPECT_FALSE(vault->file("Brand new.md"));
    QTest::mouseClick(w, Qt::LeftButton, {}, w->rect().center());
    ASSERT_TRUE(QTest::qWaitFor([&] { return activePath() == "Brand new.md"; }, 2000));
    EXPECT_TRUE(vault->file("Brand new.md"));
}

TEST_F(NoteEmbedTest, TallEmbedsScrollInside)
{
    MarkdownView* mv = openNote("Tall host.md", EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    NoteEmbedWidget* w = live(e).first();
    const auto l = w->embedLayout();
    EXPECT_TRUE(l->scrollable());
    EXPECT_EQ(l->viewportHeight(), 4000); // --embed-max-height
    EXPECT_EQ(w->height(), int(l->contentTop() + 4000));
    EXPECT_TRUE(w->scrollBar()->isVisibleTo(w));
    // the wheel scrolls the embed; at its top a wheel up goes to the note
    QWheelEvent up(QPointF(50, 200), w->mapToGlobal(QPointF(50, 200)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                   Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &up);
    EXPECT_FALSE(up.isAccepted());
    QWheelEvent down(QPointF(50, 200), w->mapToGlobal(QPointF(50, 200)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                     Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &down);
    EXPECT_TRUE(down.isAccepted());
    EXPECT_GT(w->scroll(), 0);
    w->setScroll(1e9);
    EXPECT_NEAR(w->scroll(), l->maxScroll(), 1);
    EXPECT_GT(l->contentHeight(), 4000);
    // print shows everything
    ThemeManager* tm = ThemeManager::instance();
    const ThemePtr light = tm->themeFor(Theme::Scheme::Light);
    const EmbedPrintScope scope(light, StyleSheet::build(*light));
    const auto printed = NoteEmbeds::instance().layout(w->request(), w->width());
    EXPECT_FALSE(printed->scrollable());
    EXPECT_GT(printed->size().height(), 4100);
    EXPECT_TRUE(printed->iconRect().isEmpty());
}

TEST_F(NoteEmbedTest, LiveRefreshFromOpenDocument)
{
    MarkdownView* mv = openNote("Section.md", EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    const int before = live(e).first()->height();
    // edit the embedded note without saving: the embed follows (after a short pause)
    Document* doc = docs->acquire("Target.md");
    ASSERT_TRUE(doc);
    const qsizetype at = doc->text().toString().indexOf("Alpha text");
    Transaction tr;
    tr.changes = ChangeSet::of(ChangeSpec{at, at, QStringLiteral("Charlie line.\n\n")}, doc->text().length());
    tr.userEvent = QStringLiteral("input.type");
    doc->dispatch(tr, nullptr, Selection::cursor(at));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return !live(e).isEmpty() && live(e).first()->embedLayout()->markdown().contains("Charlie line.");
    }, 3000));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).first()->height() > before;
    }, 3000));
    EXPECT_TRUE(doc->isDirty()); // (rendering never saves)
    docs->release(doc);
}

TEST_F(NoteEmbedTest, LiveRefreshFromDiskAndNestedDependents)
{
    MarkdownView* mv = openNote("Host.md", EditorView::Mode::Reading);
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        pump(e);
        return live(e).size() == 1;
    }, 3000));
    NoteEmbedWidget* w = live(e).first();
    EXPECT_EQ(w->request().absPath, abs("Nested.md"));
    EXPECT_TRUE(w->embedLayout()->dependsOn(abs("Target.md")));
    QSignalSpy spy(&EmbedRegistry::instance(), &EmbedRegistry::changed);
    // Target.md changes on disk: the embed of Nested.md (which embeds Target's section) is refreshed
    QByteArray changed = kTarget;
    changed.replace("Alpha text", "Zulu words. Alpha text");
    ASSERT_TRUE(writeFile(abs("Target.md"), changed));
    vault->rescan(); // (the watcher would notice too)
    ASSERT_TRUE(QTest::qWaitFor([&] {
        for (const QList<QVariant>& args : spy)
            if (args.value(0).toString() == abs("Nested.md"))
                return true;
        return false;
    }, 5000));
    bool found = false;
    for (const auto& l : NoteEmbeds::instance().cachedLayoutsOf(abs("Target.md")))
        found |= l->markdown().contains("Zulu words");
    pump(e);
    for (const auto& l : NoteEmbeds::instance().cachedLayoutsOf(abs("Target.md")))
        found |= l->markdown().contains("Zulu words");
    EXPECT_TRUE(found);
}

TEST_F(NoteEmbedTest, PdfExportContainsEmbeddedText)
{
    PdfExportContext ctx;
    ctx.resolveAttachment = [this](const QString& linkpath) -> QString {
        const QString dest = meta->resolver().resolve(LinkResolver::splitSubpath(linkpath).first, "Section.md");
        return dest.isEmpty() ? QString() : vault->absolutePath(dest);
    };
    const QString out = tmp.path() + "/section.pdf";
    QString err;
    ASSERT_TRUE(exportPdf(Text::fromString(QString::fromUtf8(readFile(abs("Section.md")))), "Section", out, PdfExportSettings{}, ctx, &err))
        << err.toStdString();
    QPdfDocument pdf;
    ASSERT_EQ(pdf.load(out), QPdfDocument::Error::None);
    QString all;
    for (int i = 0; i < pdf.pageCount(); ++i)
        all += pdf.getAllText(i).text();
    EXPECT_TRUE(all.contains("Before.")) << all.toStdString();
    EXPECT_TRUE(all.contains("Section A")) << all.toStdString();
    EXPECT_TRUE(all.contains("Alpha text with")) << all.toStdString();
    EXPECT_TRUE(all.contains("After.")) << all.toStdString();
    // the export didn't leave print layouts in effect
    EXPECT_FALSE(EmbedPrintScope::active());
}
