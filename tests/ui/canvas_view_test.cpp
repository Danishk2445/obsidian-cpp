// The canvas view (Obsidian's canvas core plugin) in a real session and window: loading and byte-stable
// saving, mouse and keyboard interactions (create, select, box-select, move with snapping, clone, resize,
// connect, reconnect, delete, undo/redo, copy/paste), viewport (pan, zoom limits, zoom to fit), menus,
// in-place editing, commands and renames. With OC_SHOT_DIR set, writes canvas-{dark,light}.png and
// canvas-zoom-{dark,light}.png.
#include "ui/app_test_support.h"

#include "core/canvas/canvas_model.h"
#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "render/theme/theme.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/views/canvas/canvas_controls.h"
#include "ui/views/canvas/canvas_registration.h"
#include "ui/views/canvas/canvas_render.h"
#include "ui/views/canvas/canvas_view.h"
#include "ui/views/canvas/canvas_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>

using namespace oc;
using namespace oc::test;
using namespace oc::canvas;

namespace {

ThemeManager* themes()
{
    if (!ThemeManager::instance()) {
        static ThemeManager* tm = [] {
            auto* m = new ThemeManager(qApp);
            FontSettings fonts;
            fonts.interfaceFamily = Theme::firstInstalled({QStringLiteral("Inter")}, QString());
            fonts.textFamily = fonts.interfaceFamily;
            m->setFonts(fonts);
            m->setMode(ThemeManager::Mode::Dark);
            m->apply();
            return m;
        }();
        return tm;
    }
    return ThemeManager::instance();
}

QByteArray fixtureBytes(const QString& path)
{
    QFile f(QStringLiteral(OC_FIXTURES_DIR "/") + path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Every node type and edge style (a file in Obsidian's layout).
const QByteArray kShowcase = R"json({
	"nodes":[
		{"id":"g1","type":"group","x":-520,"y":-80,"width":1760,"height":800,"color":"5","label":"Research board"},
		{"id":"g2","type":"group","x":1320,"y":-80,"width":420,"height":320,"label":"Later"},
		{"id":"t1","type":"text","text":"# Canvas\n\nCards render **Markdown**:\n- lists and `code`\n- [[Plan]] links\n- [[Nowhere]]","x":0,"y":0,"width":320,"height":260},
		{"id":"f1","type":"file","file":"Notes/Idea.md","x":400,"y":0,"width":360,"height":360},
		{"id":"f2","type":"file","file":"Notes/Idea.md","subpath":"#Details","x":920,"y":0,"width":300,"height":200,"color":"4"},
		{"id":"i1","type":"file","file":"attachments/image.png","x":920,"y":340,"width":300,"height":200},
		{"id":"p1","type":"file","file":"docs/Manual.pdf","x":0,"y":340,"width":220,"height":140},
		{"id":"m1","type":"file","file":"Mini.canvas","x":260,"y":480,"width":260,"height":200},
		{"id":"x1","type":"file","file":"Missing.md","x":620,"y":480,"width":200,"height":140},
		{"id":"l1","type":"link","url":"https://obsidian.md","x":-460,"y":0,"width":300,"height":200,"color":"#e0ac00"},
		{"id":"t2","type":"text","text":"A **red** card","x":-460,"y":320,"width":300,"height":100,"color":"1"},
		{"id":"t3","type":"text","text":"Purple","x":-460,"y":560,"width":300,"height":80,"color":"6"},
		{"id":"t4","type":"text","text":"Inside a group","x":1380,"y":-20,"width":300,"height":100}
	],
	"edges":[
		{"id":"e1","fromNode":"t1","fromSide":"right","toNode":"f1","toSide":"left"},
		{"id":"e2","fromNode":"f1","fromSide":"right","toNode":"f2","toSide":"left","color":"2","label":"details"},
		{"id":"e3","fromNode":"f2","fromSide":"bottom","toNode":"i1","toSide":"top","toEnd":"none"},
		{"id":"e4","fromNode":"t2","fromSide":"top","toNode":"l1","toSide":"bottom","fromEnd":"arrow","color":"#00bfbc"},
		{"id":"e5","fromNode":"p1","fromSide":"bottom","toNode":"m1","toSide":"left","color":"3"},
		{"id":"e6","fromNode":"t3","fromSide":"right","toNode":"p1","toSide":"left","label":"see also"},
		{"id":"e7","fromNode":"i1","fromSide":"right","toNode":"t4","toSide":"left","color":"6"},
		{"id":"e8","fromNode":"m1","fromSide":"right","toNode":"x1","toSide":"left","fromEnd":"arrow","toEnd":"none"}
	]
})json";

const QByteArray kMini = R"json({
	"nodes":[
		{"id":"a","type":"group","x":0,"y":0,"width":500,"height":300,"color":"2"},
		{"id":"b","type":"text","text":"x","x":40,"y":40,"width":160,"height":80},
		{"id":"c","type":"text","text":"y","x":300,"y":180,"width":160,"height":80,"color":"4"}
	],
	"edges":[
		{"id":"e","fromNode":"b","fromSide":"right","toNode":"c","toSide":"left"}
	]
})json";

// Cards at known places for the interaction tests.
const QByteArray kPlain = R"json({
	"nodes":[
		{"id":"a","type":"text","text":"Alpha","x":0,"y":0,"width":200,"height":100},
		{"id":"b","type":"text","text":"Beta","x":400,"y":0,"width":200,"height":100},
		{"id":"c","type":"text","text":"Gamma","x":0,"y":300,"width":200,"height":100}
	],
	"edges":[
		{"id":"ab","fromNode":"a","fromSide":"right","toNode":"b","toSide":"left"}
	]
})json";

class CanvasTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {
            {"Notes/Idea.md", "# Idea\n\nSome **important** idea with a [[Plan]] link.\n\n## Details\n\n- first\n- second\n\n## Other\n\nMore.\n"},
            {"Plan.md", "# Plan\n\nStep one.\n"},
            {"attachments/image.png", fixtureBytes("vault-basic/attachments/image.png")},
            {"docs/Manual.pdf", "%PDF-1.4\n% placeholder\n"},
            {"Showcase.canvas", kShowcase},
            {"Mini.canvas", kMini},
            {"Plain.canvas", kPlain},
            {"Empty.canvas", ""},
            {"Broken.canvas", "{\"nodes\":["},
        };
    }
    void SetUp() override
    {
        themes();
        CanvasWidget::setAnimationsEnabledByDefault(false);
        AppFixture::SetUp();
        if (HasFatalFailure())
            return;
        registerCanvasView(*registry);
        commands = new CommandRegistry;
        session->setCommands(commands);
        registerCanvasCommands(*commands, window);
        window->resize(1400, 850);
    }
    void TearDown() override
    {
        AppFixture::TearDown();
        delete commands;
    }

    CanvasView* openCanvas(const QString& path, Workspace::NewLeaf mode = Workspace::NewLeaf::No)
    {
        Workspace* ws = window->workspace();
        ws->openFile(vault->file(path), mode);
        auto* cv = qobject_cast<CanvasView*>(ws->activeRootLeaf()->view());
        if (cv) {
            QApplication::processEvents();
            cv->canvas()->setFocus();
        }
        return cv;
    }
    static QPoint at(CanvasWidget* c, QPointF canvasPt) { return c->mapFromCanvas(canvasPt).toPoint(); }
    // Mouse events with modifiers on every move (QTest::mouseMove has none).
    static void send(QWidget* vp, QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
    {
        QMouseEvent ev(type, QPointF(pos), vp->mapToGlobal(QPointF(pos)), button, buttons, mods);
        QApplication::sendEvent(vp, &ev);
    }
    static void dragView(CanvasWidget* c, QPoint a, QPoint b, Qt::KeyboardModifiers mods = {}, Qt::MouseButton button = Qt::LeftButton,
                         int steps = 6)
    {
        QWidget* vp = c->viewport();
        send(vp, QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton, mods);
        send(vp, QEvent::MouseButtonPress, a, button, button, mods);
        for (int i = 1; i <= steps; ++i)
            send(vp, QEvent::MouseMove, a + (b - a) * i / steps, Qt::NoButton, button, mods);
        send(vp, QEvent::MouseButtonRelease, b, button, Qt::NoButton, mods);
    }
    static void drag(CanvasWidget* c, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}, int steps = 6)
    {
        dragView(c, at(c, from), at(c, to), mods, Qt::LeftButton, steps);
    }
    static void click(CanvasWidget* c, QPointF pos, Qt::KeyboardModifiers mods = {})
    {
        QWidget* vp = c->viewport();
        const QPoint p = at(c, pos);
        send(vp, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, mods);
        send(vp, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
        send(vp, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
    }
    static void dclick(CanvasWidget* c, QPointF pos)
    {
        QWidget* vp = c->viewport();
        const QPoint p = at(c, pos);
        send(vp, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, {});
        send(vp, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, {});
        send(vp, QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton, {});
        send(vp, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, {});
    }
    QByteArray disk(const QString& path) const { return readFile(root + "/" + path); }

    CommandRegistry* commands = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Loading and saving
// ---------------------------------------------------------------------------------------------

TEST_F(CanvasTest, OpensCanvasFilesAndFitsThem)
{
    CanvasView* cv = openCanvas("Showcase.canvas");
    ASSERT_TRUE(cv);
    EXPECT_EQ(cv->viewType(), "canvas");
    EXPECT_EQ(cv->icon(), "layout-dashboard");
    EXPECT_EQ(cv->displayText(), "Showcase");
    CanvasWidget* c = cv->canvas();
    EXPECT_EQ(c->data().nodes.size(), 13);
    EXPECT_EQ(c->data().edges.size(), 8);
    // Zoomed to fit (10% margin, never above 100%).
    const Viewport v = c->canvasViewport();
    const QRectF box = allNodesBounds(c->data());
    EXPECT_NEAR(v.x, box.center().x(), 0.01);
    EXPECT_NEAR(v.y, box.center().y(), 0.01);
    const auto z = zoomToRect(box, QSizeF(c->viewport()->size()));
    ASSERT_TRUE(z);
    EXPECT_NEAR(v.zoom, z->zoom, 1e-9);
    EXPECT_LT(v.zoom, 0);
    // View state: file + viewport (Obsidian's viewState).
    const QJsonObject st = cv->state();
    EXPECT_EQ(st.value("file").toString(), "Showcase.canvas");
    EXPECT_NEAR(st.value("viewState").toObject().value("zoom").toDouble(), v.zoom, 1e-9);
}

TEST_F(CanvasTest, UntouchedCanvasIsNeverWritten)
{
    const qint64 before = QFileInfo(root + "/Showcase.canvas").lastModified().toMSecsSinceEpoch();
    CanvasView* cv = openCanvas("Showcase.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->zoomBy(0.5);
    c->selectAll();
    c->deselectAll();
    EXPECT_TRUE(cv->save());
    EXPECT_FALSE(cv->isDirty());
    window->workspace()->closeActiveLeaf();
    QApplication::processEvents();
    EXPECT_EQ(disk("Showcase.canvas"), kShowcase);
    EXPECT_EQ(QFileInfo(root + "/Showcase.canvas").lastModified().toMSecsSinceEpoch(), before);
}

TEST_F(CanvasTest, EditsAreSavedInObsidiansLayout)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->document().setNodeRect(u"b", QRectF(420, 20, 200, 100));
    c->commit();
    EXPECT_TRUE(cv->isDirty());
    EXPECT_TRUE(cv->save());
    const QByteArray out = disk("Plain.canvas");
    const QList<QByteArray> a = kPlain.split('\n'), b = out.split('\n');
    ASSERT_EQ(a.size(), b.size());
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].contains("\"id\":\"b\""))
            EXPECT_EQ(b[i], "\t\t{\"id\":\"b\",\"type\":\"text\",\"text\":\"Beta\",\"x\":420,\"y\":20,\"width\":200,\"height\":100},");
        else
            EXPECT_EQ(a[i], b[i]);
    }
    // Undoing back to the loaded state writes the original bytes.
    c->undo();
    EXPECT_TRUE(cv->save());
    EXPECT_EQ(disk("Plain.canvas"), kPlain);
}

