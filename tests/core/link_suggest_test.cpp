// Editor suggest logic ("[[", "#", "[^"): triggers, link/heading/block/display suggestions over a real
// vault, Obsidian's link replacement and block-id insertion, vault tag counting.
#include "core/metadata/link_suggest.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;
using namespace Qt::StringLiterals;
using T = LinkSuggestion::Type;

namespace {
bool writeFile(const QString& path, const QByteArray& bytes, qint64 mtimeSecsAgo = 0)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size()))
        return false;
    f.close();
    if (mtimeSecsAgo) {
        QFile g(path);
        if (!g.open(QIODevice::ReadWrite))
            return false;
        g.setFileTime(QDateTime::currentDateTime().addSecs(-mtimeSecsAgo), QFileDevice::FileModificationTime);
    }
    return true;
}
} // namespace

TEST(LinkSuggestTrigger, Link)
{
    auto t = linkSuggestTrigger(u"see [[Oth", 9);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->start, 6);
    EXPECT_EQ(t->end, 9);
    EXPECT_EQ(t->query, "Oth");
    EXPECT_TRUE(linkSuggestTrigger(u"[[", 2));
    EXPECT_EQ(linkSuggestTrigger(u"[[", 2)->query, "");
    EXPECT_FALSE(linkSuggestTrigger(u"[[a]] b", 7));      // closed
    EXPECT_FALSE(linkSuggestTrigger(u"[a] b", 5));
    EXPECT_TRUE(linkSuggestTrigger(u"[[a]] [[b", 9));     // the last "[[" counts
    EXPECT_EQ(linkSuggestTrigger(u"[[Note#Head|al", 14)->query, "Note#Head|al");
    EXPECT_FALSE(linkSuggestTrigger(u"![[pic.png|10", 13)); // image size
    EXPECT_TRUE(linkSuggestTrigger(u"![[doc.pdf|10", 13));
    // only the text before the cursor matters
    EXPECT_EQ(linkSuggestTrigger(u"[[abc]]", 4)->query, "ab");
}

TEST(LinkSuggestTrigger, Tag)
{
    auto t = tagSuggestTrigger(u"text #pro", 9);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->start, 5);
    EXPECT_EQ(t->query, "pro");
    EXPECT_TRUE(tagSuggestTrigger(u"#", 1));
    EXPECT_EQ(tagSuggestTrigger(u"#a/b", 4)->query, "a/b");
    EXPECT_FALSE(tagSuggestTrigger(u"a#b", 3));    // needs whitespace or line start before '#'
    EXPECT_FALSE(tagSuggestTrigger(u"#a b", 4));   // whitespace ends the tag
    EXPECT_FALSE(tagSuggestTrigger(u"#a.b", 4));   // punctuation ends it
    EXPECT_FALSE(tagSuggestTrigger(u"##", 1)); // not right before another '#'
}

TEST(LinkSuggestTrigger, Footnote)
{
    auto t = footnoteSuggestTrigger(u"text[^ab", 8);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->start, 4);
    EXPECT_EQ(t->end, 8);
    EXPECT_EQ(t->query, "ab");
    auto closed = footnoteSuggestTrigger(u"x[^ab] more", 5);
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->end, 6); // includes the "]"
    EXPECT_FALSE(footnoteSuggestTrigger(u"[^ab]: def", 4)); // a definition
    EXPECT_FALSE(footnoteSuggestTrigger(u"[[^x", 4));
}

namespace {
QString linktext(const QString& target, const QString&, bool omitMd)
{
    QString name = target.mid(target.lastIndexOf(u'/') + 1);
    if (omitMd && name.endsWith(u".md"))
        name.chop(3);
    return name;
}
LinkSuggestion fileSuggestion(const QString& path)
{
    LinkSuggestion s;
    s.type = T::File;
    s.file = path;
    s.path = path.endsWith(u".md") ? path.left(path.size() - 3) : path;
    return s;
}
} // namespace

