#include "editor/export/pdf_exporter.h"
#include "render/theme/theme.h"

#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace oc;

TEST(PdfExport, WritesMultiPagePdf)
{
    ThemeManager themes;
    themes.setMode(ThemeManager::Mode::Dark);
    themes.apply();
    QString md = QStringLiteral("## Section\n\nSome **bold** text and a [[link]].\n\n");
    for (int i = 0; i < 200; ++i)
        md += QStringLiteral("- list item %1 with enough text to wrap maybe\n").arg(i);
    md += QStringLiteral("\n```bash\necho hi\n```\n");
    QTemporaryDir dir;
    const QString out = dir.path() + "/note.pdf";
    PdfExportSettings s; // user's settings: Letter, margin 0, 100%, includeName
    QString err;
    ASSERT_TRUE(exportPdf(Text::fromString(md), "My note", out, s, {}, &err)) << err.toStdString();
    QFile f(out);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray pdf = f.readAll();
    EXPECT_TRUE(pdf.startsWith("%PDF-"));
    // several pages
    int pages = 0;
    for (qsizetype i = pdf.indexOf("/Type /Page"); i >= 0; i = pdf.indexOf("/Type /Page", i + 1))
        if (pdf.mid(i, 12) != "/Type /Pages")
            ++pages;
    EXPECT_GE(pages, 3);
}
