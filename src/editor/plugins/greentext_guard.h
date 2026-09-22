#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace oc {

class EditorView;

// Native port of the user's "Greentext Guard" community plugin (.obsidian/plugins/greentext-guard).
// In notes whose file name contains one of `patterns` (case-insensitive substring):
//  - pasting text that has a line starting with ">" wraps it in a code fence (optionally together with the
//    blank-line-delimited block it lands in), so it isn't rendered as a blockquote;
//  - typing ">" at the start of a line inserts "`>`" (or "\>"), unless inside a fence.
// Plus the command "Wrap selection in a code fence".
class GreentextGuard {
public:
    struct Settings {
        QString patterns = QStringLiteral("4chan");
        bool onPaste = true;
        bool expandToBlock = true;
        QString onType = QStringLiteral("inline"); // "inline" | "escape" | "off"

        static Settings fromJson(const QJsonObject& o);
        QJsonObject toJson() const;
    };

    static GreentextGuard& instance();
    void setSettings(const Settings& s) { m_settings = s; }
    const Settings& settings() const { return m_settings; }

    void install();   // registers editor hooks (idempotent)
    void uninstall();
    bool installed() const { return m_inputHook != 0; }

    bool fileMatches(const QString& path) const;
    static QString fenceWrap(const QString& text);
    // Command callback; returns false (and the caller shows a notice) when nothing is selected.
    bool wrapSelectionInFence(EditorView& view);

    bool handlePaste(EditorView& view, const QString& text, bool hasFiles);
    bool handleInput(EditorView& view, qsizetype from, qsizetype to, const QString& text);

private:
    Settings m_settings;
    int m_inputHook = 0;
    int m_pasteHook = 0;
};

} // namespace oc
