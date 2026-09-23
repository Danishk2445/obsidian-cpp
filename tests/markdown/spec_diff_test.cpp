// Differential tests of the block parser against cmark-gfm (and cmark 0.31.2 when installed):
// all CommonMark 0.31.2 spec examples, the GFM table examples and random grammar-generated
// documents without Obsidian-only syntax. Compares block types, line ranges, list tightness and
// heading levels after the normalisation described in cmark_oracle.h. Skipped when the tools are
// missing.
#include "markdown/cmark_oracle.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

#include <gtest/gtest.h>

using namespace octest;

namespace {

struct SpecExample {
    int number = 0;
    QString section;
    QString markdown;
};

std::vector<SpecExample> loadSpec()
{
    std::vector<SpecExample> out;
    QFile f(QStringLiteral(OC_SOURCE_DIR "/third_party/commonmark-spec/spec.json"));
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        out.push_back({o.value(QStringLiteral("example")).toInt(), o.value(QStringLiteral("section")).toString(),
                       o.value(QStringLiteral("markdown")).toString()});
    }
    return out;
}

bool verbose() { return qEnvironmentVariableIsSet("OC_SPEC_VERBOSE"); }

struct Tally {
    int total = 0, same = 0, obsidian = 0, quirks = 0;
    QStringList failures;
};

Tally runExamples(const QString& tool, const std::vector<SpecExample>& ex)
{
    std::vector<QString> docs;
    for (const SpecExample& e : ex)
        docs.push_back(e.markdown);
    const std::vector<QByteArray> xml = runCmarkBatch(tool, docs);
    Tally t;
    for (size_t i = 0; i < ex.size(); ++i) {
        const Comparison c = compareWithCmark(ex[i].markdown, xml[i], tool == QLatin1String("cmark-gfm"));
        ++t.total;
        if (c.same) {
            ++t.same;
            t.quirks += c.quirks ? 1 : 0;
            continue;
        }
        if (c.obsidian)
            ++t.obsidian;
        t.failures << QString::number(ex[i].number);
        if (verbose())
            fprintf(stderr, "---- %s example %d (%s)%s\n%s---- ours\n%s---- %s\n%s\n", qPrintable(tool), ex[i].number, qPrintable(ex[i].section),
                    c.obsidian ? " [obsidian syntax]" : "", qPrintable(ex[i].markdown), qPrintable(c.ours), qPrintable(tool), qPrintable(c.theirs));
    }
    return t;
}

// GFM spec table examples (198-205) plus a few extra table shapes.
std::vector<SpecExample> gfmTableExamples()
{
    const char* md[] = {
        "| foo | bar |\n| --- | --- |\n| baz | bim |\n",
        "| abc | defghi |\n:-: | -----------:\nbar | baz\n",
        "| f\\|oo  |\n| ------ |\n| b `\\|` az |\n| b **\\|** im |\n",
        "| abc | def |\n| --- | --- |\n| bar | baz |\n> bar\n",
        "| abc | def |\n| --- | --- |\n| bar | baz |\nbar\n\nbar\n",
        "| abc | def |\n| --- |\n| bar |\n",
        "| abc | def |\n| --- | --- |\n| bar |\n| bar | baz | boo |\n",
        "| abc | def |\n| --- | --- |\n",
        "para\n| a | b |\n|---|---|\n| 1 | 2 |\n",
        "a\n|-|\n",
        "| a |\n|-|\n|\n",
        "| a |\n| - |\n===\n",
        "| a |\n| - |\n    code\n",
        "- | a |\n  | - |\n  | b |\n",
        "> | a |\n> | - |\n> | b |\nlazy\n",
        "| a | b |\n| :-- | --: |\n| 1 | 2 |\n\n| c |\n| :-: |\n",
        "x\ny\n| a |\n| - |\n",
    };
    std::vector<SpecExample> out;
    int n = 1;
    for (const char* m : md)
        out.push_back({n++, QStringLiteral("tables"), QString::fromUtf8(m)});
    return out;
}

