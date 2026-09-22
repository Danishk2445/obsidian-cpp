// Differential tests of md::parseInline() on single-paragraph inputs:
//  - every example of the CommonMark 0.31.2 inline sections (third_party/commonmark-spec/spec.json) and
//    the GFM strikethrough / extended-autolink examples (github/cmark-gfm test/spec.txt, CC-BY-SA 4.0),
//  - compared with /usr/bin/cmark-gfm --to html --unsafe -e strikethrough -e autolink (primary oracle;
//    --unsafe so raw HTML is emitted instead of "<!-- raw HTML omitted -->"), and with the spec's own
//    expected HTML (divergences must be in kKnownSpecDivergences below).
// Examples that contain Obsidian-only syntax ([[ $ %% == ^[ [^ and #tags) or block structure (anything
// that is not exactly one paragraph after link reference definitions) are skipped.
#include "markdown/inline_html.h"
#include "markdown/inline_rules.h"
#include "markdown/inline_tree.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>
#include <gtest/gtest.h>

#include <cstdio>

using namespace oc;
using namespace oc::md;

namespace {

struct Case {
    QString id;       // "cm-123" / "gfm-autolink-4"
    QString section;
    QString markdown;
    QString expected; // expected HTML of the reference spec
};

// Divergences from the *spec's* expected HTML, with the reason (all intentional).
const QHash<QString, const char*> kKnownSpecDivergences = {
    {QStringLiteral("cm-602"), "GFM extended autolink: bare https://foo.bar/baz inside '<... bim>' (Obsidian links bare URLs)"},
    {QStringLiteral("cm-608"), "GFM extended autolink: bare https://foo.bar inside '< ... >'"},
    {QStringLiteral("cm-611"), "GFM extended autolink: bare https://example.com"},
    {QStringLiteral("cm-612"), "GFM extended email autolink: bare foo@bar.example.com"},
};

// Divergences from cmark-gfm 0.29.0.gfm.13 (compared with its Strong-in-Strong rendering quirk emulated).
const QHash<QString, const char*> kKnownCmarkDivergences = {
    {QStringLiteral("cm-28"), "CommonMark 0.31: numeric references have at most 7 digits (&#87654321; is text); cmark-gfm 0.29 decodes it"},
    {QStringLiteral("cm-606"), "cmark-gfm links the email in '<foo\\+@bar.example.com>' after merging the escaped '+' into the text; "
                              "escapes stay separate nodes here (matches the spec and Obsidian's reading view)"},
    {QStringLiteral("cm-354.2"), "CommonMark 0.31: Unicode symbols (S*) count as punctuation for flanking ('*£*bravo.' is not emphasis); cmark-gfm 0.29 predates it"},
    {QStringLiteral("cm-354.3"), "CommonMark 0.31: Unicode symbols (S*) count as punctuation for flanking ('*€*charlie.'); cmark-gfm 0.29 predates it"},
    {QStringLiteral("gfm-strike-single"), "Obsidian: strikethrough needs exactly two tildes (GFM also accepts one)"},
};

const QStringList kInlineSections = {
    QStringLiteral("Backslash escapes"),
    QStringLiteral("Entity and numeric character references"),
    QStringLiteral("Code spans"),
    QStringLiteral("Emphasis and strong emphasis"),
    QStringLiteral("Links"),
    QStringLiteral("Images"),
    QStringLiteral("Autolinks"),
    QStringLiteral("Raw HTML"),
    QStringLiteral("Hard line breaks"),
    QStringLiteral("Soft line breaks"),
    QStringLiteral("Textual content"),
};

// GFM spec (0.29-gfm) extension examples, one paragraph each (multi-paragraph examples split).
struct GfmExample {
    const char* id;
    const char* markdown;
    const char* html;
};
const GfmExample kGfmExamples[] = {
    {"gfm-strike-1", "~~Hi~~ Hello, world!", "<p><del>Hi</del> Hello, world!</p>\n"},
    {"gfm-strike-2a", "This ~~has a", "<p>This ~~has a</p>\n"},
    {"gfm-strike-2b", "new paragraph~~.", "<p>new paragraph~~.</p>\n"},
    // cmark-gfm also strikes single tildes; Obsidian does not (expected = Obsidian's rendering)
    {"gfm-strike-single", "~~Hi~~ Hello, ~there~ world!", "<p><del>Hi</del> Hello, ~there~ world!</p>\n"},
    {"gfm-autolink-1", "www.commonmark.org", "<p><a href=\"http://www.commonmark.org\">www.commonmark.org</a></p>\n"},
    {"gfm-autolink-2", "Visit www.commonmark.org/help for more information.",
     "<p>Visit <a href=\"http://www.commonmark.org/help\">www.commonmark.org/help</a> for more information.</p>\n"},
    {"gfm-autolink-3a", "Visit www.commonmark.org.", "<p>Visit <a href=\"http://www.commonmark.org\">www.commonmark.org</a>.</p>\n"},
    {"gfm-autolink-3b", "Visit www.commonmark.org/a.b.", "<p>Visit <a href=\"http://www.commonmark.org/a.b\">www.commonmark.org/a.b</a>.</p>\n"},
    {"gfm-autolink-4a", "www.google.com/search?q=Markup+(business)",
     "<p><a href=\"http://www.google.com/search?q=Markup+(business)\">www.google.com/search?q=Markup+(business)</a></p>\n"},
    {"gfm-autolink-4b", "www.google.com/search?q=Markup+(business)))",
     "<p><a href=\"http://www.google.com/search?q=Markup+(business)\">www.google.com/search?q=Markup+(business)</a>))</p>\n"},
    {"gfm-autolink-4c", "(www.google.com/search?q=Markup+(business))",
     "<p>(<a href=\"http://www.google.com/search?q=Markup+(business)\">www.google.com/search?q=Markup+(business)</a>)</p>\n"},
    {"gfm-autolink-4d", "(www.google.com/search?q=Markup+(business)",
     "<p>(<a href=\"http://www.google.com/search?q=Markup+(business)\">www.google.com/search?q=Markup+(business)</a></p>\n"},
    {"gfm-autolink-5", "www.google.com/search?q=(business))+ok",
     "<p><a href=\"http://www.google.com/search?q=(business))+ok\">www.google.com/search?q=(business))+ok</a></p>\n"},
    {"gfm-autolink-6a", "www.google.com/search?q=commonmark&hl=en",
     "<p><a href=\"http://www.google.com/search?q=commonmark&amp;hl=en\">www.google.com/search?q=commonmark&amp;hl=en</a></p>\n"},
    {"gfm-autolink-6b", "www.google.com/search?q=commonmark&hl;",
     "<p><a href=\"http://www.google.com/search?q=commonmark\">www.google.com/search?q=commonmark</a>&amp;hl;</p>\n"},
    {"gfm-autolink-7", "www.commonmark.org/he<lp", "<p><a href=\"http://www.commonmark.org/he\">www.commonmark.org/he</a>&lt;lp</p>\n"},
    {"gfm-autolink-8a", "http://commonmark.org", "<p><a href=\"http://commonmark.org\">http://commonmark.org</a></p>\n"},
    {"gfm-autolink-8b", "(Visit https://encrypted.google.com/search?q=Markup+(business))",
     "<p>(Visit <a href=\"https://encrypted.google.com/search?q=Markup+(business)\">https://encrypted.google.com/search?q=Markup+(business)</a>)</p>\n"},
    {"gfm-autolink-8c", "Anonymous FTP is available at ftp://foo.bar.baz.",
     "<p>Anonymous FTP is available at <a href=\"ftp://foo.bar.baz\">ftp://foo.bar.baz</a>.</p>\n"},
    {"gfm-autolink-9", "foo@bar.baz", "<p><a href=\"mailto:foo@bar.baz\">foo@bar.baz</a></p>\n"},
    {"gfm-autolink-10", "hello@mail+xyz.example isn't valid, but hello+xyz@mail.example is.",
     "<p>hello@mail+xyz.example isn't valid, but <a href=\"mailto:hello+xyz@mail.example\">hello+xyz@mail.example</a> is.</p>\n"},
    {"gfm-autolink-11a", "a.b-c_d@a.b", "<p><a href=\"mailto:a.b-c_d@a.b\">a.b-c_d@a.b</a></p>\n"},
    {"gfm-autolink-11b", "a.b-c_d@a.b.", "<p><a href=\"mailto:a.b-c_d@a.b\">a.b-c_d@a.b</a>.</p>\n"},
    {"gfm-autolink-11c", "a.b-c_d@a.b-", "<p>a.b-c_d@a.b-</p>\n"},
    {"gfm-autolink-11d", "a.b-c_d@a.b_", "<p>a.b-c_d@a.b_</p>\n"},
    // GFM emphasis-section examples mixing autolinks
    {"gfm-emph-1", "**a<http://foo.bar/?q=**>", "<p>**a<a href=\"http://foo.bar/?q=**\">http://foo.bar/?q=**</a></p>\n"},
    {"gfm-emph-2", "__a<http://foo.bar/?q=__>", "<p>__a<a href=\"http://foo.bar/?q=__\">http://foo.bar/?q=__</a></p>\n"},
};

bool isBlankLine(QStringView l)
{
    for (const QChar c : l)
        if (c != u' ' && c != u'\t')
            return false;
    return true;
}

qsizetype skipSpTab(QStringView s, qsizetype i)
{
    while (i < s.size() && (s[i] == u' ' || s[i] == u'\t'))
        ++i;
    return i;
}

// Test-side link reference definition parser (CommonMark 4.7) at the start of `s` + pos.
bool parseRefDef(QStringView s, qsizetype pos, qsizetype* end, LinkRefDefinition* out)
{
    qsizetype i = pos;
    for (int k = 0; k < 3 && i < s.size() && s[i] == u' '; ++k)
        ++i;
    qsizetype lf = 0, lt = 0;
    const qsizetype le = rules::scanLinkLabel(s, i, &lf, &lt);
    if (le < 0 || le >= s.size() || s[le] != u':')
        return false;
    if (s.mid(lf, lt - lf).trimmed().isEmpty())
        return false;
    i = skipSpTab(s, le + 1);
    if (i < s.size() && s[i] == u'\n')
        i = skipSpTab(s, i + 1);
    qsizetype df = 0, dt = 0;
    bool pointy = false;
    // scanLinkDestination needs a following char for raw destinations; append a sentinel view
    const QString padded = s.toString() + QLatin1Char('\n');
    const qsizetype de = rules::scanLinkDestination(padded, i, &df, &dt, &pointy);
    if (de < 0 || (!pointy && dt == df))
        return false;
    const qsizetype afterDest = de;
    auto lineEndOk = [&](qsizetype k) { return k >= s.size() || s[k] == u'\n'; };
    // with title
    qsizetype j = skipSpTab(s, afterDest);
    if (j < s.size() && s[j] == u'\n')
        j = skipSpTab(s, j + 1);
    if (j > afterDest) {
        qsizetype tf = 0, tt = 0;
        const qsizetype te = rules::scanLinkTitle(s, j, &tf, &tt);
        if (te >= 0) {
            const qsizetype k = skipSpTab(s, te);
            if (lineEndOk(k)) {
                out->label = rules::normalizeLinkLabel(s.mid(lf, lt - lf));
                out->destination = rules::unescapeString(s.mid(df, dt - df));
                out->title = rules::unescapeString(s.mid(tf, tt - tf));
                *end = k < s.size() ? k + 1 : k;
                return true;
            }
        }
    }
    const qsizetype k = skipSpTab(s, afterDest);
    if (!lineEndOk(k))
        return false;
    out->label = rules::normalizeLinkLabel(s.mid(lf, lt - lf));
    out->destination = rules::unescapeString(s.mid(df, dt - df));
    out->title.clear();
    *end = k < s.size() ? k + 1 : k;
    return true;
}

// Emulates the block parser for inputs made only of paragraphs and reference definitions: returns the
// paragraph contents (leading reference definitions of each block removed, lines with leading whitespace
// removed, final whitespace removed) and collects the definitions.
QStringList prepareParagraphs(const QString& markdown, QList<LinkRefDefinition>* refs)
{
    QString text = markdown;
    if (text.endsWith(u'\n'))
        text.chop(1);
    const QStringList lines = text.split(u'\n');
    QList<QStringList> blocks;
    QStringList cur;
    for (const QString& l : lines) {
        if (isBlankLine(l)) {
            if (!cur.isEmpty())
                blocks << cur;
            cur.clear();
        } else {
            cur << l;
        }
    }
    if (!cur.isEmpty())
        blocks << cur;
    QStringList paragraphs;
    for (const QStringList& b : blocks) {
        const QString joined = b.join(u'\n');
        qsizetype pos = 0;
        for (;;) {
            LinkRefDefinition def;
            qsizetype end = 0;
            if (!parseRefDef(joined, pos, &end, &def))
                break;
            bool dup = false;
            for (const LinkRefDefinition& d : *refs)
                dup = dup || d.label == def.label;
            if (!dup)
                refs->push_back(def);
            pos = end;
            if (pos >= joined.size())
                break;
        }
        const QString rest = joined.mid(pos);
        if (!rest.trimmed().isEmpty())
            paragraphs << rest;
    }
    QStringList out;
    for (const QString& p : paragraphs) {
        QStringList para = p.split(u'\n');
        for (QString& l : para)
            l = l.mid(skipSpTab(l, 0));
        QString content = para.join(u'\n');
        while (!content.isEmpty() && (content.back() == u' ' || content.back() == u'\t'))
            content.chop(1);
        out << content;
    }
    return out;
}

// The tree uses Obsidian-only syntax (then CommonMark / GFM output is not comparable).
bool usesObsidianSyntax(const InlineTree& t)
{
    for (const InlineNode& n : t.nodes()) {
        switch (n.type) {
        case InlineType::WikiLink:
        case InlineType::Embed:
        case InlineType::InlineMath:
        case InlineType::DisplayMath:
        case InlineType::Comment:
        case InlineType::Tag:
        case InlineType::FootnoteRef:
        case InlineType::InlineFootnote:
        case InlineType::BlockId:
        case InlineType::Highlight:
            return true;
        default:
            break;
        }
    }
    return false;
}

// "<p>...</p>\n<p>...</p>\n" -> paragraphs; empty if the HTML has anything else (other blocks).
QStringList splitParagraphs(const QString& html)
{
    QStringList out;
    qsizetype pos = 0;
    while (pos < html.size()) {
        if (!QStringView(html).mid(pos).startsWith(QLatin1StringView("<p>")))
            return {};
        const qsizetype end = html.indexOf(QLatin1StringView("</p>\n"), pos);
        if (end < 0)
            return {};
        const QString para = html.mid(pos, end + 5 - pos);
        if (para.count(QLatin1StringView("<p>")) != 1)
            return {};
        out << para;
        pos = end + 5;
    }
    return out;
}

QString runTool(const QString& program, const QStringList& args, const QString& markdown)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(5000))
        return QStringLiteral("<%1 failed to start>").arg(program);
    p.write(markdown.toUtf8());
    p.closeWriteChannel();
    p.waitForFinished(10000);
    return QString::fromUtf8(p.readAllStandardOutput());
}