TEST(LinkSuggestReplace, File)
{
    const QString line = u"see [[Oth"_s;
    // Enter: [[Other]] and the cursor after it
    LinkReplacement r = linkSuggestReplacement(fileSuggestion("Other.md"), "Oth", line, 4, 9, "Welcome.md", "Enter", false, linktext);
    EXPECT_EQ(r.replacement, "[[Other]]");
    EXPECT_EQ(r.start, 4);
    EXPECT_EQ(r.end, 9);
    EXPECT_EQ(r.selectionStart, 13);
    EXPECT_EQ(r.selectionEnd, 13);
    // Tab: the cursor before "]]"
    r = linkSuggestReplacement(fileSuggestion("Other.md"), "Oth", line, 4, 9, "Welcome.md", "Tab", false, linktext);
    EXPECT_EQ(r.selectionStart, 11);
    EXPECT_EQ(r.selectionEnd, 11);
    // "#": link a heading next
    r = linkSuggestReplacement(fileSuggestion("Other.md"), "Oth", line, 4, 9, "Welcome.md", "#", false, linktext);
    EXPECT_EQ(r.replacement, "[[Other#]]");
    EXPECT_EQ(r.selectionStart, 12);
    // "#" on a PDF adds "page="
    r = linkSuggestReplacement(fileSuggestion("doc.pdf"), "do", u"[[do", 0, 4, "Welcome.md", "#", false, linktext);
    EXPECT_EQ(r.replacement, "[[doc.pdf#page=]]");
    // an existing "]]" (auto-paired) is replaced over
    r = linkSuggestReplacement(fileSuggestion("Other.md"), "Oth", u"see [[Oth]] tail", 4, 9, "Welcome.md", "Enter", false, linktext);
    EXPECT_EQ(r.end, 11);
    EXPECT_EQ(r.replacement, "[[Other]]");
    // an existing alias is kept
    r = linkSuggestReplacement(fileSuggestion("Other.md"), "Oth", u"[[Oth|shown]]", 0, 5, "Welcome.md", "", false, linktext);
    EXPECT_EQ(r.replacement, "[[Other|shown]]");
    EXPECT_EQ(r.end, 13);
    // markdown links
    r = linkSuggestReplacement(fileSuggestion("My Note.md"), "my", u"[[my", 0, 4, "Welcome.md", "Enter", true, linktext);
    EXPECT_EQ(r.replacement, "[My Note](My%20Note.md)");
    EXPECT_EQ(r.selectionStart, int(r.replacement.size()));
}

TEST(LinkSuggestReplace, FolderPathGetsAlias)
{
    auto lt = [](const QString& target, const QString&, bool) { return QString(target).chopped(3); }; // full path
    LinkReplacement r = linkSuggestReplacement(fileSuggestion("A/Deep.md"), "de", u"[[de", 0, 4, "x.md", "Enter", false, lt);
    EXPECT_EQ(r.replacement, "[[A/Deep|Deep]]");
    // not for images
    auto lt2 = [](const QString& target, const QString&, bool) { return target; };
    r = linkSuggestReplacement(fileSuggestion("A/pic.png"), "pi", u"![[pi", 1, 5, "x.md", "Enter", false, lt2);
    EXPECT_EQ(r.replacement, "[[A/pic.png]]");
}

TEST(LinkSuggestReplace, NoneHeadingAliasBlock)
{
    LinkSuggestion none;
    LinkReplacement r = linkSuggestReplacement(none, "New note", u"[[New note", 0, 10, "x.md", "Enter", false, linktext);
    EXPECT_EQ(r.replacement, "[[New note]]");
    r = linkSuggestReplacement(none, "New note", u"[[New note", 0, 10, "x.md", "", false, linktext);
    EXPECT_EQ(r.replacement, "[[]]"); // a click on "No match found" inserts nothing

    LinkSuggestion h;
    h.type = T::Heading;
    h.file = "Other.md";
    h.path = "Other";
    h.subpath = "#Head";
    h.heading = "Head";
    r = linkSuggestReplacement(h, "Other#He", u"[[Other#He", 0, 10, "x.md", "Enter", false, linktext);
    EXPECT_EQ(r.replacement, "[[Other#Head]]");
    h.path.clear(); // "[[#He" in the current file
    h.file = "x.md";
    r = linkSuggestReplacement(h, "#He", u"[[#He", 0, 5, "x.md", "Enter", false, linktext);
    EXPECT_EQ(r.replacement, "[[#Head]]");

    LinkSuggestion a;
    a.type = T::Alias;
    a.file = "Other.md";
    a.path = "Other";
    a.alias = "Nick";
    r = linkSuggestReplacement(a, "Nic", u"[[Nic", 0, 5, "x.md", "Tab", false, linktext);
    EXPECT_EQ(r.replacement, "[[Other|Nick]]");
    EXPECT_EQ(r.selectionStart, 8); // Tab selects the alias
    EXPECT_EQ(r.selectionEnd, 12);

    LinkSuggestion b;
    b.type = T::Block;
    b.file = "Other.md";
    b.path = "Other";
    b.blockId = "abc";
    r = linkSuggestReplacement(b, "Other#^te", u"[[Other#^te", 0, 11, "x.md", "Enter", false, linktext);
    EXPECT_EQ(r.replacement, "[[Other#^abc]]");
    EXPECT_TRUE(r.newBlockId.isEmpty());
    b.blockId.clear();
    r = linkSuggestReplacement(b, "Other#^te", u"[[Other#^te", 0, 11, "x.md", "Enter", false, linktext);
    ASSERT_EQ(r.newBlockId.size(), 6);
    EXPECT_EQ(r.replacement, "[[Other#^" + r.newBlockId + "]]");
}

