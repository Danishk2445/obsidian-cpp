// Screenshots of note embeds in both themes (skipped unless OC_SHOT_DIR is set), on a copy of the
// vault-basic fixture: note-embeds-{live,reading}-{dark,light}.png (Embeds.md) and
// note-embeds-demo-{live,reading}-{dark,light}.png (a whole note, a section, a block, a missing
// heading, a note that doesn't exist, a nested embed).
#include "media_test_support.h"

#include "ui/views/markdown/note_embed_view.h"

#include <QDirIterator>

using namespace oc;
using namespace oc::test;

namespace {

const QByteArray kDemo = "# Embed demo\n"
                         "\n"
                         "A whole note:\n"
                         "\n"
                         "![[Welcome]]\n"
                         "\n"
                         "A heading and its section:\n"
                         "\n"
                         "![[Formatting#Emphasis]]\n"
                         "\n"
                         "A block, a missing heading and a note that doesn't exist:\n"
                         "\n"
                         "![[Formatting#^para-id]]\n"
                         "\n"
                         "![[Formatting#Nope]]\n"
                         "\n"
                         "![[Brand new note]]\n"
                         "\n"
                         "A nested embed:\n"
                         "\n"
                         "![[Embeds]]\n"
                         "\n"
                         "The end.\n";

class NoteEmbedShots : public AppFixture, public ::testing::WithParamInterface<bool> {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        QList<QPair<QString, QByteArray>> f;
        const QString base = QStringLiteral(OC_FIXTURES_DIR "/vault-basic");
        QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            f.append({QDir(base).relativeFilePath(path), readFile(path)});
        }
        f.append({QStringLiteral("Embed demo.md"), kDemo});
        return f;
    }
    QString shotDir() const { return QString::fromLocal8Bit(qgetenv("OC_SHOT_DIR")); }
    void SetUp() override
    {
        if (shotDir().isEmpty())
            GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
        mediaThemes();
        setDarkTheme(GetParam());
        AppFixture::SetUp();
        registerNoteEmbeds(session.get());
        window->resize(1400, 850);
        QApplication::processEvents();
    }
    void TearDown() override
    {
        AppFixture::TearDown();
        setDarkTheme(true);
    }
    QString suffix() const { return GetParam() ? "dark" : "light"; }
    void shoot(const QString& note, EditorView::Mode mode, const QString& name)
    {
        MarkdownView* mv = open(note, mode);
        ASSERT_TRUE(mv);
        EditorView* e = mv->editor();
        e->setCursorPosition(0);
        e->clearFocus(); // no caret, nothing revealed
        for (int i = 0; i < 4; ++i) {
            QTest::qWait(30);
            e->viewport()->repaint();
            QApplication::processEvents();
            HostedEmbeds::of(e)->sync();
        }
        QCursor::setPos(-10000, -10000);
        QApplication::processEvents();
        window->repaint();
        EXPECT_TRUE(window->grab().save(shotDir() + "/" + name + "-" + suffix() + ".png"));
    }
};

} // namespace

TEST_P(NoteEmbedShots, EmbedsFixture)
{
    shoot("Embeds.md", EditorView::Mode::LivePreview, "note-embeds-live");
    shoot("Embeds.md", EditorView::Mode::Reading, "note-embeds-reading");
}

TEST_P(NoteEmbedShots, Demo)
{
    shoot("Embed demo.md", EditorView::Mode::LivePreview, "note-embeds-demo-live");
    shoot("Embed demo.md", EditorView::Mode::Reading, "note-embeds-demo-reading");
}

INSTANTIATE_TEST_SUITE_P(Themes, NoteEmbedShots, ::testing::Values(true, false), [](const auto& info) {
    return info.param ? std::string("Dark") : std::string("Light");
});