// cmark-gfm with the strikethrough + autolink extensions (the oracle), or its CommonMark core alone.
QString runCmarkGfm(const QString& markdown, bool extensions = true)
{
    QStringList args = {QStringLiteral("--to"), QStringLiteral("html"), QStringLiteral("--unsafe")};
    if (extensions)
        args << QStringLiteral("-e") << QStringLiteral("strikethrough") << QStringLiteral("-e") << QStringLiteral("autolink");
    return runTool(QStringLiteral("/usr/bin/cmark-gfm"), args, markdown);
}

QList<Case> loadCases()
{
    QList<Case> cases;
    QFile f(QStringLiteral(OC_SOURCE_DIR "/third_party/commonmark-spec/spec.json"));
    if (!f.open(QIODevice::ReadOnly))
        return cases;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QString section = o.value(QStringLiteral("section")).toString();
        if (!kInlineSections.contains(section))
            continue;
        Case c;
        c.id = QStringLiteral("cm-%1").arg(o.value(QStringLiteral("example")).toInt());
        c.section = section;
        c.markdown = o.value(QStringLiteral("markdown")).toString();
        c.expected = o.value(QStringLiteral("html")).toString();
        cases << c;
    }
    for (const GfmExample& g : kGfmExamples)
        cases << Case{QString::fromUtf8(g.id), QStringLiteral("GFM"), QString::fromUtf8(g.markdown) + QLatin1Char('\n'), QString::fromUtf8(g.html)};
    return cases;
}

} // namespace