TEST(LinkSuggestReplace, BlockIdInsertion)
{
    LinkSuggestion b;
    b.type = T::Block;
    b.content = "para one\nstill\n\n# H\n\n- item\n  - child\n";
    b.blockType = "paragraph";
    b.blockFrom = 0;
    b.blockTo = 14;
    BlockIdInsertion ins = blockIdInsertion(b, "abc123");
    EXPECT_EQ(ins.at, 14);
    EXPECT_EQ(ins.text, " ^abc123");
    EXPECT_EQ(ins.newlines, 0);
    b.blockType = "heading";
    b.blockFrom = 16;
    b.blockTo = 19;
    ins = blockIdInsertion(b, "abc123");
    EXPECT_EQ(ins.text, "\n\n^abc123"); // followed by a blank line already
    EXPECT_EQ(ins.newlines, 2);
    b.blockType = "listItem";
    b.blockFrom = 21;
    b.blockTo = 37;
    b.blockParagraphEnd = 27;
    ins = blockIdInsertion(b, "x");
    EXPECT_EQ(ins.at, 27); // after the item's own text, not after its children
    EXPECT_EQ(ins.text, " ^x");
    b.blockType = "code";
    b.content = "```\nx\n```";
    b.blockFrom = 0;
    b.blockTo = 9;
    ins = blockIdInsertion(b, "x");
    EXPECT_EQ(ins.text, "\n\n^x\n");
    EXPECT_EQ(ins.newlines, 3);
}

TEST(LinkSuggest, HeadingSubpathText)
{
    EXPECT_EQ(headingSubpathText("A: b #c | d ^e \\ f"), "A b c d e f");
    EXPECT_EQ(headingSubpathText("  x  [[y]]  %%z%% "), "x y z");
}

class LinkSuggestVaultTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tmp.isValid());
        root = tmp.path() + "/vault";
        writeFile(root + "/Welcome.md", "See [[Other]] and [[Missing note]].\n#tag/sub and #Tag\n", 300);
        writeFile(root + "/Other.md", "---\naliases: [Nick]\n---\n# Head\ntext\n## Sub\n# Second\npara with words ^blk\n\n- item one\n", 100);
        writeFile(root + "/Folder/Deep note.md", "#tag\n", 200);
        writeFile(root + "/img.png", "png", 50);
        vault = std::make_unique<Vault>(root);
        meta = std::make_unique<MetadataCache>();
        QObject::connect(vault.get(), &Vault::loaded, meta.get(), [this] {
            QStringList paths;
            for (TFile* f : vault->files())
                paths << f->path();
            meta->indexAll(paths);
        });
        meta->setContentProvider([this](const QString& p) -> std::optional<QByteArray> {
            QFile f(root + "/" + p);
            if (!f.open(QIODevice::ReadOnly))
                return std::nullopt;
            return f.readAll();
        });
        vault->load();
        ASSERT_TRUE(QTest::qWaitFor([&] { return meta->isReady(); }, 10000));
        provider = std::make_unique<LinkSuggestProvider>(vault.get(), meta.get());
        provider->setContentReader([this](const QString& p) -> std::optional<QString> {
            QFile f(root + "/" + p);
            if (!f.open(QIODevice::ReadOnly))
                return std::nullopt;
            return QString::fromUtf8(f.readAll());
        });
    }
    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<Vault> vault;
    std::unique_ptr<MetadataCache> meta;
    std::unique_ptr<LinkSuggestProvider> provider;
};

TEST_F(LinkSuggestVaultTest, Files)
{
    // empty query: most recently modified first
    QList<LinkSuggestion> s = provider->suggestions("", "Welcome.md");
    EXPECT_EQ(provider->mode(), LinkSuggestProvider::Mode::File);
    ASSERT_GE(s.size(), 4);
    EXPECT_EQ(s[0].path, "img.png");
    EXPECT_EQ(s[0].type, T::File);
    EXPECT_EQ(provider->instructions().size(), 3);
    // fuzzy: basename hits
    s = provider->suggestions("deep", "Welcome.md");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].file, "Folder/Deep note.md");
    EXPECT_EQ(s[0].path, "Folder/Deep note");
    ASSERT_FALSE(s[0].matches.isEmpty());
    EXPECT_EQ(s[0].matches.first().from, 7); // shifted into the full path
    // nothing matches: a None placeholder
    s = provider->suggestions("zzzzqqq", "Welcome.md");
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(s[0].type, T::None);
}