TEST_F(CanvasTest, AutosavesAfterTwoSeconds)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    cv->canvas()->selectOnly("c");
    cv->canvas()->setSelectionColor("3");
    EXPECT_EQ(disk("Plain.canvas"), kPlain);
    EXPECT_TRUE(QTest::qWaitFor([&] { return disk("Plain.canvas") != kPlain; }, 5000));
    EXPECT_TRUE(disk("Plain.canvas").contains("\"id\":\"c\",\"type\":\"text\",\"text\":\"Gamma\",\"x\":0,\"y\":300,\"width\":200,\"height\":100,\"color\":\"3\""));
}

TEST_F(CanvasTest, EmptyNewCanvasGetsObsidiansFormat)
{
    CanvasView* cv = openCanvas("Empty.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    EXPECT_TRUE(c->data().isEmpty());
    const QString id = c->createTextNode(QPointF(0, 0), Anchor::Center, std::nullopt, QString(), false);
    ASSERT_FALSE(id.isEmpty());
    EXPECT_TRUE(cv->save());
    EXPECT_EQ(disk("Empty.canvas"),
              "{\n\t\"nodes\":[\n\t\t{\"id\":\"" + id.toUtf8() + "\",\"x\":-125,\"y\":-30,\"width\":250,\"height\":60,\"type\":\"text\",\"text\":\"\"}\n\t],\n\t\"edges\":[]\n}");
}

TEST_F(CanvasTest, UnreadableCanvasIsShownReadOnlyAndNeverSaved)
{
    CanvasView* cv = openCanvas("Broken.canvas");
    ASSERT_TRUE(cv);
    EXPECT_TRUE(cv->isInvalid());
    EXPECT_TRUE(cv->canvas()->isReadOnly());
    EXPECT_TRUE(cv->canvas()->createTextNode(QPointF(0, 0), Anchor::Center).isEmpty());
    EXPECT_TRUE(cv->save());
    EXPECT_EQ(disk("Broken.canvas"), "{\"nodes\":[");
    EXPECT_FALSE(cv->canvas()->grab().isNull());
}

TEST_F(CanvasTest, ExternalChangesReload)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->selectOnly("a");
    const Viewport before = c->canvasViewport();
    QByteArray changed = kPlain;
    changed.replace("\"text\":\"Alpha\"", "\"text\":\"Alpha 2\"");
    ASSERT_TRUE(writeFile(root + "/Plain.canvas", changed));
    vault->rescan();
    EXPECT_TRUE(QTest::qWaitFor([&] { return c->data().node(u"a") && c->data().node(u"a")->text == "Alpha 2"; }, 5000));
    EXPECT_EQ(c->selection(), QSet<QString>{"a"});
    EXPECT_EQ(c->canvasViewport().zoom, before.zoom);
    EXPECT_FALSE(cv->isDirty());
}

// ---------------------------------------------------------------------------------------------
// Interactions
// ---------------------------------------------------------------------------------------------

TEST_F(CanvasTest, DoubleClickCreatesAndEditsATextCard)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 150, 0);
    dclick(c, QPointF(300, 600));
    QApplication::processEvents();
    ASSERT_EQ(c->data().nodes.size(), 4);
    const Node n = c->data().nodes.last();
    EXPECT_EQ(n.rect(), QRectF(175, 570, 250, 60)); // centered on the double click
    EXPECT_EQ(c->selection(), QSet<QString>{n.id});
    EXPECT_EQ(c->editingId(), n.id);
    EditorView* ed = c->activeEditor();
    ASSERT_TRUE(ed);
    EXPECT_EQ(ed->mode(), EditorView::Mode::LivePreview);
    QTest::keyClicks(ed, "Hello card");
    QApplication::processEvents();
    EXPECT_EQ(c->data().node(n.id)->text, "Hello card");
    QTest::keyClick(ed, Qt::Key_Escape);
    QApplication::processEvents();
    EXPECT_TRUE(c->editingId().isEmpty());
    EXPECT_EQ(c->selection(), QSet<QString>{n.id});
    // The card's rendered content is there once editing ends.
    EXPECT_GT(c->contentFor(n.id)->contentHeight(250), 0);
    // One undo removes the typing, the next the card.
    c->undo();
    EXPECT_EQ(c->data().node(n.id)->text, "");
    c->undo();
    EXPECT_FALSE(c->data().node(n.id));
}