TEST(InlineSpec, CommonMarkAndGfmDifferential)
{
    const QList<Case> cases = loadCases();
    ASSERT_GT(cases.size(), 300) << "spec.json missing?";
    const bool haveCmark = QFile::exists(QStringLiteral("/usr/bin/cmark-gfm"));

    int considered = 0, examples = 0;
    int agreeSpec = 0, agreeCmark = 0, cmarkCompared = 0;
    QStringList specDiffs, cmarkDiffIds, unexpectedSpecDiffs, unexpectedCmarkDiffs, skippedObsidian, skippedBlock;
    auto json = [](const QString& s) { return QString::fromUtf8(QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact)); };
    for (const Case& c : cases) {
        QList<LinkRefDefinition> refs;
        const QStringList contents = prepareParagraphs(c.markdown, &refs);
        const QStringList expected = splitParagraphs(c.expected);
        if (contents.isEmpty() || contents.size() != expected.size()) {
            skippedBlock << c.id;
            continue;
        }
        const QStringList cmark = haveCmark ? splitParagraphs(runCmarkGfm(c.markdown)) : QStringList();
        bool counted = false;
        for (qsizetype k = 0; k < contents.size(); ++k) {
            const QString id = contents.size() == 1 ? c.id : QStringLiteral("%1.%2").arg(c.id).arg(k + 1);
            const InlineTreePtr tree = parseInline(contents[k], refs);
            if (usesObsidianSyntax(*tree)) {
                skippedObsidian << id;
                std::printf("[inline-spec]   (Obsidian syntax) %s %s\n    spec: %s    ours: %s", qPrintable(id), qPrintable(json(contents[k])),
                            qPrintable(expected[k]), qPrintable(test::renderParagraph(*tree)));
                continue;
            }
            ++considered;
            if (!counted) {
                ++examples;
                counted = true;
            }
            const QString ours = test::renderParagraph(*tree);
            if (ours == expected[k]) {
                ++agreeSpec;
            } else {
                specDiffs << id;
                if (!kKnownSpecDivergences.contains(id))
                    unexpectedSpecDiffs << QStringLiteral("%1 [%2]\n  md:       %3\n  expected: %4\n  ours:     %5")
                                               .arg(id, c.section, json(contents[k]), expected[k].trimmed(), ours.trimmed());
            }
            if (cmark.size() != contents.size())
                continue; // cmark-gfm sees other block structure here (e.g. a GFM table)
            ++cmarkCompared;
            const QString oursCompat = test::renderParagraph(*tree, true);
            if (cmark[k] == oursCompat) {
                ++agreeCmark;
            } else {
                cmarkDiffIds << id;
                if (!kKnownCmarkDivergences.contains(id))
                    unexpectedCmarkDiffs << QStringLiteral("%1 [%2]\n  md:        %3\n  cmark-gfm: %4\n  ours:      %5")
                                                .arg(id, c.section, json(contents[k]), cmark[k].trimmed(), oursCompat.trimmed());
            }
        }
    }
    std::printf("[inline-spec] %d paragraphs from %d examples considered\n", considered, examples);
    std::printf("[inline-spec] skipped, Obsidian syntax (%lld): %s\n", qlonglong(skippedObsidian.size()), qPrintable(skippedObsidian.join(QLatin1Char(' '))));
    std::printf("[inline-spec] skipped, block structure (%lld): %s\n", qlonglong(skippedBlock.size()), qPrintable(skippedBlock.join(QLatin1Char(' '))));
    std::printf("[inline-spec] agreement with the spec's expected HTML: %d/%d = %.1f%%\n", agreeSpec, considered, 100.0 * agreeSpec / std::max(1, considered));
    for (const QString& id : specDiffs)
        std::printf("[inline-spec]   spec divergence %s: %s\n", qPrintable(id), kKnownSpecDivergences.value(id, "UNEXPECTED"));
    if (haveCmark) {
        std::printf("[inline-spec] agreement with cmark-gfm: %d/%d = %.1f%%\n", agreeCmark, cmarkCompared, 100.0 * agreeCmark / std::max(1, cmarkCompared));
        for (const QString& id : cmarkDiffIds)
            std::printf("[inline-spec]   cmark-gfm divergence %s: %s\n", qPrintable(id), kKnownCmarkDivergences.value(id, "UNEXPECTED"));
    }
    for (const QString& d : unexpectedSpecDiffs)
        ADD_FAILURE() << "unexpected divergence from the spec: " << qPrintable(d);
    for (const QString& d : unexpectedCmarkDiffs)
        ADD_FAILURE() << "unexpected divergence from cmark-gfm: " << qPrintable(d);
    EXPECT_GE(agreeSpec * 100, considered * 95);
    if (haveCmark)
        EXPECT_GE(agreeCmark * 100, cmarkCompared * 95);
    else
        std::printf("[inline-spec] /usr/bin/cmark-gfm not installed: differential part skipped\n");
}