// ---- random CommonMark documents -------------------------------------------------------------------
class Gen {
public:
    explicit Gen(quint32 seed) : m_r(seed) {}
    QString doc()
    {
        QStringList lines;
        const int blocks = 1 + int(m_r.bounded(8));
        for (int i = 0; i < blocks; ++i) {
            lines += block(0);
            const int blanks = int(m_r.bounded(3)) == 0 ? int(m_r.bounded(3)) : 0;
            for (int b = 0; b < blanks; ++b)
                lines << blankLine();
        }
        QString s = lines.join(QLatin1Char('\n'));
        if (m_r.bounded(3))
            s += QLatin1Char('\n');
        return s;
    }

private:
    int pick(int n) { return int(m_r.bounded(n)); }
    QString word()
    {
        static const char* w[] = {"foo", "bar", "baz", "qux", "a", "b*", "_c_", "`x`", "\\*", "[l]", "&amp;", "<i>", "1.5", "x|y"};
        return QString::fromLatin1(w[pick(int(std::size(w)))]);
    }
    QString words(int n)
    {
        QStringList p;
        for (int i = 0; i < n; ++i)
            p << word();
        return p.join(QLatin1Char(' '));
    }
    QString blankLine()
    {
        switch (pick(5)) {
        case 0:
            return QStringLiteral(" ");
        case 1:
            return QStringLiteral("\t");
        default:
            return QString();
        }
    }
    QString indent()
    {
        static const char* in[] = {"", "", "", " ", "  ", "   ", "\t", "    ", " \t"};
        return QString::fromLatin1(in[pick(int(std::size(in)))]);
    }
    QString noise()
    {
        static const char* tok[] = {" ", "\t", ">", "> ", "-", "- ", "*", "* ", "+ ", "1. ", "2) ", "#", "# ", "```", "~~~", "|",
                                    "=", "==", "---", "***", "<div>", "</div>", "<!--", "-->", "a", "b ", "[x]: /u", "    ", "_", "`"};
        QString s;
        const int n = 1 + pick(5);
        for (int i = 0; i < n; ++i)
            s += QString::fromLatin1(tok[pick(int(std::size(tok)))]);
        return s;
    }
    QStringList paragraph()
    {
        QStringList out;
        const int n = 1 + pick(3);
        for (int i = 0; i < n; ++i) {
            if (i == 0 && pick(6) == 0)
                out << QStringLiteral("[ref%1]: /url%1%2").arg(pick(3)).arg(pick(2) ? QStringLiteral(" \"t\"") : QString());
            else
                out << (i ? indent() : QString()) + words(1 + pick(5));
        }
        return out;
    }
    QStringList fence()
    {
        const QChar ch = pick(2) ? u'`' : u'~';
        const int len = 3 + pick(3);
        QStringList out;
        out << QString(pick(4), u' ') + QString(len, ch) + (pick(2) ? QStringLiteral("js") : QString());
        const int body = pick(4);
        for (int i = 0; i < body; ++i)
            out << (pick(5) == 0 ? QString(3 + pick(3), ch) + QStringLiteral(" x") : indent() + words(1 + pick(3)));
        const int close = pick(6);
        if (close > 0)
            out << QString(pick(4), u' ') + QString(close == 1 ? len - 1 : len + pick(2), ch);
        return out;
    }
    QStringList table()
    {
        const int cols = 1 + pick(3);
        QStringList out;
        if (pick(4) == 0)
            out << words(2);
        QStringList h, d;
        for (int c = 0; c < cols; ++c) {
            h << words(1);
            static const char* al[] = {"---", ":--", "--:", ":-:", "-"};
            d << QString::fromLatin1(al[pick(5)]);
        }
        const bool pipes = pick(3) != 0;
        auto row = [&](const QStringList& cells) {
            QString r = cells.join(QStringLiteral(" | "));
            return pipes ? QStringLiteral("| ") + r + QStringLiteral(" |") : r;
        };
        out << row(h);
        if (pick(8) == 0)
            d << QStringLiteral("---");
        out << row(d);
        const int rows = pick(4);
        for (int r = 0; r < rows; ++r) {
            QStringList cells;
            for (int c = 0; c < cols + pick(2) - (cols > 1 ? pick(2) : 0); ++c)
                cells << words(1);
            out << row(cells);
        }
        return out;
    }
    QStringList html()
    {
        static const char* h[] = {"<div>", "<div class=\"x\">\nfoo", "<!-- c\nd -->", "<pre>\nx\n</pre>", "<a href=\"x\">", "</div>", "<?php\n?>",
                                  "<!DOCTYPE html>", "<![CDATA[\nx\n]]>", "<table><tr><td>", "<del>", "<script>\nx"};
        return QString::fromLatin1(h[pick(int(std::size(h)))]).split(QLatin1Char('\n'));
    }
    QStringList prefixed(const QStringList& inner, const QString& first, const QString& rest, bool lazyOk)
    {
        QStringList out;
        for (int i = 0; i < inner.size(); ++i) {
            if (i == 0)
                out << first + inner[i];
            else if (lazyOk && pick(6) == 0)
                out << inner[i];
            else
                out << (inner[i].isEmpty() && pick(2) ? rest.trimmed().isEmpty() ? QString() : rest.trimmed() : rest + inner[i]);
        }
        if (inner.isEmpty())
            out << first;
        return out;
    }
    QStringList blocks(int depth)
    {
        QStringList out;
        const int n = 1 + pick(depth > 2 ? 2 : 3);
        for (int i = 0; i < n; ++i) {
            if (i > 0 && pick(2))
                out << QString();
            out += block(depth);
        }
        return out;
    }
    QStringList block(int depth)
    {
        const int k = pick(depth > 3 ? 9 : 13);
        switch (k) {
        case 0:
        case 1:
            return paragraph();
        case 2:
            return {QString(pick(3), u' ') + QString(1 + pick(7), u'#') + (pick(4) ? QStringLiteral(" ") + words(2) : QString())
                    + (pick(3) == 0 ? QStringLiteral(" ##") : QString())};
        case 3: {
            QStringList p = paragraph();
            p << QString(pick(3), u' ') + QString(1 + pick(4), pick(2) ? u'=' : u'-');
            return p;
        }
        case 4: {
            static const char* hr[] = {"***", "- - -", "___", "* * *", " ---", "-----"};
            return {QString::fromLatin1(hr[pick(6)])};
        }
        case 5:
            return fence();
        case 6: {
            QStringList out;
            const int n = 1 + pick(3);
            for (int i = 0; i < n; ++i)
                out << (pick(4) == 0 ? QString() : (pick(2) ? QStringLiteral("    ") : QStringLiteral("\t")) + words(2));
            if (out.first().isEmpty())
                out.first() = QStringLiteral("    code");
            return out;
        }
        case 7:
            return html();
        case 8:
            return {noise()};
        case 9:
        case 10: {
            static const char* q[] = {"> ", ">", " > ", ">\t"};
            const QString p = QString::fromLatin1(q[pick(4)]);
            return prefixed(blocks(depth + 1), p, p, true);
        }
        case 11: {
            QStringList out;
            const bool ordered = pick(3) == 0;
            const QChar bullet = QStringLiteral("-*+")[pick(3)];
            const int items = 1 + pick(3);
            for (int i = 0; i < items; ++i) {
                QString marker = ordered ? QString::number(i + (pick(4) == 0 ? 3 : 1)) + (pick(5) ? u'.' : u')') : QString(bullet);
                const QString sep = pick(8) == 0 ? QStringLiteral("\t") : QString(1 + (pick(6) == 0 ? pick(5) : 0), u' ');
                const QString cont = pick(5) == 0 ? QStringLiteral("\t") : QString(marker.size() + sep.size(), u' ');
                QStringList inner = pick(7) == 0 ? QStringList{} : blocks(depth + 1);
                out += prefixed(inner, QString(pick(3), u' ') + marker + (inner.isEmpty() ? QString() : sep), cont, true);
                if (pick(3) == 0)
                    out << blankLine();
            }
            return out;
        }
        default:
            return table();
        }
    }

