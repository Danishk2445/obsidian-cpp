// Folding: Obsidian's fold services (heading / indent / frontmatter) and EditorView fold behaviour.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/fold/fold_service.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;

namespace {
struct Parsed {
    explicit Parsed(const char* s)
        : text(Text::fromString(QString::fromUtf8(s)))
    {
        tree.parse(text);
    }
    std::optional<FoldRange> at(int line, FoldOptions o = {}) const { return foldableAt(text, tree, line, o); }
    Text text;
    md::SyntaxTree tree;
};
} // namespace

TEST(FoldService, Headings)
{
    Parsed p("# A\ntext\n## B\nmore\n\n# C\nend");
    EXPECT_EQ(*p.at(0), (FoldRange{0, 4}));   // up to the next H1, blank line included
    EXPECT_EQ(*p.at(2), (FoldRange{2, 4}));   // H2 stops at the H1
    EXPECT_EQ(*p.at(5), (FoldRange{5, 6}));
    EXPECT_FALSE(p.at(6).has_value());        // last line
    Parsed empty("# A\n# B\n");
    EXPECT_FALSE(empty.at(0).has_value());    // nothing to fold before a same-level heading
    FoldOptions noHeadings;
    noHeadings.headings = false;
    EXPECT_FALSE(p.at(0, noHeadings).has_value());
    Parsed setext("Title\n=====\nbody\n");
    ASSERT_EQ(setext.tree.role(0), md::LineRole::SetextText);
    EXPECT_EQ(setext.at(0), (FoldRange{0, 3}));
}

TEST(FoldService, IndentAndFrontmatter)
{
    Parsed p("- a\n\t- b\n\n\t\t- c\n- d\n\n  x\ny");
    EXPECT_EQ(*p.at(0), (FoldRange{0, 3}));   // blank lines inside are skipped, stops at "- d"
    EXPECT_EQ(*p.at(1), (FoldRange{1, 3}));
    EXPECT_EQ(*p.at(4), (FoldRange{4, 6}));   // trailing blank lines stay visible
    EXPECT_FALSE(p.at(3).has_value());
    EXPECT_FALSE(p.at(2).has_value());        // blank line
    Parsed fm("---\na: 1\nb: 2\n---\nbody");
    EXPECT_EQ(*fm.at(0), (FoldRange{0, 3}));
    EXPECT_TRUE(isFoldable(fm.text, fm.tree, 0, {}));
    Parsed hr("text\n\n---\nmore\n---\n");
    EXPECT_FALSE(hr.at(2).has_value());       // "---" after content is not frontmatter
}

TEST(FoldService, CheapCheckAgreesWithFullOne)
{
    Parsed p("# A\ntext\n## B\n\n# C\n# D\n- x\n  - y\n- z\n");
    for (int l = 0; l < p.text.lineCount(); ++l)
        EXPECT_EQ(isFoldable(p.text, p.tree, l, {}), p.at(l).has_value()) << "line " << l;
}

class EditorFoldTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        s_themes = new ThemeManager();
        s_themes->apply();
    }
    static void TearDownTestSuite() { delete s_themes; }
    void open(const QByteArray& content)
    {
        ASSERT_TRUE(dir.isValid());
        storage = std::make_unique<LocalFileStorage>(dir.path());
        QFile f(dir.path() + "/t.md");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
        doc = std::make_shared<Document>(storage.get(), QStringLiteral("t.md"));
        ASSERT_TRUE(doc->load());
        view = std::make_unique<EditorView>();
        view->resize(700, 500);
        view->setDocument(doc);
        view->show();
        QApplication::processEvents();
    }
    static ThemeManager* s_themes;
    QTemporaryDir dir;
    std::unique_ptr<LocalFileStorage> storage;
    std::shared_ptr<Document> doc;
    std::unique_ptr<EditorView> view;
};
ThemeManager* EditorFoldTest::s_themes = nullptr;