namespace {

// Runs many independent paragraphs through one cmark-gfm process (separated by a marker paragraph) and
// returns the HTML of each, or "" where the output is not a single paragraph. `prefix` (link reference
// definitions) is put in front of the document; it must not produce output.
QStringList cmarkGfmBatch(const QStringList& paragraphs, const QString& prefix)
{
    static const QString marker = QStringLiteral("QQMARKERQQ");
    QString doc = prefix;
    for (const QString& p : paragraphs)
        doc += p + QStringLiteral("\n\n") + marker + QStringLiteral("\n\n");
    const QString html = runCmarkGfm(doc);
    QStringList parts = html.split(QStringLiteral("<p>") + marker + QStringLiteral("</p>\n"));
    if (parts.size() != paragraphs.size() + 1) {
        // some input opened a block that swallowed markers (fence, HTML block...): one process each
        parts.clear();
        for (const QString& p : paragraphs)
            parts << runCmarkGfm(prefix + p);
    }
    QStringList out;
    for (qsizetype k = 0; k < paragraphs.size(); ++k)
        out << (splitParagraphs(parts[k]).size() == 1 ? parts[k] : QString());
    return out;
}

QString runMarkdownIt(const QString& markdown)
{
    QTemporaryFile f; // the markdown-it CLI reads a file, not stdin
    if (!f.open())
        return QString();
    f.write(markdown.toUtf8());
    f.close();
    QProcess p;
    p.start(QStringLiteral("/usr/bin/markdown-it"), {f.fileName()});
    p.waitForFinished(20000);
    return QString::fromUtf8(p.readAllStandardOutput());
}

// Tie-breaker for a mismatch with cmark-gfm: its CommonMark core (0.29) has known bugs, e.g. after a
// link a later '[' re-enables the link openers before it (nested links), the old per-character
// openers_bottom of process_emphasis, "![^x](y)" read as '!' + link. When two independent CommonMark
// 0.31 implementations (md4c, markdown-it; both print cmark's XHTML format) agree with each other but
// not with cmark-gfm's core, cmark-gfm cannot judge the input: if its extensions do not change its
// output, our HTML must equal theirs; otherwise the input is only counted.
bool cmarkCoreQuirk(const QString& markdown, const QString& gfmHtml, const QString& ours)
{
    static const bool haveOthers = QFile::exists(QStringLiteral("/usr/bin/md2html")) && QFile::exists(QStringLiteral("/usr/bin/markdown-it"));
    if (!haveOthers)
        return false;
    const QString core = runCmarkGfm(markdown, false);
    const QString md4c = runTool(QStringLiteral("/usr/bin/md2html"), {QStringLiteral("-x")}, markdown);
    if (md4c == core || runMarkdownIt(markdown) != md4c)
        return false;
    return core != gfmHtml || ours == md4c;
}

struct DiffStats {
    int compared = 0;
    int coreQuirks = 0; // inputs hitting a cmark-gfm core bug (see cmarkCoreQuirk())
    QStringList mismatches;
};

void compareWithCmark(const QStringList& inputs, DiffStats* stats, const QList<LinkRefDefinition>& prefixRefs = {}, const QString& prefix = {})
{
    const QStringList ref = cmarkGfmBatch(inputs, prefix);
    for (qsizetype k = 0; k < inputs.size(); ++k) {
        if (ref[k].isEmpty())
            continue;
        QList<LinkRefDefinition> refs = prefixRefs;
        const QStringList contents = prepareParagraphs(inputs[k], &refs);
        if (contents.size() != 1)
            continue;
        const InlineTreePtr tree = parseInline(contents[0], refs);
        if (usesObsidianSyntax(*tree))
            continue;
        ++stats->compared;
        const QString ours = test::renderParagraph(*tree, true);
        if (ours == ref[k])
            continue;
        if (cmarkCoreQuirk(prefix + inputs[k], ref[k], test::renderParagraph(*tree))) {
            ++stats->coreQuirks;
            continue;
        }
        QString esc = inputs[k];
        esc.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        stats->mismatches << QStringLiteral("input: %1\n  cmark-gfm: %2\n  ours:      %3").arg(esc, ref[k].trimmed(), ours.trimmed());
    }
}

} // namespace

