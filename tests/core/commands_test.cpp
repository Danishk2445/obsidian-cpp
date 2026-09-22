#include "core/commands/command_registry.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;

TEST(Hotkey, JsonRoundTripAndParse)
{
    const Hotkey h = Hotkey::parse("Mod+Shift+B");
    EXPECT_TRUE(h.mod);
    EXPECT_TRUE(h.shift);
    EXPECT_EQ(h.key, "B");
    const QJsonObject j = h.toJson();
    EXPECT_EQ(QJsonDocument(j).toJson(QJsonDocument::Compact), QByteArray(R"({"key":"B","modifiers":["Mod","Shift"]})"));
    EXPECT_EQ(Hotkey::fromJson(j), h);
    EXPECT_EQ(Hotkey::parse("Mod+Alt+ArrowLeft").toString(), "Ctrl + Alt + ←");
    EXPECT_EQ(Hotkey::parse("F2").key, "F2");
    EXPECT_EQ(Hotkey::parse("Mod+,").key, ",");
    const Hotkey legacy = Hotkey::fromJson(QJsonObject{{"modifiers", QJsonArray{"Ctrl"}}, {"key", "enter"}});
    EXPECT_TRUE(legacy.mod);
    EXPECT_EQ(legacy.key, "Enter");
}

TEST(Hotkey, FromKeyEvent)
{
    QKeyEvent e(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier, "B");
    EXPECT_EQ(Hotkey::fromEvent(&e), Hotkey::parse("Mod+Shift+B"));
    QKeyEvent f(QEvent::KeyPress, Qt::Key_Question, Qt::ControlModifier | Qt::ShiftModifier, "?");
    EXPECT_EQ(Hotkey::fromEvent(&f).key, "/");
    QKeyEvent g(QEvent::KeyPress, Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(Hotkey::fromEvent(&g), Hotkey::parse("Mod+Alt+ArrowLeft"));
}

TEST(CommandRegistry, AvailabilityHotkeysAndCustomOverrides)
{
    CommandRegistry reg;
    int ran = 0, editorRan = 0;
    reg.add({"app:test", "Test command", "zap", {Hotkey::parse("Mod+J")}, false, [&] { ++ran; }, {}, {}, {}});
    Command ed;
    ed.id = "editor:thing";
    ed.name = "Editor thing";
    ed.defaultHotkeys = {Hotkey::parse("Mod+J")};
    ed.editorCallback = [&](EditorContext&) { ++editorRan; };
    reg.add(ed);
    EXPECT_FALSE(reg.isAvailable("editor:thing")); // no editor
    EXPECT_EQ(reg.commandsFor(Hotkey::parse("Mod+J")), (QStringList{"app:test", "editor:thing"}));
    EXPECT_TRUE(reg.execute("app:test"));
    EXPECT_EQ(ran, 1);
    QObject fakeEditor;
    reg.setEditorProvider([&] { return EditorContext{&fakeEditor, nullptr}; });
    EXPECT_TRUE(reg.execute("editor:thing"));
    EXPECT_EQ(editorRan, 1);
    // custom hotkeys (Obsidian hotkeys.json shape)
    reg.loadHotkeys(QJsonDocument::fromJson(R"({"app:test":[{"modifiers":["Mod","Shift"],"key":"K"}],"editor:thing":[]})").object());
    EXPECT_TRUE(reg.commandsFor(Hotkey::parse("Mod+J")).isEmpty());
    EXPECT_EQ(reg.commandsFor(Hotkey::parse("Mod+Shift+K")), QStringList{"app:test"});
    const QJsonObject saved = reg.saveHotkeys();
    EXPECT_TRUE(saved.value("editor:thing").toArray().isEmpty());
    EXPECT_EQ(reg.recent().first(), "editor:thing");
}

TEST(HotkeyDispatcher, RoutesKeysButLeavesTextInputsAlone)
{
    CommandRegistry reg;
    int palette = 0, bold = 0;
    reg.add({"command-palette:open", "Open command palette", "terminal", {Hotkey::parse("Mod+P")}, false, [&] { ++palette; }, {}, {}, {}});
    reg.add({"app:select-all-test", "Select all test", "", {Hotkey::parse("Mod+A")}, false, [&] { ++bold; }, {}, {}, {}});
    HotkeyDispatcher disp(&reg);
    QWidget window;
    auto* edit = new QLineEdit(&window);
    window.show();
    edit->setFocus();
    disp.setWindow(&window);
    QTest::keyClick(edit, Qt::Key_P, Qt::ControlModifier);
    EXPECT_EQ(palette, 1);
    QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier); // text input keeps Ctrl+A
    EXPECT_EQ(bold, 0);
    // overlays that capture keys are skipped
    window.setProperty("ocCapturesKeys", true);
    QTest::keyClick(edit, Qt::Key_P, Qt::ControlModifier);
    EXPECT_EQ(palette, 1);
}