TEST_F(LinkSuggestVaultTest, UnresolvedAndAliases)
{
    const md::CachedMetadata* w = meta->fileCache("Welcome.md");
    const md::CachedMetadata* o = meta->fileCache("Other.md");
    ASSERT_TRUE(w && o);
    ASSERT_EQ(w->links.size(), 2);
    QList<LinkSuggestion> s = provider->suggestions("missing", "Welcome.md");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].type, T::Linktext);
    EXPECT_EQ(s[0].path, "Missing note");
    ASSERT_EQ(o->aliases(), QStringList{"Nick"});
    s = provider->suggestions("nick", "Welcome.md");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].type, T::Alias);
    EXPECT_EQ(s[0].alias, "Nick");
    EXPECT_EQ(s[0].file, "Other.md");
    // display mode: aliases of the target
    provider->reset();
    s = provider->suggestions("Other|ni", "Welcome.md");
    EXPECT_EQ(provider->mode(), LinkSuggestProvider::Mode::Display);
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].alias, "Nick");
}

TEST_F(LinkSuggestVaultTest, Headings)
{
    const md::CachedMetadata* o = meta->fileCache("Other.md");
    ASSERT_TRUE(o);
    ASSERT_EQ(o->headings.size(), 3);
    QList<LinkSuggestion> s = provider->suggestions("Other#", "Welcome.md");
    EXPECT_EQ(provider->mode(), LinkSuggestProvider::Mode::Heading);
    ASSERT_EQ(s.size(), 3);
    EXPECT_EQ(s[0].heading, "Head");
    EXPECT_EQ(s[0].subpath, "#Head");
    EXPECT_EQ(s[1].level, 2);
    // nested: "#Head#" lists the headings inside "Head"
    s = provider->suggestions("Other#Head#", "Welcome.md");
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(s[0].heading, "Sub");
    EXPECT_EQ(s[0].subpath, "#Head#Sub");
    // fuzzy
    s = provider->suggestions("Other#sec", "Welcome.md");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].heading, "Second");
    // unknown heading: a placeholder to link it anyway
    s = provider->suggestions("Other#Nope zz", "Welcome.md");
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(s[0].type, T::Heading);
    EXPECT_TRUE(s[0].file.isEmpty());
    EXPECT_EQ(s[0].heading, "Nope zz");
    // global headings
    s = provider->suggestions("##seco", "Welcome.md");
    EXPECT_TRUE(provider->isGlobal());
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].file, "Other.md");
    EXPECT_TRUE(s[0].path.isEmpty());
}

TEST_F(LinkSuggestVaultTest, Blocks)
{
    const md::CachedMetadata* o = meta->fileCache("Other.md");
    ASSERT_TRUE(o);
    ASSERT_FALSE(o->sections.isEmpty());
    ASSERT_TRUE(o->blocks.contains("blk"));
    QList<LinkSuggestion> s = provider->suggestions("Other#^words", "Welcome.md");
    EXPECT_EQ(provider->mode(), LinkSuggestProvider::Mode::Block);
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].type, T::Block);
    EXPECT_EQ(s[0].display, "para with words");
    EXPECT_EQ(s[0].blockId, "blk");
    // "Note^" is the same as "Note#^"
    s = provider->suggestions("Other^item", "Welcome.md");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_EQ(s[0].display, "item one");
}

TEST_F(LinkSuggestVaultTest, Tags)
{
    const md::CachedMetadata* w = meta->fileCache("Welcome.md");
    ASSERT_TRUE(w);
    ASSERT_EQ(w->tags.size(), 2);
    const QHash<QString, int> tags = vaultTags(*meta);
    // "#tag" (x2, incl. the parent of #tag/sub) and "#Tag" merge; the more frequent spelling wins
    EXPECT_EQ(tags.value("#tag"), 3);
    EXPECT_EQ(tags.value("#tag/sub"), 1);
    EXPECT_FALSE(tags.contains("#Tag"));
    QList<TagSuggestion> ts = tagSuggestions(*meta, "");
    ASSERT_EQ(ts.size(), 2);
    EXPECT_EQ(ts[0].tag, "tag");
    ts = tagSuggestions(*meta, "sub");
    ASSERT_EQ(ts.size(), 1);
    EXPECT_EQ(ts[0].tag, "tag/sub");
}