// Autolink / emphasis edge cases checked against cmark-gfm only.
TEST(InlineSpec, CmarkGfmEdgeCases)
{
    if (!QFile::exists(QStringLiteral("/usr/bin/cmark-gfm")))
        GTEST_SKIP() << "cmark-gfm not installed";
    const QStringList inputs = {
        QStringLiteral("www.."), QStringLiteral("www.a.."), QStringLiteral("[ a@b.co"), QStringLiteral("[ http://x.co"),
        QStringLiteral("see http://x.co/a_b_ and"), QStringLiteral("http://a.b/c?d=e&f=g;"), QStringLiteral("*www.x.co*"), QStringLiteral("~~www.x.co~~"),
        QStringLiteral("x http://x.co)"), QStringLiteral("http://x.co/(a)"), QStringLiteral("foo@bar.baz_"), QStringLiteral("Foo.Bar@x.co"),
        QStringLiteral("a@b.c.d"), QStringLiteral("<a@b.co>"), QStringLiteral("1.2.3@x.y"), QStringLiteral("x@y.co."), QStringLiteral("http://x.co/\"q\""),
        QStringLiteral("https://x.co/a.b."), QStringLiteral("www.x.co/~u/"), QStringLiteral("ftp://a"), QStringLiteral("FTP://a"), QStringLiteral("wWw.x.co"),
        QStringLiteral("ahttp://x.com"), QStringLiteral("1http://x.com"), QStringLiteral("(http://x.com)"), QStringLiteral("*http://x.com*"),
        QStringLiteral("xwww.x.com"), QStringLiteral("_www.x.com_"), QStringLiteral("http://x"), QStringLiteral("http://"),
        QStringLiteral("https://a_b.c_d.com/"), QStringLiteral("www.a_b.c"), QStringLiteral("www.x.c_d"), QStringLiteral("http://x.com/a_"),
        QStringLiteral("http://x.com?"), QStringLiteral("http://x.com/a\"b"), QStringLiteral("http://x.com/a'"), QStringLiteral("a+b@c.d"),
        QStringLiteral("a@b.c_"), QStringLiteral("a@b.c-"), QStringLiteral("a@b"), QStringLiteral("@b.c"), QStringLiteral("a.@b.c"),
        QStringLiteral("mailto:a@b.c."), QStringLiteral("xmpp:a@b.c/x/y"), QStringLiteral("HTTP://X.COM"), QStringLiteral("Www.x.com"),
        QStringLiteral("http://x.com<b"), QStringLiteral("http://x.com&amp;"), QStringLiteral("http://x.com/?a=1&b=2;"), QStringLiteral("user@www.x.com"),
        QStringLiteral("_a@b.c_"), QStringLiteral("foo_bar@x.com"), QStringLiteral("x!a@b.c"), QStringLiteral("a@w.com"), QStringLiteral("w@b.c"),
        QStringLiteral("ab:cd@e.fg"), QStringLiteral("a@b.c:d"), QStringLiteral("*a@b.c*"), QStringLiteral("a@b.c@d.e"), QStringLiteral("amailto:a@b.c"),
        QStringLiteral("[http://a.b](http://c.d)"), QStringLiteral("[a](b) http://c.d"), QStringLiteral("![http://a.b](c)"), QStringLiteral("<http://a.b>www.c.d"),
        QStringLiteral("**http://a.b**c"), QStringLiteral("http://a.b/**c**"), QStringLiteral("`http://a.b` http://c.d"), QStringLiteral("http://a.b/`c`"),
        QStringLiteral("www.a.b/c(d)e)"), QStringLiteral("www.a.b/(((c)))"), QStringLiteral("http://a.b/c&lt;"), QStringLiteral("x@y.z)"),
        // non-ASCII characters end cmark-gfm's (byte-wise) domain check: later underscores are not seen
        QStringLiteral("http://a.bcé_x"), QStringLiteral("https://é_a.b_c"), QStringLiteral("www.a.bé_c"), QStringLiteral("www.aé.b_c.d_e"),
        QStringLiteral("http://a_b.c_d—x"), QStringLiteral("http://日本_x.y_z"), QStringLiteral("www.😀.a_b"), QStringLiteral("http://a.b«_c"),
    };
    DiffStats stats;
    compareWithCmark(inputs, &stats);
    std::printf("[inline-spec] edge cases: %d compared, %lld mismatches\n", stats.compared, qlonglong(stats.mismatches.size()));
    for (const QString& m : stats.mismatches)
        ADD_FAILURE() << qPrintable(m);
}

