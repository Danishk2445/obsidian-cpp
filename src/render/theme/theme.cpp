#include "render/theme/theme.h"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QRegularExpression>
#include <QStyleHints>

#include <cmath>

namespace oc {

namespace {
ThemeManager* g_instance = nullptr;

QString cssColor(const QColor& c)
{
    if (!c.isValid())
        return QStringLiteral("transparent");
    if (c.alpha() == 255)
        return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(QString::number(c.alphaF(), 'f', 3));
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------------------------
Theme::Theme(Scheme scheme, QHash<QString, QString> vars, FontSettings fonts, quint64 generation)
    : m_scheme(scheme)
    , m_vars(std::move(vars))
    , m_fonts(std::move(fonts))
    , m_generation(generation)
    , m_eval([this](const QString& name, bool* found) {
        auto it = m_vars.constFind(name);
        *found = it != m_vars.cend();
        return *found ? *it : QString();
    })
{
    m_interfaceFamily = m_fonts.interfaceFamily.isEmpty() ? defaultSansFamily() : m_fonts.interfaceFamily;
    m_textFamily = m_fonts.textFamily.isEmpty() ? defaultSansFamily() : m_fonts.textFamily;
    m_monoFamily = m_fonts.monospaceFamily.isEmpty() ? defaultMonospaceFamily() : m_fonts.monospaceFamily;
}

CssValue Theme::value(const QString& name) const
{
    auto it = m_cache.constFind(name);
    if (it != m_cache.cend())
        return *it;
    CssValue v;
    auto rit = m_vars.constFind(name);
    if (rit != m_vars.cend())
        v = m_eval.evaluate(*rit);
    m_cache.insert(name, v);
    return v;
}

QColor Theme::color(const QString& name, const QColor& fallback) const { return value(name).toColor(fallback); }

double Theme::px(const QString& name, double emPx, double fallback) const
{
    if (emPx < 0)
        emPx = m_fonts.baseFontSize;
    return value(name).toPx(emPx, 16.0, fallback);
}

double Theme::number(const QString& name, double fallback) const
{
    const CssValue v = value(name);
    return v.isValid() ? v.toNumber(fallback) : fallback;
}

CssLength Theme::length(const QString& name) const
{
    const CssValue v = value(name);
    if (v.kind == CssValue::Length)
        return v.len;
    if (v.kind == CssValue::Number) {
        CssLength l;
        l.px = v.num;
        return l;
    }
    return {};
}

static QFont pixelFont(const QString& family, double px)
{
    QFont f(family);
    f.setPixelSize(std::max(1, int(std::lround(px))));
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

QFont Theme::interfaceFont(double px) const { return pixelFont(m_interfaceFamily, px); }
QFont Theme::textFont(double px) const { return pixelFont(m_textFamily, px < 0 ? m_fonts.baseFontSize : px); }
QFont Theme::monospaceFont(double px) const { return pixelFont(m_monoFamily, px < 0 ? m_fonts.baseFontSize * 0.875 : px); }

QString Theme::firstInstalled(const QStringList& chain, const QString& fallback)
{
    const QStringList families = QFontDatabase::families();
    for (const QString& want : chain) {
        for (const QString& have : families) {
            if (have.compare(want, Qt::CaseInsensitive) == 0)
                return have;
        }
    }
    return fallback;
}

QString Theme::defaultSansFamily()
{
    static const QString family = [] {
        const QString platform = QGuiApplication::platformName();
        // On a real desktop use the system UI font (what Obsidian's `system-ui` resolves to).
        if (platform != u"offscreen" && platform != u"minimal") {
            const QString sys = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
            if (!sys.isEmpty())
                return sys;
        }
        return firstInstalled({QStringLiteral("Inter"), QStringLiteral("Inter Variable"), QStringLiteral("Roboto"),
                               QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans"), QStringLiteral("Liberation Sans")},
                              QStringLiteral("Sans Serif"));
    }();
    return family;
}

QString Theme::defaultMonospaceFamily()
{
    static const QString family = [] {
        // Obsidian's --font-monospace-default chain (ui-monospace is not supported by Chromium).
        return firstInstalled({QStringLiteral("SFMono-Regular"), QStringLiteral("Cascadia Mono"), QStringLiteral("Roboto Mono"),
                               QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono"), QStringLiteral("Menlo"),
                               QStringLiteral("Monaco"), QStringLiteral("Consolas"), QStringLiteral("Source Code Pro")},
                              QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    }();
    return family;
}

// ---------------------------------------------------------------------------------------------
// ThemeManager
// ---------------------------------------------------------------------------------------------
ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent)
{
    g_instance = this;
    loadDefaults();
    if (auto* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
            if (m_mode == Mode::System)
                apply();
        });
    }
}

ThemeManager::~ThemeManager()
{
    if (g_instance == this)
        g_instance = nullptr;
}

ThemeManager* ThemeManager::instance() { return g_instance; }

ThemePtr currentTheme()
{
    static ThemePtr fallback;
    if (g_instance && g_instance->theme())
        return g_instance->theme();
    if (!fallback)
        fallback = std::make_shared<Theme>(Theme::Scheme::Dark, QHash<QString, QString>{}, FontSettings{}, 0);
    return fallback;
}

void ThemeManager::loadDefaults()
{
    QFile f(QStringLiteral(":/oc/themes/obsidian-default.json"));
    if (!f.open(QIODevice::ReadOnly))
        qWarning("oc: missing theme tokens resource");
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    auto load = [&](const char* key, QHash<QString, QString>& into) {
        const QJsonObject o = root.value(QLatin1String(key)).toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            into.insert(it.key(), it.value().toString());
    };
    load("body", m_body);
    load("theme-light", m_light);
    load("theme-dark", m_dark);
}

ThemeManager::Mode ThemeManager::modeFromObsidianName(const QString& name)
{
    if (name == u"obsidian" || name == u"dark")
        return Mode::Dark;
    if (name == u"moonstone" || name == u"light")
        return Mode::Light;
    return Mode::System;
}

QString ThemeManager::obsidianNameForMode(Mode mode)
{
    switch (mode) {
    case Mode::Dark:
        return QStringLiteral("obsidian");
    case Mode::Light:
        return QStringLiteral("moonstone");
    default:
        return QStringLiteral("system");
    }
}

void ThemeManager::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    apply();
}

void ThemeManager::setAccent(const QColor& accent)
{
    m_accent = accent;
    apply();
}

void ThemeManager::setFonts(const FontSettings& fonts)
{
    m_fonts = fonts;
    apply();
}

void ThemeManager::setOverrides(const QHash<QString, QString>& body, const QHash<QString, QString>& light, const QHash<QString, QString>& dark)
{
    m_ovBody = body;
    m_ovLight = light;
    m_ovDark = dark;
    apply();
}

Theme::Scheme ThemeManager::effectiveScheme() const
{
    switch (m_mode) {
    case Mode::Dark:
        return Theme::Scheme::Dark;
    case Mode::Light:
        return Theme::Scheme::Light;
    case Mode::System:
        break;
    }
    if (auto* hints = QGuiApplication::styleHints()) {
        const Qt::ColorScheme cs = hints->colorScheme();
        if (cs == Qt::ColorScheme::Light)
            return Theme::Scheme::Light;
        if (cs == Qt::ColorScheme::Dark)
            return Theme::Scheme::Dark;
    }
    return Theme::Scheme::Dark; // Obsidian's default body class is theme-dark
}

void ThemeManager::apply()
{
    const Theme::Scheme scheme = effectiveScheme();
    QHash<QString, QString> vars = m_body;
    auto merge = [&](const QHash<QString, QString>& layer) {
        for (auto it = layer.cbegin(); it != layer.cend(); ++it)
            vars.insert(it.key(), it.value());
    };
    merge(scheme == Theme::Scheme::Dark ? m_dark : m_light);
    merge(m_ovBody);
    merge(scheme == Theme::Scheme::Dark ? m_ovDark : m_ovLight);
    if (m_accent.isValid()) {
        // Obsidian derives --accent-h/s/l from the picked accent color.
        float h = 0, s = 0, l = 0;
        m_accent.getHslF(&h, &s, &l);
        vars.insert(QStringLiteral("--accent-h"), QString::number(std::max(0.0f, h) * 360.0f, 'f', 1));
        vars.insert(QStringLiteral("--accent-s"), QString::number(s * 100.0f, 'f', 1) + QLatin1Char('%'));
        vars.insert(QStringLiteral("--accent-l"), QString::number(l * 100.0f, 'f', 1) + QLatin1Char('%'));
    }
    vars.insert(QStringLiteral("--font-text-size"), QString::number(m_fonts.baseFontSize) + QStringLiteral("px"));

    auto theme = std::make_shared<Theme>(scheme, std::move(vars), m_fonts, ++m_generation);
    m_theme = theme;

    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QPalette pal;
        const auto c = [&](const char* n) { return theme->color(QLatin1String(n)); };
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            const bool dis = group == QPalette::Disabled;
            const QColor text = dis ? c("--text-faint") : c("--text-normal");
            pal.setColor(group, QPalette::Window, c("--background-primary"));
            pal.setColor(group, QPalette::WindowText, text);
            pal.setColor(group, QPalette::Base, c("--background-primary"));
            pal.setColor(group, QPalette::AlternateBase, c("--background-primary-alt"));
            pal.setColor(group, QPalette::Text, text);
            pal.setColor(group, QPalette::Button, c("--interactive-normal"));
            pal.setColor(group, QPalette::ButtonText, text);
            pal.setColor(group, QPalette::BrightText, c("--text-on-accent"));
            pal.setColor(group, QPalette::Highlight, c("--interactive-accent"));
            pal.setColor(group, QPalette::HighlightedText, c("--text-on-accent"));
            pal.setColor(group, QPalette::ToolTipBase, c("--background-secondary"));
            pal.setColor(group, QPalette::ToolTipText, c("--text-normal"));
            pal.setColor(group, QPalette::PlaceholderText, c("--text-faint"));
            pal.setColor(group, QPalette::Link, c("--text-accent"));
            pal.setColor(group, QPalette::LinkVisited, c("--text-accent"));
            pal.setColor(group, QPalette::Light, c("--background-modifier-border-hover"));
            pal.setColor(group, QPalette::Midlight, c("--background-modifier-border"));
            pal.setColor(group, QPalette::Mid, c("--background-modifier-border"));
            pal.setColor(group, QPalette::Dark, c("--background-secondary"));
            pal.setColor(group, QPalette::Shadow, QColor(0, 0, 0, 90));
            pal.setColor(group, QPalette::Accent, c("--interactive-accent"));
        }
        app->setPalette(pal);
        app->setFont(theme->interfaceFont(theme->px(QStringLiteral("--font-ui-small"), -1, 13)));
        m_qss = buildStyleSheet(*theme);
        app->setStyleSheet(m_qss);
    }
    emit themeChanged();
}