    QRandomGenerator m_r;
};

} // namespace

TEST(SpecDiff, CommonMarkSpecExamplesVsCmarkGfm)
{
    if (!cmarkAvailable(QStringLiteral("cmark-gfm")))
        GTEST_SKIP() << "cmark-gfm not installed";
    const std::vector<SpecExample> ex = loadSpec();
    ASSERT_EQ(ex.size(), 652u);
    const Tally t = runExamples(QStringLiteral("cmark-gfm"), ex);
    const double pct = 100.0 * t.same / t.total;
    printf("cmark-gfm: %d/%d spec examples agree (%.1f%%), %d mismatches involve Obsidian syntax\nmismatches: %s\n", t.same, t.total, pct,
           t.obsidian, qPrintable(t.failures.join(QLatin1Char(' '))));
    EXPECT_GE(pct, 97.0);
}

TEST(SpecDiff, CommonMarkSpecExamplesVsCmark0312)
{
    if (!cmarkAvailable(QStringLiteral("cmark")))
        GTEST_SKIP() << "cmark not installed";
    const std::vector<SpecExample> ex = loadSpec();
    const Tally t = runExamples(QStringLiteral("cmark"), ex);
    const double pct = 100.0 * t.same / t.total;
    printf("cmark: %d/%d spec examples agree (%.1f%%)\nmismatches: %s\n", t.same, t.total, pct, qPrintable(t.failures.join(QLatin1Char(' '))));
    EXPECT_GE(pct, 97.0);
}

TEST(SpecDiff, GfmTables)
{
    if (!cmarkAvailable(QStringLiteral("cmark-gfm")))
        GTEST_SKIP() << "cmark-gfm not installed";
    const std::vector<SpecExample> ex = gfmTableExamples();
    const Tally t = runExamples(QStringLiteral("cmark-gfm"), ex);
    printf("tables: %d/%d agree; mismatches: %s\n", t.same, t.total, qPrintable(t.failures.join(QLatin1Char(' '))));
    EXPECT_EQ(t.same, t.total);
}

TEST(SpecDiff, RandomDocumentsVsCmarkGfm)
{
    if (!cmarkAvailable(QStringLiteral("cmark-gfm")))
        GTEST_SKIP() << "cmark-gfm not installed";
    bool ok = false;
    int count = qEnvironmentVariableIntValue("OC_SPEC_RANDOM", &ok);
    if (!ok)
        count = 3000;
    Gen gen(20260922);
    std::vector<SpecExample> ex;
    for (int i = 0; i < count; ++i)
        ex.push_back({i, QStringLiteral("random"), gen.doc()});
    const Tally t = runExamples(QStringLiteral("cmark-gfm"), ex);
    const double pct = 100.0 * t.same / t.total;
    printf("random: %d/%d documents agree (%.2f%%), %d of them only modulo documented cmark tightness quirks\n", t.same, t.total, pct,
           t.quirks);
    EXPECT_GE(pct, 99.0) << "first mismatches: " << qPrintable(t.failures.mid(0, 20).join(QLatin1Char(' ')));
}