TEST_F(CanvasTest, ClickSelectsBoxSelectsAndShiftToggles)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    click(c, QPointF(100, 50));
    EXPECT_EQ(c->selection(), QSet<QString>{"a"});
    click(c, QPointF(500, 50), Qt::ShiftModifier);
    EXPECT_EQ(c->selection(), (QSet<QString>{"a", "b"}));
    click(c, QPointF(100, 50), Qt::ShiftModifier);
    EXPECT_EQ(c->selection(), QSet<QString>{"b"});
    // Clicking the empty canvas deselects; dragging selects what the box touches.
    click(c, QPointF(300, 220));
    EXPECT_TRUE(c->selection().isEmpty());
    drag(c, QPointF(-50, -50), QPointF(150, 350));
    EXPECT_EQ(c->selection(), (QSet<QString>{"a", "c"}));
    // Clicking an edge selects it, clicking it again edits its label.
    const QPointF mid = edgeGeometry(QRectF(0, 0, 200, 100), Side::Right, QRectF(400, 0, 200, 100), Side::Left).center();
    click(c, mid);
    EXPECT_EQ(c->selection(), QSet<QString>{"ab"});
    click(c, mid);
    ASSERT_TRUE(c->labelEditor());
    QTest::keyClicks(c->labelEditor(), "relates");
    QTest::keyClick(c->labelEditor(), Qt::Key_Return);
    EXPECT_EQ(c->data().edge(u"ab")->label, "relates");
}

TEST_F(CanvasTest, DragMovesWithGridSnapping)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    drag(c, QPointF(100, 350), QPointF(107, 417));
    // The top-left (7, 367) snaps to the grid (0, 360); the dragged card comes to the top.
    EXPECT_EQ(c->data().node(u"c")->rect(), QRectF(0, 360, 200, 100));
    EXPECT_EQ(c->data().nodes.last().id, "c");
    EXPECT_EQ(c->selection(), QSet<QString>{"c"});
    // Alt disables snapping; Shift constrains to an axis.
    drag(c, QPointF(100, 400), QPointF(113, 405), Qt::AltModifier);
    EXPECT_EQ(c->data().node(u"c")->rect(), QRectF(13, 365, 200, 100));
    drag(c, QPointF(100, 400), QPointF(163, 409), Qt::ShiftModifier | Qt::AltModifier);
    EXPECT_EQ(c->data().node(u"c")->rect(), QRectF(76, 365, 200, 100));
    // Snapping to another card's edge wins over the grid.
    c->undo();
    c->undo();
    drag(c, QPointF(100, 400), QPointF(503, 405));
    EXPECT_EQ(c->data().node(u"c")->x, 400); // left edges line up with "b"
    // Escape cancels a drag.
    QWidget* vp = c->viewport();
    send(vp, QEvent::MouseButtonPress, at(c, QPointF(500, 410)), Qt::LeftButton, Qt::LeftButton, {});
    send(vp, QEvent::MouseMove, at(c, QPointF(520, 430)), Qt::NoButton, Qt::LeftButton, {});
    send(vp, QEvent::MouseMove, at(c, QPointF(600, 500)), Qt::NoButton, Qt::LeftButton, {});
    QTest::keyClick(c, Qt::Key_Escape);
    send(vp, QEvent::MouseButtonRelease, at(c, QPointF(600, 500)), Qt::LeftButton, Qt::NoButton, {});
    EXPECT_EQ(c->data().node(u"c")->x, 400);
}

TEST_F(CanvasTest, CtrlDragClones)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    c->setSelection({"a", "b"});
    drag(c, QPointF(100, 50), QPointF(100, 250), Qt::ControlModifier | Qt::AltModifier);
    ASSERT_EQ(c->data().nodes.size(), 5);
    EXPECT_EQ(c->data().node(u"a")->rect(), QRectF(0, 0, 200, 100)); // originals stay
    EXPECT_EQ(c->data().edges.size(), 2);                           // the edge between them is cloned too
    EXPECT_EQ(c->selection().size(), 2);
    for (const QString& id : c->selection()) {
        const Node* n = c->data().node(id);
        ASSERT_TRUE(n);
        EXPECT_EQ(n->y, 200);
    }
}

TEST_F(CanvasTest, ResizeFromTheHandles)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    // Bottom-right corner (hovering shows the card's resizers).
    drag(c, QPointF(200, 100), QPointF(263, 147));
    EXPECT_EQ(c->data().node(u"a")->rect(), QRectF(0, 0, 260, 140));
    // Minimum size.
    drag(c, QPointF(0, 20), QPointF(250, 20), Qt::AltModifier); // away from the side's connection point
    EXPECT_EQ(c->data().node(u"a")->width, 50);
    EXPECT_TRUE(c->canUndo());
}