namespace {

// Inputs where cmark-gfm 0.29.0.gfm.13 is known to differ from CommonMark 0.31.2 (or has bugs); the
// random differential avoids them.
bool hasKnownCmarkQuirk(const QString& s)
{
    // 0.31: "<!" + any ASCII letter starts a declaration (0.29: uppercase only)
    for (qsizetype i = 0; i + 2 < s.size(); ++i)
        if (s[i] == u'<' && s[i + 1] == u'!' && s[i + 2] >= u'a' && s[i + 2] <= u'z')
            return true;
    // cmark-gfm merges escaped chars / entities into the text its email autolinker scans, and loses
    // track of links nested through an autolink (emails inside link text get linked)
    if (s.contains(u'@') && (s.contains(u'\\') || s.contains(u'&') || (s.contains(u'<') && s.contains(QLatin1StringView("](")))))
        return true;
    // cmark-gfm's email autolinker restarts at a second '@' of a domain ("a@b.co@b") without resetting
    // its dot count, so it links "b.co@b" although GFM requires a '.' in the domain
    static const QRegularExpression doubleAt(QStringLiteral("@[A-Za-z0-9._-]*@"));
    if (s.contains(doubleAt))
        return true;
    // cmark-gfm skips '~' (strikethrough) when computing the flanking of neighbouring delimiter runs
    for (const char* pair : {"*~", "~*", "_~", "~_"})
        if (s.contains(QLatin1StringView(pair)))
            return true;
    // cmark 0.29 accepts unbalanced '(' in a link destination
    for (qsizetype i = s.indexOf(QLatin1StringView("](")); i >= 0; i = s.indexOf(QLatin1StringView("]("), i + 1)) {
        int depth = 0;
        for (qsizetype k = i + 2; k < s.size() && !QChar(s[k]).isSpace(); ++k)
            depth += s[k] == u'(' ? 1 : s[k] == u')' ? -1 : 0;
        if (depth > 0)
            return true;
    }
    // cmark 0.29's backtick-closer cache goes stale after an unmatched backtick run
    QList<int> runs;
    for (qsizetype i = 0; i < s.size();) {
        if (s[i] != u'`') {
            ++i;
            continue;
        }
        qsizetype k = i;
        while (k < s.size() && s[k] == u'`')
            ++k;
        runs << int(k - i);
        i = k;
    }
    for (qsizetype a = 0; a < runs.size(); ++a) {
        bool closed = false;
        for (qsizetype b = a + 1; b < runs.size() && !closed; ++b)
            closed = runs[b] == runs[a];
        if (!closed && QSet<int>(runs.begin(), runs.end()).size() > 1)
            return true;
    }
    return false;
}

struct FuzzProfile {
    const char* name;
    QString alphabet;
    QStringList chunks;
    QString prefix;                   // reference definitions in front of every batch
    QList<LinkRefDefinition> refs;    // the same definitions for parseInline()
};

LinkRefDefinition refDef(const char* label, const char* dest, const char* title = "")
{
    return LinkRefDefinition{rules::normalizeLinkLabel(QString::fromUtf8(label)), QString::fromUtf8(dest), QString::fromUtf8(title), 0};
}

QList<FuzzProfile> fuzzProfiles()
{
    FuzzProfile plain;
    plain.name = "plain";
    plain.alphabet = QStringLiteral("ab *_`[]()!<>&;\\\".,:/@-\nx'w");
    plain.chunks = {QStringLiteral("**"),       QStringLiteral("__"),       QStringLiteral("~~"),        QStringLiteral("http://a.bc"),
                    QStringLiteral("www.a.bc"), QStringLiteral("a@b.co"),   QStringLiteral("&amp;"),     QStringLiteral("&#35;"),
                    QStringLiteral("<b>"),      QStringLiteral("</b>"),     QStringLiteral("<a href=\"x\">"), QStringLiteral("](b)"),
                    QStringLiteral("](<c d>)"), QStringLiteral("[x]"),      QStringLiteral("![y"),       QStringLiteral("``"),
                    QStringLiteral("  \n"),     QStringLiteral("\\\n"),     QStringLiteral("é"),         QStringLiteral("](b \"t\")"),
                    QStringLiteral("***"),      QStringLiteral("_*"),       QStringLiteral("<x@y.zz>"),  QStringLiteral("<http://q.r>")};
    // Wider character set (tabs, digits, Unicode punctuation, NBSP, Obsidian-ish characters that must stay
    // text on their own) and reference links against a fixed set of definitions.
    FuzzProfile wide;
    wide.name = "wide+refs";
    wide.alphabet = QStringLiteral("ab *_`[]()!<>&;\\\"'.,:/@-\n\tx1=|^#$%{}+?w") + QChar(0x2014) + QChar(0xAB) + QChar(0xA0) + QChar(0xE9);
    wide.chunks = {QStringLiteral("[x]"),        QStringLiteral("[y][]"),       QStringLiteral("[Foo  bar][x]"), QStringLiteral("![x]"),
                   QStringLiteral("[FOO BAR]"),  QStringLiteral("][y]"),        QStringLiteral("[ẞ]"),          QStringLiteral("[z]"),
                   QStringLiteral("](u 't')"),   QStringLiteral("](<>)"),       QStringLiteral("]( b )"),       QStringLiteral("](a(b)c)"),
                   QStringLiteral("**"),         QStringLiteral("__"),          QStringLiteral("~~"),           QStringLiteral("``"),
                   QStringLiteral("http://a.b"), QStringLiteral("www.a.b/c"),   QStringLiteral("&quot;"),       QStringLiteral("&#x41;"),
                   QStringLiteral("<i>"),        QStringLiteral("</i>"),        QStringLiteral("<!-- c -->"),   QStringLiteral("<?p?>"),
                   QStringLiteral("  \n"),       QStringLiteral("\\\n"),        QStringLiteral("\\*"),          QStringLiteral("\\["),
                   QStringLiteral("***"),        QStringLiteral("*_"),          QStringLiteral("`a`"),          QStringLiteral("[^x")};
    wide.prefix = QStringLiteral("[x]: /u\n[y]: </v w> \"t\"\n[Foo  Bar]: /f 'q'\n[ẞ]: /ss\n\n");
    wide.refs = {refDef("x", "/u"), refDef("y", "/v w", "t"), refDef("Foo  Bar", "/f", "q"), refDef("ẞ", "/ss")};
    return {plain, wide};
}

} // namespace

