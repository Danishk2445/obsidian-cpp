// app.json editor options applied live: "showIndentGuide" (indentation guides) and "strictLineBreaks" through
// MarkdownView (every mode, toggled while open), hover previews, note embeds (cached layouts rebuilt), canvas
// cards' rendering options and the PDF export.
#include "media_test_support.h"

#include "editor/embeds/note_embed.h"
#include "editor/live_preview_decorator.h"
#include "ui/overlay/modal.h"
#include "ui/plugins/pdf_export_ui.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/markdown/note_embed_view.h"
#include "ui/views/markdown/page_preview.h"

#include <QPdfDocument>
#include <QPushButton>

using namespace oc;
using namespace oc::test;

namespace {
const QByteArray kNote = "- a\n"          // 0
                         "\t- b\n"        // 1
                         "\t\t- c\n"      // 2
                         "\n"             // 3
                         "Soft one\n"     // 4
                         "soft two\n";    // 5
}

class EditorOptionsTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"Note.md", kNote}, {"Host.md", "Host.\n\n![[Note]]\n\nEnd.\n"}};
    }
    void SetUp() override
    {
        mediaThemes();
        AppFixture::SetUp();
        registerNoteEmbeds(session.get());
        desktopHandlers().openUrl = [this](const QUrl& u) { opened << u; };
    }
    void TearDown() override
    {
        pdfSavePathProvider() = nullptr;
        AppFixture::TearDown();
    }
    const LineDecoration& deco(EditorView* e, int line) { return e->layout().layoutFor(line)->deco; }
    void setOption(const char* key, bool on)
    {
        session->vaultConfig()->set(QString::fromLatin1(key), on);
        QApplication::processEvents();
    }
    QList<QUrl> opened;
};

TEST_F(EditorOptionsTest, IndentGuidesAppliedLive)
{
    EXPECT_TRUE(session->vaultConfig()->flag("showIndentGuide")); // Obsidian's default
    MarkdownView* mv = open("Note.md", EditorView::Mode::LivePreview);
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EXPECT_TRUE(e->showIndentGuides());
    EXPECT_EQ(deco(e, 2).guideColumns, (QList<int>{0, 1}));
    e->setCursorPosition(e->document()->text().lineStart(1) + 2);
    EXPECT_EQ(deco(e, 2).activeGuideColumn, 1);

    setOption("showIndentGuide", false);
    EXPECT_FALSE(e->showIndentGuides());
    EXPECT_TRUE(deco(e, 2).guideColumns.isEmpty());
    EXPECT_EQ(deco(e, 2).activeGuideColumn, -1);
    mv->setMode(EditorView::Mode::Reading);
    EXPECT_TRUE(deco(e, 2).guidePositions.isEmpty());

    setOption("showIndentGuide", true);
    EXPECT_TRUE(e->showIndentGuides());
    EXPECT_EQ(deco(e, 2).guidePositions.size(), 2); // Reading view: one per nested list around the line
    mv->setMode(EditorView::Mode::Source);
    EXPECT_EQ(deco(e, 2).guideColumns, (QList<int>{0, 1}));
    // a view opened while the option is off starts without guides
    setOption("showIndentGuide", false);
    MarkdownView* other = open("Host.md", EditorView::Mode::LivePreview);
    ASSERT_TRUE(other);
    EXPECT_FALSE(other->editor()->showIndentGuides());
    setOption("showIndentGuide", true);
}

TEST_F(EditorOptionsTest, StrictLineBreaksAppliedLive)
{
    EXPECT_FALSE(session->vaultConfig()->flag("strictLineBreaks"));
    MarkdownView* mv = open("Note.md", EditorView::Mode::Reading);
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EXPECT_EQ(e->layout().unitStart(5), 5);
    setOption("strictLineBreaks", true);
    EXPECT_TRUE(e->strictLineBreaks());
    EXPECT_EQ(e->layout().unitStart(5), 4); // "Soft one soft two"
    // Live Preview's lines keep their line breaks
    mv->setMode(EditorView::Mode::LivePreview);
    EXPECT_EQ(e->layout().unitStart(5), 5);
    mv->setMode(EditorView::Mode::Reading);
    EXPECT_EQ(e->layout().unitStart(5), 4);
    setOption("strictLineBreaks", false);
    EXPECT_EQ(e->layout().unitStart(5), 5);
}

TEST_F(EditorOptionsTest, HoverPreviewsRenderWithTheOptions)
{
    setOption("strictLineBreaks", true);
    setOption("showIndentGuide", false);
    HoverPopover p(window->overlay(), session.get(), vault->file("Note.md"), QString(), nullptr);
    ASSERT_TRUE(p.editor());
    EXPECT_TRUE(p.editor()->strictLineBreaks());
    EXPECT_FALSE(p.editor()->showIndentGuides());
    EXPECT_EQ(p.editor()->layout().unitStart(5), 4);
    setOption("strictLineBreaks", false);
    setOption("showIndentGuide", true);
    HoverPopover q(window->overlay(), session.get(), vault->file("Note.md"), QString(), nullptr);
    EXPECT_FALSE(q.editor()->strictLineBreaks());
    EXPECT_TRUE(q.editor()->showIndentGuides());
}

TEST_F(EditorOptionsTest, NoteEmbedsFollowTheOptions)
{
    NoteEmbeds& ne = NoteEmbeds::instance();
    const EmbedRequest req = EmbedRequest::fromLink(root + "/Note.md", "Note", QString());
    const std::shared_ptr<NoteEmbedLayout> broken = ne.layout(req, 600);
    const qreal brokenH = broken->size().height();
    EXPECT_EQ(ne.layout(req, 600), broken); // cached
    setOption("strictLineBreaks", true);
    const std::shared_ptr<NoteEmbedLayout> strict = ne.layout(req, 600);
    EXPECT_NE(strict, broken); // rebuilt with the new option
    EXPECT_LT(strict->size().height(), brokenH);
    setOption("showIndentGuide", false);
    EXPECT_NE(ne.layout(req, 600), strict);
    setOption("strictLineBreaks", false);
    setOption("showIndentGuide", true);
    EXPECT_EQ(ne.layout(req, 600)->size().height(), brokenH);
    // the host note's editor shows the embed at its new size
    MarkdownView* mv = open("Host.md", EditorView::Mode::Reading);
    ASSERT_TRUE(mv);
    const double before = mv->editor()->documentHeight();
    setOption("strictLineBreaks", true);
    mv->editor()->layout().ensureLaidOut(0, 1e9);
    EXPECT_LT(mv->editor()->documentHeight(), before);
    setOption("strictLineBreaks", false);
}

TEST_F(EditorOptionsTest, PdfExportUsesTheOptions)
{
    setOption("strictLineBreaks", true);
    MarkdownView* mv = open("Note.md", EditorView::Mode::Reading);
    ASSERT_TRUE(mv);
    const QString out = tmp.path() + "/out/Note.pdf";
    QDir().mkpath(tmp.path() + "/out");
    pdfSavePathProvider() = [&](MainWindow*, const QString&) { return out; };
    Modal* m = openExportPdfModal(window, mv);
    ASSERT_TRUE(m);
    m->button(0)->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !opened.isEmpty(); }, 10000));
    QPdfDocument pdf;
    pdf.load(out);
    ASSERT_GT(pdf.pageCount(), 0);
    EXPECT_TRUE(pdf.getAllText(0).text().contains("Soft one soft two")) << pdf.getAllText(0).text().toStdString();
    setOption("strictLineBreaks", false);
}