TEST_F(CanvasTest, ConnectAndReconnectEdges)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    // From the connection point at the bottom of "a" to the top of "c".
    drag(c, QPointF(100, 99), QPointF(104, 305));
    ASSERT_EQ(c->data().edges.size(), 2);
    const Edge& e = c->data().edges.last();
    EXPECT_EQ(e.fromNode, "a");
    EXPECT_EQ(e.fromSide, Side::Bottom);
    EXPECT_EQ(e.toNode, "c");
    EXPECT_EQ(e.toSide, Side::Top);
    EXPECT_EQ(e.toEnd, EdgeEnd::Arrow);
    // Dragging an edge moves its nearer end to another card's side.
    const QPointF nearB = QPointF(380, 50);
    drag(c, nearB, QPointF(205, 350));
    const Edge* ab = c->data().edge(u"ab");
    ASSERT_TRUE(ab);
    EXPECT_EQ(ab->toNode, "c");
    EXPECT_EQ(ab->toSide, Side::Right);
    // Dropped on nothing, the edge is removed (Obsidian).
    const QPointF mid = edgeGeometry(QRectF(0, 0, 200, 100), Side::Right, QRectF(0, 300, 200, 100), Side::Right).pointAt(0.9);
    drag(c, mid, QPointF(900, 900));
    EXPECT_FALSE(c->data().edge(u"ab"));
}