QString ThemeManager::buildStyleSheet(const Theme& t) const
{
    QFile f(QStringLiteral(":/oc/qss/app.qss"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QString tpl = QString::fromUtf8(f.readAll());
    // ${--var} -> color or px ; ${px:--var} -> integer px ; ${font:interface|text|mono}
    static const QRegularExpression re(QStringLiteral(R"(\$\{(?:(px|font):)?([^}]+)\})"));
    QString out;
    out.reserve(tpl.size() + 1024);
    qsizetype last = 0;
    auto it = re.globalMatch(tpl);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(tpl).mid(last, m.capturedStart() - last);
        const QString kind = m.captured(1);
        const QString name = m.captured(2).trimmed();
        if (kind == u"px") {
            out += QString::number(int(std::lround(t.px(name)))) + QStringLiteral("px");
        } else if (kind == u"font") {
            out += QLatin1Char('"') + (name == u"mono" ? t.monospaceFamily() : name == u"text" ? t.textFamily() : t.interfaceFamily()) + QLatin1Char('"');
        } else {
            const CssValue v = t.value(name);
            if (v.kind == CssValue::Color)
                out += cssColor(v.color);
            else if (v.kind == CssValue::Length)
                out += QString::number(int(std::lround(v.len.resolve(t.textSize())))) + QStringLiteral("px");
            else
                out += cssColor(v.toColor(QColor(255, 0, 255)));
        }
        last = m.capturedEnd();
    }
    out += QStringView(tpl).mid(last);
    return out;
}

} // namespace oc