// Random CommonMark/GFM inputs (no Obsidian syntax) compared with cmark-gfm (md4c and markdown-it break
// ties where cmark-gfm's CommonMark core is wrong, see cmarkCoreQuirk()).
TEST(InlineSpec, RandomDifferentialWithCmarkGfm)
{
    if (!QFile::exists(QStringLiteral("/usr/bin/cmark-gfm")))
        GTEST_SKIP() << "cmark-gfm not installed";
    // OC_INLINE_FUZZ_ITERS / OC_INLINE_FUZZ_SEED widen the search locally (iterations per profile)
    const int iters = qEnvironmentVariableIsSet("OC_INLINE_FUZZ_ITERS") ? qEnvironmentVariableIntValue("OC_INLINE_FUZZ_ITERS") : 3000;
    const quint32 seed = qEnvironmentVariableIsSet("OC_INLINE_FUZZ_SEED") ? quint32(qEnvironmentVariableIntValue("OC_INLINE_FUZZ_SEED")) : 20260922u;
    for (const FuzzProfile& prof : fuzzProfiles()) {
        QRandomGenerator r(seed);
        QStringList inputs;
        for (int iter = 0; iter < iters; ++iter) {
            QString s;
            const int len = 1 + r.bounded(24);
            for (int k = 0; k < len; ++k) {
                if (r.bounded(3) == 0)
                    s += prof.chunks[r.bounded(int(prof.chunks.size()))];
                else
                    s += prof.alphabet[r.bounded(int(prof.alphabet.size()))];
            }
            // keep paragraphs self-contained in the batch: no reference definitions, no blank lines
            if (s.contains(QLatin1StringView("]:")) || s.contains(QLatin1StringView("\n\n")) || s.trimmed().isEmpty() || hasKnownCmarkQuirk(s))
                continue;
            inputs << s;
        }
        DiffStats stats;
        for (qsizetype from = 0; from < inputs.size(); from += 500)
            compareWithCmark(inputs.mid(from, 500), &stats, prof.refs, prof.prefix);
        std::printf("[inline-spec] random differential (%s): %d compared, %lld mismatches, %d cmark-gfm core quirks\n",
                    prof.name, stats.compared, qlonglong(stats.mismatches.size()), stats.coreQuirks);
        for (const QString& m : stats.mismatches.mid(0, 40))
            std::printf("[inline-spec] %s\n", qPrintable(m));
        EXPECT_TRUE(stats.mismatches.isEmpty()) << prof.name;
    }
}