TEST_F(CanvasTest, DeleteUndoRedoAndKeys)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->selectOnly("a");
    QTest::keyClick(c, Qt::Key_Delete);
    EXPECT_FALSE(c->data().node(u"a"));
    EXPECT_TRUE(c->data().edges.isEmpty()); // its edge goes too
    QTest::keyClick(c, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_TRUE(c->data().node(u"a"));
    EXPECT_EQ(c->data().edges.size(), 1);
    QTest::keyClick(c, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_FALSE(c->data().node(u"a"));
    QTest::keyClick(c, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(c, Qt::Key_Y, Qt::ControlModifier);
    EXPECT_FALSE(c->data().node(u"a"));
    c->undo();
    // Select all, nudge with the arrows (grid steps, 5 with Shift).
    QTest::keyClick(c, Qt::Key_A, Qt::ControlModifier);
    EXPECT_EQ(c->selection().size(), 3);
    QTest::keyClick(c, Qt::Key_Right);
    EXPECT_EQ(c->data().node(u"a")->x, 20);
    QTest::keyClick(c, Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(c->data().node(u"b")->y, 100);
    QTest::keyClick(c, Qt::Key_Escape);
    EXPECT_TRUE(c->selection().isEmpty());
}

TEST_F(CanvasTest, CopyPasteWithinAndBetweenCanvases)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setSelection({"a", "b"});
    QTest::keyClick(c, Qt::Key_C, Qt::ControlModifier);
    const QMimeData* md = QGuiApplication::clipboard()->mimeData();
    ASSERT_TRUE(md && md->hasFormat("obsidian/canvas"));
    const std::optional<Json> clip = parseJson(md->data("obsidian/canvas"));
    ASSERT_TRUE(clip);
    EXPECT_EQ(clip->value(u"nodes").size(), 2);
    EXPECT_EQ(clip->value(u"edges").size(), 1);
    // Pasted at the view center (the selection's center moves there).
    c->setViewport(1000, 1000, 0);
    QTest::keyClick(c, Qt::Key_V, Qt::ControlModifier);
    ASSERT_EQ(c->data().nodes.size(), 5);
    EXPECT_EQ(c->selection().size(), 2);
    EXPECT_EQ(itemsBounds(c->data(), c->selection()).center(), QPointF(1000, 1000));
    EXPECT_EQ(c->data().edges.size(), 2);
    // Into another canvas.
    CanvasView* other = openCanvas("Empty.canvas", Workspace::NewLeaf::Tab);
    ASSERT_TRUE(other);
    other->canvas()->paste();
    EXPECT_EQ(other->canvas()->data().nodes.size(), 2);
    EXPECT_EQ(other->canvas()->data().edges.size(), 1);
    // Plain text becomes a card, a URL a link card.
    QGuiApplication::clipboard()->setText("https://example.com/page");
    other->canvas()->paste();
    EXPECT_EQ(other->canvas()->data().nodes.last().type, NodeType::Link);
    QGuiApplication::clipboard()->setText("some *text*");
    other->canvas()->paste();
    EXPECT_EQ(other->canvas()->data().nodes.last().text, "some *text*");
    // Cut removes.
    c->selectOnly("c");
    QTest::keyClick(c, Qt::Key_X, Qt::ControlModifier);
    EXPECT_FALSE(c->data().node(u"c"));
}

TEST_F(CanvasTest, ViewportPanZoomAndLimits)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(0, 0, 0);
    QWidget* vp = c->viewport();
    // Wheel pans (by the delta / scale * 0.93).
    QWheelEvent pan(QPointF(100, 100), vp->mapToGlobal(QPointF(100, 100)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false);
    QApplication::sendEvent(vp, &pan);
    EXPECT_NEAR(c->canvasViewport().y, 93, 1e-9);
    // Ctrl+wheel zooms in around the pointer.
    const QPoint p(200, 150);
    const QPointF under = c->mapToCanvas(p);
    QWheelEvent zoomIn(QPointF(p), vp->mapToGlobal(QPointF(p)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                       Qt::NoScrollPhase, false);
    QApplication::sendEvent(vp, &zoomIn);
    EXPECT_NEAR(c->canvasViewport().zoom, 1.0 / 3, 1e-9);
    EXPECT_NEAR(c->mapToCanvas(p).x(), under.x(), 1e-6);
    EXPECT_NEAR(c->mapToCanvas(p).y(), under.y(), 1e-6);
    // Limits.
    for (int i = 0; i < 30; ++i)
        c->zoomBy(1);
    EXPECT_EQ(c->canvasViewport().zoom, kMaxZoom);
    for (int i = 0; i < 30; ++i)
        c->zoomBy(-1);
    EXPECT_EQ(c->canvasViewport().zoom, kMinZoom);
    // Controls: reset zoom, zoom in/out by 0.5.
    c->controls()->button("reset-zoom")->click();
    EXPECT_EQ(c->canvasViewport().zoom, 0);
    c->controls()->button("zoom-in")->click();
    EXPECT_EQ(c->canvasViewport().zoom, 0.5);
    c->controls()->button("zoom-out")->click();
    c->controls()->button("zoom-out")->click();
    EXPECT_EQ(c->canvasViewport().zoom, -0.5);
    // Shift+1: zoom to fit; Shift+2: zoom to the selection.
    QTest::keyClick(c, Qt::Key_Exclam, Qt::ShiftModifier);
    const QRectF all = allNodesBounds(c->data());
    EXPECT_NEAR(c->canvasViewport().x, all.center().x(), 1e-9);
    c->selectOnly("c");
    QTest::keyClick(c, Qt::Key_At, Qt::ShiftModifier);
    EXPECT_NEAR(c->canvasViewport().x, 100, 1e-9);
    EXPECT_NEAR(c->canvasViewport().y, 350, 1e-9);
    EXPECT_EQ(c->canvasViewport().zoom, 0);
    // Middle-drag pans.
    const Viewport before = c->canvasViewport();
    dragView(c, QPoint(300, 300), QPoint(340, 320), {}, Qt::MiddleButton, 1);
    EXPECT_NEAR(c->canvasViewport().x, before.x - 40, 1e-9);
    EXPECT_NEAR(c->canvasViewport().y, before.y - 20, 1e-9);
    // Space + drag pans too.
    QTest::keyPress(c, Qt::Key_Space);
    dragView(c, QPoint(300, 300), QPoint(310, 300), {}, Qt::LeftButton, 1);
    QTest::keyRelease(c, Qt::Key_Space);
    EXPECT_NEAR(c->canvasViewport().x, before.x - 50, 1e-9);
    EXPECT_EQ(c->data().nodes.size(), 3); // no card was moved or created
}

TEST_F(CanvasTest, ContextMenus)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    auto titles = [](QMenu* m) {
        QStringList out;
        for (QAction* a : m->actions())
            if (!a->isSeparator())
                out << a->text();
        return out;
    };
    QMenu* bg = c->showContextMenu(at(c, QPointF(300, 220)));
    ASSERT_TRUE(bg);
    EXPECT_EQ(titles(bg).mid(0, 5), QStringList({"Add card", "Add note from vault", "Add media from vault", "Add web page", "Create group"}));
    EXPECT_TRUE(titles(bg).contains("Snap to grid"));
    bg->close();
    QMenu* node = c->showContextMenu(at(c, QPointF(100, 50)));
    ASSERT_TRUE(node);
    EXPECT_EQ(c->selection(), QSet<QString>{"a"});
    const QStringList nt = titles(node);
    EXPECT_TRUE(nt.contains("Edit"));
    EXPECT_TRUE(nt.contains("Convert to file..."));
    EXPECT_EQ(nt.last(), "Remove");
    node->close();
    const QPointF mid = edgeGeometry(QRectF(0, 0, 200, 100), Side::Right, QRectF(400, 0, 200, 100), Side::Left).center();
    QMenu* edge = c->showContextMenu(at(c, mid));
    ASSERT_TRUE(edge);
    EXPECT_TRUE(titles(edge).contains("Follow connection"));
    EXPECT_TRUE(titles(edge).contains("Edit label"));
    edge->close();
    // "Add card" from the background menu.
    QMenu* bg2 = c->showContextMenu(at(c, QPointF(900, 900)));
    ASSERT_TRUE(bg2);
    bg2->actions().first()->trigger();
    EXPECT_EQ(c->data().nodes.size(), 4);
    EXPECT_EQ(c->data().nodes.last().rect().topLeft(), QPointF(900, 900));
}

TEST_F(CanvasTest, SelectionMenuColorsAndGroups)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    c->setSelection({"a", "b"});
    QApplication::processEvents();
    canvasui::SelectionMenu* menu = c->selectionMenu();
    ASSERT_TRUE(menu->isVisible());
    QStringList names;
    for (const char* n : {"remove", "color", "zoom-to-selection", "create-group", "align"})
        EXPECT_TRUE(menu->button(QString::fromLatin1(n))) << n;
    // The toolbar sits above the selection.
    EXPECT_LT(menu->geometry().bottom(), at(c, QPointF(0, 0)).y());
    menu->button("color")->click();
    ASSERT_TRUE(menu->isColorPickerOpen());
    c->setSelectionColor("4");
    EXPECT_EQ(c->data().node(u"a")->color, "4");
    EXPECT_EQ(c->data().node(u"b")->color, "4");
    menu->button("create-group")->click();
    ASSERT_FALSE(c->data().nodes.isEmpty());
    const Node g = c->data().nodes.first();
    ASSERT_TRUE(g.isGroup());
    EXPECT_EQ(g.rect(), QRectF(-20, -20, 640, 140)); // the selection + 20
    EXPECT_EQ(g.label, "Untitled group");
    ASSERT_TRUE(c->labelEditor());
    QTest::keyClicks(c->labelEditor(), "Pair");
    QTest::keyClick(c->labelEditor(), Qt::Key_Return);
    EXPECT_EQ(c->data().node(g.id)->label, "Pair");
    // Moving the group moves its contents.
    c->selectOnly(g.id);
    drag(c, QPointF(300, 80), QPointF(300, 180), Qt::AltModifier);
    EXPECT_EQ(c->data().node(u"a")->y, 100);
    EXPECT_EQ(c->data().node(u"b")->y, 100);
    EXPECT_EQ(c->data().node(u"c")->y, 300); // outside the group
    // Edge line direction from the menu.
    c->selectOnly("ab");
    QApplication::processEvents();
    ASSERT_TRUE(menu->button("line-direction"));
    c->setSelectionEdgeEnds(EdgeEnd::Arrow, EdgeEnd::Arrow);
    EXPECT_EQ(c->data().edge(u"ab")->fromEnd, EdgeEnd::Arrow);
}

TEST_F(CanvasTest, ReadOnlyBlocksEditingAndIsRemembered)
{
    CanvasView* cv = openCanvas("Plain.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(300, 200, 0);
    c->setReadOnly(true);
    EXPECT_FALSE(c->cardMenu()->isVisible());
    drag(c, QPointF(100, 50), QPointF(200, 150));
    EXPECT_EQ(c->data().node(u"a")->rect(), QRectF(0, 0, 200, 100));
    dclick(c, QPointF(300, 700));
    EXPECT_EQ(c->data().nodes.size(), 3);
    c->selectOnly("a");
    QTest::keyClick(c, Qt::Key_Delete);
    EXPECT_TRUE(c->data().node(u"a"));
    // Remembered per file (in the clone's data dir, never in the vault).
    window->workspace()->closeActiveLeaf();
    CanvasView* again = openCanvas("Plain.canvas");
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->canvas()->isReadOnly());
    EXPECT_FALSE(QFileInfo::exists(root + "/.obsidian"));
    again->canvas()->setReadOnly(false);
}

TEST_F(CanvasTest, CardContentsAndLinks)
{
    CanvasView* cv = openCanvas("Showcase.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    // Note cards render the note (with its title), images their pixels, canvases a minimap.
    auto* idea = dynamic_cast<canvasui::MarkdownContent*>(c->contentFor("f1"));
    ASSERT_TRUE(idea);
    EXPECT_GT(idea->contentHeight(360), 100);
    EXPECT_TRUE(dynamic_cast<canvasui::ImageContent*>(c->contentFor("i1")));
    EXPECT_GT(c->contentFor("i1")->aspectRatio(), 0);
    EXPECT_TRUE(dynamic_cast<canvasui::MinimapContent*>(c->contentFor("m1")));
    EXPECT_TRUE(dynamic_cast<canvasui::PlaceholderContent*>(c->contentFor("p1")));
    EXPECT_TRUE(dynamic_cast<canvasui::PlaceholderContent*>(c->contentFor("x1")));
    // A subpath card shows only its section.
    auto* details = dynamic_cast<canvasui::MarkdownContent*>(c->contentFor("f2"));
    ASSERT_TRUE(details);
    EXPECT_LT(details->contentHeight(300), idea->contentHeight(300));
    // Clicking a link in the focused text card opens it.
    c->setViewport(160, 130, 0);
    c->selectOnly("t1");
    QString opened;
    QObject::connect(c, &CanvasWidget::linkActivated, c, [&](const QString& t, bool) { opened = t; });
    canvasui::CardContent* t1 = c->contentFor("t1");
    const Node* n = c->data().node(u"t1");
    QPointF linkPos;
    for (double y = 0; y < n->height && linkPos.isNull(); y += 2)
        for (double x = 0; x < n->width; x += 4)
            if (auto l = t1->linkAt(QPointF(x, y), QSizeF(n->width - 2, n->height - 2), 0); l && l->target == "Plan") {
                linkPos = QPointF(x, y);
                break;
            }
    ASSERT_FALSE(linkPos.isNull());
    click(c, n->rect().topLeft() + QPointF(1, 1) + linkPos);
    EXPECT_EQ(opened, "Plan");
    QApplication::processEvents();
    // The view opens it like Obsidian's openLinkText.
    auto* mv = qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view());
    ASSERT_TRUE(mv);
    EXPECT_EQ(mv->file()->path(), "Plan.md");
}

TEST_F(CanvasTest, EditingANoteCardEditsTheNote)
{
    CanvasView* cv = openCanvas("Showcase.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(580, 180, 0);
    c->startEditing("f1");
    EditorView* ed = c->activeEditor();
    ASSERT_TRUE(ed);
    ASSERT_TRUE(ed->document());
    EXPECT_EQ(ed->document()->path(), "Notes/Idea.md");
    ed->setCursorPosition(ed->document()->text().length());
    QTest::keyClicks(ed, "Added.");
    c->stopEditing();
    ASSERT_TRUE(docs->saveAll());
    EXPECT_TRUE(disk("Notes/Idea.md").endsWith("More.\nAdded."));
    EXPECT_FALSE(cv->isDirty()); // the canvas itself did not change
}

TEST_F(CanvasTest, FileDropsFromTheExplorer)
{
    CanvasView* cv = openCanvas("Empty.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->setViewport(0, 0, 0);
    const QPoint center = c->viewport()->rect().center();
    const QPoint global = c->viewport()->mapToGlobal(center);
    EXPECT_TRUE(c->ocFileDrag({"Plan.md"}, global, false));
    EXPECT_TRUE(c->ocFileDrag({"Plan.md"}, global, true));
    ASSERT_EQ(c->data().nodes.size(), 1);
    EXPECT_EQ(c->data().nodes[0].file, "Plan.md");
    EXPECT_EQ(c->data().nodes[0].rect().topLeft(), c->mapToCanvas(center));
    // A folder adds its files in a grid.
    EXPECT_TRUE(c->ocFileDrag({"Notes"}, global, true));
    EXPECT_EQ(c->data().nodes.size(), 2);
    EXPECT_FALSE(c->ocFileDrag({"Plan.md"}, c->viewport()->mapToGlobal(QPoint(-50, -50)), true));
}

TEST_F(CanvasTest, CommandsAndRenames)
{
    // canvas:new-file creates an empty "Untitled.canvas" at the vault root and opens it.
    ASSERT_TRUE(commands->execute("canvas:new-file"));
    TFile* f = vault->file("Untitled.canvas");
    ASSERT_TRUE(f);
    EXPECT_EQ(disk("Untitled.canvas"), "");
    auto* cv = qobject_cast<CanvasView*>(window->workspace()->activeRootLeaf()->view());
    ASSERT_TRUE(cv);
    EXPECT_EQ(cv->file(), f);
    // Jump to group / convert to file are available in a canvas only.
    EXPECT_TRUE(commands->isAvailable("canvas:jump-to-group"));
    EXPECT_FALSE(commands->isAvailable("canvas:convert-to-file"));
    const QString id = cv->canvas()->createTextNode(QPointF(0, 0), Anchor::Center, std::nullopt, "text", false);
    cv->canvas()->selectOnly(id);
    EXPECT_TRUE(commands->isAvailable("canvas:convert-to-file"));
    // Renaming a note updates the cards that show it, in open and closed canvases.
    CanvasView* show = openCanvas("Showcase.canvas");
    ASSERT_TRUE(show);
    ASSERT_TRUE(fileManager->renameFile(vault->file("Notes/Idea.md"), "Notes/Big idea.md"));
    EXPECT_TRUE(QTest::qWaitFor([&] { return show->canvas()->data().node(u"f1")->file == "Notes/Big idea.md"; }, 5000));
    EXPECT_EQ(show->canvas()->data().node(u"f2")->file, "Notes/Big idea.md");
    EXPECT_TRUE(disk("Showcase.canvas").contains("\"file\":\"Notes/Big idea.md\",\"subpath\":\"#Details\""));
    ASSERT_TRUE(fileManager->renameFile(vault->file("Plan.md"), "Plan 2.md"));
    QTest::qWait(700);
    EXPECT_EQ(disk("Mini.canvas"), kMini); // mentions no renamed file: untouched
}

TEST_F(CanvasTest, CanvasLinksInTheLinkGraph)
{
    ASSERT_TRUE(meta->waitUntilClean());
    const QHash<QString, int> links = meta->resolvedLinks().value("Showcase.canvas");
    EXPECT_EQ(links.value("Notes/Idea.md"), 2);
    EXPECT_EQ(links.value("Plan.md"), 1);
    EXPECT_EQ(links.value("attachments/image.png"), 1);
    EXPECT_TRUE(meta->backlinksOf("Plan.md").contains("Showcase.canvas"));
    EXPECT_EQ(meta->unresolvedLinks().value("Showcase.canvas").value("Nowhere"), 1);
}

// ---------------------------------------------------------------------------------------------
// Screenshots
// ---------------------------------------------------------------------------------------------

class CanvasShots : public CanvasTest, public ::testing::WithParamInterface<bool> {};

TEST_P(CanvasShots, EveryNodeTypeAndEdgeStyle)
{
    ThemeManager* tm = themes();
    tm->setMode(GetParam() ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
    tm->apply();
    CanvasView* cv = openCanvas("Showcase.canvas");
    ASSERT_TRUE(cv);
    CanvasWidget* c = cv->canvas();
    c->selectOnly("f2");
    QApplication::processEvents();
    const QString suffix = GetParam() ? "dark" : "light";
    // Images decode asynchronously.
    QTest::qWait(300);
    QApplication::processEvents();
    const QImage full = window->grab().toImage();
    EXPECT_FALSE(full.isNull());
    c->deselectAll();
    c->setViewport(560, 260, 0);
    c->setSelection({"t1", "f1"});
    QTest::qWait(100);
    const QImage zoomed = window->grab().toImage();
    // Zoomed out below the breakpoint: placeholders.
    c->setViewport(300, 200, -2.5);
    const QImage far = c->grab().toImage();
    EXPECT_FALSE(far.isNull());
    const QImage exported = cv->renderImage(-1);
    EXPECT_FALSE(exported.isNull());
    if (const QString dir = qEnvironmentVariable("OC_SHOT_DIR"); !dir.isEmpty()) {
        full.save(dir + "/canvas-" + suffix + ".png");
        zoomed.save(dir + "/canvas-zoom-" + suffix + ".png");
        far.save(dir + "/canvas-far-" + suffix + ".png");
        exported.save(dir + "/canvas-export-" + suffix + ".png");
    }
    tm->setMode(ThemeManager::Mode::Dark);
    tm->apply();
}

INSTANTIATE_TEST_SUITE_P(Themes, CanvasShots, ::testing::Values(true, false), [](const auto& info) { return info.param ? "Dark" : "Light"; });
