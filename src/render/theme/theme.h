#pragma once

#include "render/theme/css_value.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

namespace oc {

struct FontSettings {
    QString interfaceFamily; // empty = default (system UI font)
    QString textFamily;      // empty = default (system UI font)
    QString monospaceFamily; // empty = first installed of Obsidian's monospace chain
    double baseFontSize = 16; // --font-text-size (px)
};

// Immutable snapshot of resolved design tokens (Obsidian CSS custom property names). Values are
// evaluated lazily and cached; use from the GUI thread only.
class Theme {
public:
    enum class Scheme { Light, Dark };

    Theme(Scheme scheme, QHash<QString, QString> vars, FontSettings fonts, quint64 generation);

    Scheme scheme() const { return m_scheme; }
    bool isDark() const { return m_scheme == Scheme::Dark; }
    quint64 generation() const { return m_generation; }

    bool has(const QString& name) const { return m_vars.contains(name); }
    QString raw(const QString& name) const { return m_vars.value(name); }
    CssValue value(const QString& name) const;

    QColor color(const QString& name, const QColor& fallback = QColor()) const;
    // Lengths: em resolves against emPx (default: --font-text-size), rem against 16px.
    double px(const QString& name, double emPx = -1, double fallback = 0) const;
    double number(const QString& name, double fallback = 0) const;
    CssLength length(const QString& name) const;

    double textSize() const { return m_fonts.baseFontSize; }
    const FontSettings& fontSettings() const { return m_fonts; }

    QString interfaceFamily() const { return m_interfaceFamily; }
    QString textFamily() const { return m_textFamily; }
    QString monospaceFamily() const { return m_monoFamily; }

    // Pixel-sized fonts (Obsidian sizes everything in CSS px).
    QFont interfaceFont(double px = 13) const;
    QFont textFont(double px = -1) const;
    QFont monospaceFont(double px = -1) const;

    // Resolve a family chain like Obsidian's --font-default to the first installed family.
    static QString firstInstalled(const QStringList& chain, const QString& fallback);
    static QString defaultSansFamily();
    static QString defaultMonospaceFamily();

private:
    Scheme m_scheme;
    QHash<QString, QString> m_vars;
    FontSettings m_fonts;
    quint64 m_generation;
    QString m_interfaceFamily, m_textFamily, m_monoFamily;
    CssEvaluator m_eval;
    mutable QHash<QString, CssValue> m_cache;
};

using ThemePtr = std::shared_ptr<const Theme>;

// Owns token layers (defaults + user overrides), builds Theme snapshots, and applies them to the
// application (QPalette + generated QSS). Emits themeChanged() after every rebuild.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    enum class Mode { System, Light, Dark };

    explicit ThemeManager(QObject* parent = nullptr);
    ~ThemeManager() override;

    static ThemeManager* instance();

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    // Obsidian stores appearance.json "theme": "obsidian" (dark) | "moonstone" (light) | "system".
    static Mode modeFromObsidianName(const QString& name);
    static QString obsidianNameForMode(Mode mode);

    QColor accent() const { return m_accent; }
    void setAccent(const QColor& accent); // invalid = Obsidian default hsl(258, 88%, 66%)

    const FontSettings& fonts() const { return m_fonts; }
    void setFonts(const FontSettings& fonts);

    // CSS custom-property overrides (e.g. imported from a theme/snippet file), per selector layer.
    void setOverrides(const QHash<QString, QString>& body, const QHash<QString, QString>& light, const QHash<QString, QString>& dark);

    ThemePtr theme() const { return m_theme; }
    Theme::Scheme effectiveScheme() const;
    // A theme snapshot for a specific scheme without applying it (e.g. light colours for PDF export).
    ThemePtr themeFor(Theme::Scheme scheme) const;

    // Rebuild snapshot and restyle the application.
    void apply();

    QString generatedStyleSheet() const { return m_qss; }

signals:
    void themeChanged();

private:
    void loadDefaults();
    QString buildStyleSheet(const Theme& t) const;

    Mode m_mode = Mode::System;
    QColor m_accent;
    FontSettings m_fonts;
    QHash<QString, QString> m_body, m_light, m_dark;
    QHash<QString, QString> m_ovBody, m_ovLight, m_ovDark;
    ThemePtr m_theme;
    quint64 m_generation = 0;
    QString m_qss;
};

// Current theme snapshot (never null once a ThemeManager exists).
ThemePtr currentTheme();

} // namespace oc
