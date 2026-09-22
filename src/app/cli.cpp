#include "app/cli.h"

#include <QCommandLineParser>

namespace oc {

CliOptions parseCli(const QStringList& args)
{
    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Obsidian C++ — a native Markdown knowledge base"));
    p.addHelpOption();
    p.addVersionOption();
    const QCommandLineOption vault(QStringLiteral("vault"), QStringLiteral("Open this folder as a vault."), QStringLiteral("dir"));
    const QCommandLineOption open(QStringLiteral("open"), QStringLiteral("Open a vault-relative file (path[:line[:col]])."), QStringLiteral("file"));
    const QCommandLineOption mode(QStringLiteral("mode"), QStringLiteral("Markdown view mode: live, source or reading."), QStringLiteral("mode"));
    const QCommandLineOption theme(QStringLiteral("theme"), QStringLiteral("Color scheme for this run: dark, light or system."), QStringLiteral("scheme"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Window size WxH."), QStringLiteral("WxH"));
    const QCommandLineOption shot(QStringLiteral("screenshot"), QStringLiteral("Save a PNG of the window once idle, then quit."), QStringLiteral("png"));
    const QCommandLineOption delay(QStringLiteral("delay"), QStringLiteral("Extra delay before the screenshot (ms)."), QStringLiteral("ms"));
    const QCommandLineOption script(QStringLiteral("script"), QStringLiteral("Run a scenario script."), QStringLiteral("file"));
    const QCommandLineOption collapse(QStringLiteral("collapse"), QStringLiteral("Sidebars to collapse: left, right, both or none."), QStringLiteral("which"));
    const QCommandLineOption det(QStringLiteral("deterministic"), QStringLiteral("No animations or caret blink; fixed fonts."));
    const QCommandLineOption picker(QStringLiteral("picker"), QStringLiteral("Show the vault picker."));
    const QCommandLineOption noReg(QStringLiteral("no-register"), QStringLiteral("Don't add the vault to the vault list."));
    const QCommandLineOption native(QStringLiteral("native-frame"), QStringLiteral("Use the native window frame."));
    const QCommandLineOption frameless(QStringLiteral("frameless"), QStringLiteral("Use the Obsidian-style frameless window."));
    const QCommandLineOption accent(QStringLiteral("accent"), QStringLiteral("Accent color for this run."), QStringLiteral("#rrggbb"));
    const QCommandLineOption edit(QStringLiteral("edit"), QStringLiteral("Developer harness: edit a single file in a bare editor window."), QStringLiteral("file"));
    p.addOptions({vault, open, mode, theme, size, shot, delay, script, collapse, det, picker, noReg, native, frameless, accent, edit});
    p.addPositionalArgument(QStringLiteral("vault"), QStringLiteral("Vault folder (same as --vault)."), QStringLiteral("[vault]"));
    p.process(args);

    CliOptions o;
    o.vault = p.value(vault);
    if (o.vault.isEmpty() && !p.positionalArguments().isEmpty())
        o.vault = p.positionalArguments().first();
    o.open = p.value(open);
    o.mode = p.value(mode);
    o.theme = p.value(theme);
    if (p.isSet(size)) {
        const QStringList wh = p.value(size).split(QLatin1Char('x'));
        if (wh.size() == 2)
            o.size = QSize(wh[0].toInt(), wh[1].toInt());
    }
    o.screenshot = p.value(shot);
    o.delayMs = p.value(delay).toInt();
    o.script = p.value(script);
    o.collapse = p.value(collapse);
    o.deterministic = p.isSet(det);
    o.picker = p.isSet(picker);
    o.noRegister = p.isSet(noReg);
    if (p.isSet(native))
        o.frame = 0;
    if (p.isSet(frameless))
        o.frame = 1;
    o.accent = p.value(accent);
    o.edit = p.value(edit);
    return o;
}

} // namespace oc