TEST_F(EditorFoldTest, FoldHidesLinesAndCursorSkipsThem)
{
    open("# A\none\ntwo\n# B\nthree");
    const double before = view->layout().totalHeight();
    QSignalSpy changed(view.get(), &EditorView::foldsChanged);
    ASSERT_TRUE(view->foldLine(0));
    EXPECT_EQ(changed.count(), 1);
    EXPECT_TRUE(view->isFoldedAt(0));
    EXPECT_TRUE(view->layout().isHidden(1));
    EXPECT_TRUE(view->layout().isHidden(2));
    EXPECT_FALSE(view->layout().isHidden(3));
    EXPECT_LT(view->layout().totalHeight(), before);
    EXPECT_EQ(view->layout().lineHeight(1), 0);
    // the fold line shows the "…" placeholder widget
    const DocLayout::UnitLayout* ul = view->layout().layoutFor(0);
    ASSERT_NE(ul, nullptr);
    EXPECT_FALSE(ul->block.widgets().isEmpty());
    // moving right from the end of "# A" jumps over the fold
    view->setCursorPosition(3);
    view->moveHorizontal(1, false, false);
    EXPECT_EQ(view->cursorPosition(), doc->text().lineStart(3));
    view->moveHorizontal(-1, false, false);
    EXPECT_EQ(view->cursorPosition(), 3);
    // vertical movement skips hidden lines too
    view->moveVertical(1, false);
    EXPECT_EQ(doc->text().lineAt(view->cursorPosition()), 3);
    ASSERT_TRUE(view->unfoldLine(0));
    EXPECT_FALSE(view->layout().isHidden(1));
}

TEST_F(EditorFoldTest, EditsMapFoldsAndTypingAtTheFoldUnfolds)
{
    open("intro\n# A\none\ntwo\n# B\nthree");
    ASSERT_TRUE(view->foldLine(1));
    // an edit above shifts the fold
    view->setCursorPosition(0);
    view->insertText("x\n");
    ASSERT_EQ(view->folds().size(), 1);
    EXPECT_EQ(view->folds().first(), (FoldRange{2, 4}));
    EXPECT_TRUE(view->layout().isHidden(3));
    // typing at the end of the folded heading unfolds it
    view->setCursorPosition(doc->text().lineEnd(2));
    view->insertText("!");
    EXPECT_TRUE(view->folds().isEmpty());
    EXPECT_FALSE(view->layout().isHidden(3));
}

TEST_F(EditorFoldTest, FoldAllUnfoldAllAndKeys)
{
    open("# A\n- a\n\t- b\n# B\ntext\n");
    view->foldAll();
    EXPECT_TRUE(view->isFoldedAt(0));
    EXPECT_TRUE(view->isFoldedAt(1)); // nested folds too (Obsidian folds every foldable line)
    EXPECT_TRUE(view->isFoldedAt(3));
    view->unfoldAll();
    EXPECT_TRUE(view->folds().isEmpty());
    view->setFocus();
    view->setCursorPosition(1);
    QTest::keyClick(view.get(), Qt::Key_BraceLeft, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(view->isFoldedAt(0));
    QTest::keyClick(view.get(), Qt::Key_BraceRight, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_FALSE(view->isFoldedAt(0));
    QTest::keyClick(view.get(), Qt::Key_BracketLeft, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_FALSE(view->folds().isEmpty());
    QTest::keyClick(view.get(), Qt::Key_BracketRight, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_TRUE(view->folds().isEmpty());
}

TEST_F(EditorFoldTest, FoldMoreFoldLessAndRestore)
{
    open("# A\n## B\ntext\n## C\nmore\n");
    view->setCursorPosition(doc->text().lineStart(2));
    ASSERT_TRUE(view->foldMore()); // innermost: "## B"
    EXPECT_TRUE(view->isFoldedAt(1));
    ASSERT_TRUE(view->foldMore()); // then "# A"
    EXPECT_TRUE(view->isFoldedAt(0));
    ASSERT_TRUE(view->foldLess()); // outermost first
    EXPECT_FALSE(view->isFoldedAt(0));
    EXPECT_TRUE(view->isFoldedAt(1));
    const QList<FoldRange> saved = view->folds();
    view->unfoldAll();
    view->setFolds(saved + QList<FoldRange>{{3, 99}}); // invalid ones are dropped
    EXPECT_EQ(view->folds(), saved);
}

TEST_F(EditorFoldTest, ClickingTheIndicatorAndPlaceholder)
{
    open("# A\none\ntwo\n# B\nthree");
    const QRectF r = view->foldIndicatorRect(0);
    ASSERT_FALSE(r.isEmpty());
    QTest::mouseMove(view->viewport(), r.center().toPoint());
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, r.center().toPoint());
    EXPECT_TRUE(view->isFoldedAt(0));
    // clicking the "…" after the heading unfolds
    const DocLayout::UnitLayout* ul = view->layout().layoutFor(0);
    ASSERT_NE(ul, nullptr);
    ASSERT_FALSE(ul->widgetRects.isEmpty());
    const QRectF w = ul->widgetRects.first();
    const QPointF docPt(w.center().x(), view->layout().lineTop(0) + w.center().y());
    const QPointF vp = view->docToViewport(docPt);
    QTest::qWait(QApplication::doubleClickInterval() + 50);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, vp.toPoint());
    EXPECT_FALSE(view->isFoldedAt(0));
}
