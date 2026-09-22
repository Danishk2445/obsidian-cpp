// Paste and drop in markdown views: attachment folder rules, pasted images, files from inside and
// outside the vault, URL over a selection, HTML -> Markdown (only for structured HTML).
#include "app_test_support.h"
#include "ui/views/markdown/attachments.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

using namespace oc;
using namespace oc::test;

class AttachmentsTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"Note.md", "Hello world\n"}, {"Folder/Deep.md", "deep\n"}, {"pic.png", "png"}};
    }
    void setFolder(const QString& v) { session->vaultConfig()->set("attachmentFolderPath", v); }
    static void pasteMime(EditorView* e, QMimeData* md)
    {
        QGuiApplication::clipboard()->setMimeData(md);
        e->paste();
    }
};

TEST_F(AttachmentsTest, AttachmentFolderRules)
{
    TFile* deep = vault->file("Folder/Deep.md");
    ASSERT_TRUE(deep);
    setFolder("/");
    EXPECT_EQ(attachments::availablePathFor(*session, "Pasted image 1", "png", deep), "Pasted image 1.png");
    EXPECT_EQ(attachments::availablePathFor(*session, "pic", "png", deep), "pic 1.png"); // unique
    setFolder("./");
    EXPECT_EQ(attachments::availablePathFor(*session, "a", "png", deep), "Folder/a.png");
    setFolder("./att");
    EXPECT_EQ(attachments::availablePathFor(*session, "a", "png", deep), "Folder/att/a.png");
    EXPECT_TRUE(vault->folder("Folder/att")); // created
    setFolder("assets");
    EXPECT_EQ(attachments::availablePathFor(*session, "a:b#c", "png", deep), "assets/a b c.png"); // sanitized
    EXPECT_TRUE(attachments::isUrl("https://example.com/x?y=1"));
    EXPECT_TRUE(attachments::isUrl("obsidian://open?vault=x"));
    EXPECT_FALSE(attachments::isUrl("not a url"));
    EXPECT_FALSE(attachments::isUrl("plain"));
}

TEST_F(AttachmentsTest, PasteImage)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setCursorPosition(e->document()->text().length());
    QImage img(4, 3, QImage::Format_ARGB32);
    img.fill(Qt::red);
    auto* md = new QMimeData;
    md->setImageData(img);
    pasteMime(e, md);
    const QString line = lastLine(e);
    ASSERT_TRUE(line.startsWith("![[Pasted image ")) << qPrintable(line);
    ASSERT_TRUE(line.endsWith(".png]]"));
    const QString name = line.mid(3, line.size() - 5);
    TFile* f = vault->file(name);
    ASSERT_TRUE(f);
    EXPECT_TRUE(QImage(root + "/" + name).size() == QSize(4, 3));
}

TEST_F(AttachmentsTest, PasteUrlOverSelection)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setSelection(Selection::single(6, 11)); // "world"
    auto* md = new QMimeData;
    md->setText("https://example.com");
    pasteMime(e, md);
    EXPECT_EQ(e->document()->text().line(0), "Hello [world](https://example.com)");
    // no selection: the URL is pasted as text
    e->setCursorPosition(0);
    md = new QMimeData;
    md->setText("https://a.b");
    pasteMime(e, md);
    EXPECT_TRUE(e->document()->text().line(0).startsWith("https://a.bHello"));
}

TEST_F(AttachmentsTest, PasteHtml)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setCursorPosition(e->document()->text().length());
    auto* md = new QMimeData;
    md->setHtml("<p>Some <b>bold</b> and <a href=\"https://x.y\">link</a></p>");
    md->setText("Some bold and link");
    pasteMime(e, md);
    EXPECT_EQ(lastLine(e), "Some **bold** and [link](https://x.y)");
    // editor-style HTML (styled spans only) pastes as its plain text
    md = new QMimeData;
    md->setHtml("<div style=\"color:#ccc\"><span style=\"color:#f00\">echo</span> <span>$HOME *x*</span></div>");
    md->setText("echo $HOME *x*");
    e->insertText("\n");
    pasteMime(e, md);
    EXPECT_EQ(lastLine(e), "echo $HOME *x*");
}

TEST_F(AttachmentsTest, DropFiles)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setCursorPosition(e->document()->text().length());
    // a vault file is embedded as is
    QMimeData inside;
    inside.setUrls({QUrl::fromLocalFile(root + "/pic.png")});
    ASSERT_TRUE(attachments::handleDrop(*mv, inside));
    EXPECT_EQ(lastLine(e), "![[pic.png]]");
    // an outside file is copied into the vault first
    const QString outside = tmp.path() + "/outside/photo.jpg";
    writeFile(outside, "jpg");
    e->insertText("\n");
    QMimeData out;
    out.setUrls({QUrl::fromLocalFile(outside)});
    ASSERT_TRUE(attachments::handleDrop(*mv, out));
    EXPECT_EQ(lastLine(e), "![[photo.jpg]]");
    EXPECT_EQ(readFile(root + "/photo.jpg"), QByteArray("jpg"));
}
