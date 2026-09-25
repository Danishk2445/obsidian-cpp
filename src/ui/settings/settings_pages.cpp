#include "ui/settings/settings_pages.h"

#include "core/commands/command_registry.h"
#include "core/config/clone_paths.h"
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/config/vault_registry.h"
#include "core/fuzzy/fuzzy.h"
#include "core/metadata/metadata_cache.h"
#include "core/recovery/file_recovery.h"
#include "core/time/moment_format.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/plugins/greentext_guard.h"
#include "editor/spell/spell_checker.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/plugins/file_recovery_ui.h"
#include "ui/settings/hotkeys_tab.h"
#include "ui/settings/path_suggest.h"
#include "ui/settings/setting_item.h"
#include "ui/settings/settings_modal.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/widgets/color_swatch.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/workspace/workspace.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

#include <Sonnet/Speller>

namespace oc {

using namespace Qt::StringLiterals;

// ---------------------------------------------------------------------------------------------
// Core plugins, CSS snippets, fonts
// ---------------------------------------------------------------------------------------------

const QList<CorePluginInfo>& corePluginInfos()
{
    static const QList<CorePluginInfo> list = [] {
        auto t = [](const char* s) { return QCoreApplication::translate("oc::Settings", s); };
        return QList<CorePluginInfo>{
            {u"audio-recorder"_s, t("Audio recorder"), t("Record audio notes and save them as attachments."), false},
            {u"backlink"_s, t("Backlinks"),
             t("Show links from other files to the current file. Backlinks can be shown in a separate view or at the bottom of the note."), true},
            {u"bases"_s, t("Bases"), t("Create custom views that let you edit, sort, and filter files using their properties."), true},
            {u"bookmarks"_s, t("Bookmarks"), t("Save shortcuts to files, searches, headings, and graphs."), true},
            {u"canvas"_s, t("Canvas"), t("Arrange and connect notes on an infinite canvas."), true},
            {u"command-palette"_s, t("Command palette"), t("Use Cmd/Ctrl+P and begin typing to invoke a command."), true},
            {u"daily-notes"_s, t("Daily notes"), t("Create or open today's daily note."), true},
            {u"editor-status"_s, t("Show editing mode in status bar"), t("Show the editing mode toggle in the status bar."), true},
            {u"file-explorer"_s, t("Files"), t("Browse the files and folders in your vault."), true},
            {u"file-recovery"_s, t("File recovery"),
             t("Restore recent snapshots to recover from accidental data loss. Snapshots are only saved for Markdown files."), true},
            {u"footnotes"_s, t("Footnotes view"), t("Show a list of footnotes from the current note."), false},
            {u"global-search"_s, t("Search"), t("Search for a keyword in all the notes."), true},
            {u"graph"_s, t("Graph view"), t("Visualize the relationships between your notes."), true},
            {u"markdown-importer"_s, t("Format converter"), t("Convert Markdown from other apps to Obsidian format."), false},
            {u"note-composer"_s, t("Note composer"), t("Merge two notes or split one into two."), true},
            {u"outgoing-link"_s, t("Outgoing links"), t("Show outgoing links and detect unlinked mentions of other notes in the current note."), true},
            {u"outline"_s, t("Outline"), t("Show the table of contents for the current note."), true},
            {u"page-preview"_s, t("Page preview"), t("Hover an internal link to preview its content. In editor mode, press Ctrl/Cmd while hovering."), true},
            {u"properties"_s, t("Properties view"), t("Show the metadata for your files in the sidebar."), true},
            {u"publish"_s, t("Publish"), t("Host your notes online as a website, wiki or documentation."), false},
            {u"random-note"_s, t("Random note"), t("Open a random note to rediscover or review."), false},
            {u"slash-command"_s, t("Slash commands"), t("Trigger commands in the editor by using the forward slash key."), false},
            {u"slides"_s, t("Slides"), t("Create a presentation by using “---” to separate slides."), false},
            {u"switcher"_s, t("Quick switcher"), t("Jump to other files with your keyboard."), true},
            {u"sync"_s, t("Sync"), t("Synchronize your files through Obsidian Sync."), true},
            {u"tag-pane"_s, t("Tags view"), t("Show a list of all tags and their number of occurrences."), true},
            {u"templates"_s, t("Templates"), t("Insert template content from a folder of template files."), true},
            {u"webviewer"_s, t("Web viewer"), t("Open external links to web pages inside Obsidian."), false},
            {u"word-count"_s, t("Word count"), t("Show word count in the status bar."), true},
            {u"workspaces"_s, t("Workspaces"), t("Save and load workspace layouts."), false},
            {u"zk-prefixer"_s, t("Unique note creator"),
             t("Create notes with unique timestamp prefixes, for workflows like zettelkasten or slip box."), false},
        };
    }();
    return list;
}

namespace {
QStringList parseFontList(const QString& value)
{
    QStringList out;
    for (QString part : value.split(QLatin1Char(','))) {
        part = part.trimmed();
        if (part.size() >= 2 && (part.startsWith(QLatin1Char('"')) || part.startsWith(QLatin1Char('\''))) && part.endsWith(part[0]))
            part = part.mid(1, part.size() - 2).trimmed();
        if (!part.isEmpty() && !out.contains(part))
            out << part;
    }
    return out;
}

bool fontInstalled(const QString& family)
{
    static const QStringList families = QFontDatabase::families();
    for (const QString& f : families)
        if (f.compare(family, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}
} // namespace

QString resolveFontList(const QString& list)
{
    for (const QString& f : parseFontList(list))
        if (fontInstalled(f))
            return Theme::firstInstalled({f}, f);
    return {};
}

namespace css_snippets {
QString folder() { return ClonePaths::configRoot() + u"/snippets"_s; }

QStringList list()
{
    QStringList out;
    const QDir dir(folder());
    for (const QFileInfo& fi : dir.entryInfoList({u"*.css"_s}, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase))
        out << fi.completeBaseName();
    return out;
}

QStringList enabled(const ConfigStore* config)
{
    QStringList out;
    if (!config)
        return out;
    for (const QJsonValue& v : config->value(u"appearance"_s, u"enabledCssSnippets"_s).toArray())
        if (v.isString())
            out << v.toString();
    return out;
}

void setEnabled(ConfigStore* config, const QString& name, bool on)
{
    if (!config)
        return;
    QStringList names = enabled(config);
    names.removeAll(name);
    if (on)
        names << name;
    config->setValue(u"appearance"_s, u"enabledCssSnippets"_s, QJsonArray::fromStringList(names));
}

Overrides parse(const QString& cssIn)
{
    Overrides out;
    // Strip comments, then walk top-level rules (nested @media blocks are skipped).
    QString css = cssIn;
    static const QRegularExpression comments(u"/\\*.*?\\*/"_s, QRegularExpression::DotMatchesEverythingOption);
    css.remove(comments);
    static const QRegularExpression decl(u"(--[A-Za-z0-9_-]+)\\s*:\\s*([^;]+?)\\s*(?:!important\\s*)?(?:;|$)"_s);
    qsizetype i = 0;
    while (i < css.size()) {
        const qsizetype open = css.indexOf(QLatin1Char('{'), i);
        if (open < 0)
            break;
        const QString selector = css.mid(i, open - i).trimmed();
        // matching close brace
        int depth = 1;
        qsizetype j = open + 1;
        for (; j < css.size() && depth > 0; ++j) {
            if (css[j] == QLatin1Char('{'))
                ++depth;
            else if (css[j] == QLatin1Char('}'))
                --depth;
        }
        const QString body = css.mid(open + 1, j - open - 2);
        i = j;
        if (selector.startsWith(QLatin1Char('@')))
            continue;
        for (const QString& selRaw : selector.split(QLatin1Char(','))) {
            const QString sel = selRaw.trimmed();
            QHash<QString, QString>* target = nullptr;
            if (sel == u"body" || sel == u":root" || sel == u"html")
                target = &out.body;
            else if (sel == u".theme-light" || sel == u"body.theme-light" || sel == u".theme-light body")
                target = &out.light;
            else if (sel == u".theme-dark" || sel == u"body.theme-dark" || sel == u".theme-dark body")
                target = &out.dark;
            if (!target)
                continue;
            auto it = decl.globalMatch(body);
            while (it.hasNext()) {
                const auto m = it.next();
                target->insert(m.captured(1), m.captured(2).trimmed());
            }
        }
    }
    return out;
}

Overrides collect(const ConfigStore* config)
{
    Overrides out;
    const QStringList available = list();
    for (const QString& name : enabled(config)) {
        if (!available.contains(name))
            continue;
        QFile f(folder() + QLatin1Char('/') + name + u".css"_s);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const Overrides o = parse(QString::fromUtf8(f.readAll()));
        for (auto it = o.body.cbegin(); it != o.body.cend(); ++it)
            out.body.insert(it.key(), it.value());
        for (auto it = o.light.cbegin(); it != o.light.cend(); ++it)
            out.light.insert(it.key(), it.value());
        for (auto it = o.dark.cbegin(); it != o.dark.cend(); ++it)
            out.dark.insert(it.key(), it.value());
    }
    return out;
}
} // namespace css_snippets

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

namespace {
QString tr(const char* s) { return QCoreApplication::translate("oc::Settings", s); }

using Options = QList<QPair<QString, QString>>;

SettingsController* controllerOf(const SettingsTab& t) { return t.modal() ? t.modal()->controller() : nullptr; }

QString link(const QString& href, const QString& text) { return u"<a href=\""_s + href.toHtmlEscaped() + u"\">"_s + text.toHtmlEscaped() + u"</a>"_s; }

// A moment.js format input with the live sample in the description (Obsidian's MomentFormatComponent:
// the placeholder is the default format, the sample uses it while the input is empty).
void momentFormatRow(SettingItem& row, const QString& descPrefixHtml, const QString& value, const QString& defaultFormat,
                     std::function<void(const QString&)> onChange)
{
    const QString syntax = tr("For more syntax, refer to ") + link(u"https://momentjs.com/docs/#/displaying/format/"_s, tr("format reference"));
    auto sampleHtml = [descPrefixHtml, syntax, defaultFormat](const QString& fmt) {
        const QString sample = formatMoment(QDateTime::currentDateTime(), fmt.trimmed().isEmpty() ? defaultFormat : fmt.trimmed());
        QString html = descPrefixHtml;
        if (!html.isEmpty())
            html += u"<br>"_s;
        html += syntax + u"<br>"_s + tr("Your current syntax looks like this: ").toHtmlEscaped() + u"<b style=\"color:"_s
                + currentTheme()->color(u"--text-accent"_s).name() + u"\">"_s + sample.toHtmlEscaped() + u"</b>"_s;
        return html;
    };
    row.setRichDesc(sampleHtml(value));
    QPointer<SettingItem> r = &row;
    row.addText(value, defaultFormat, [r, sampleHtml, onChange](const QString& v) {
        if (r)
            r->setRichDesc(sampleHtml(v));
        onChange(v);
    });
}

// A plain "please confirm"-style information modal.
void infoModal(SettingsModal* modal, const QString& title, const QString& text)
{
    if (!modal || !modal->overlay())
        return;
    ConfirmModal::ask(modal->overlay(), title, text, {{tr("OK"), Modal::ButtonStyle::Cta}});
}

// ---------------------------------------------------------------------------------------------
// General
// ---------------------------------------------------------------------------------------------
class GeneralTab : public SettingsTab {
public:
    explicit GeneralTab(SettingsModal* m)
        : SettingsTab(m, u"about"_s, tr("General"), u"circle-user"_s)
    {
    }
    QList<SettingDef> definitions() override
    {
        QList<SettingDef> t;
        QString version = QCoreApplication::applicationVersion();
        if (version.isEmpty())
            version = u"dev"_s;
        SettingDef v = SettingDef::custom(tr("Version %1").arg(version), {}, [](SettingItem& row) {
            row.setRichDesc(tr("Qt version: %1").arg(QString::fromLatin1(qVersion())).toHtmlEscaped() + u"<br>"_s
                            + tr("Obsidian C++ is a native clone of Obsidian 1.13. It updates with your package manager.").toHtmlEscaped());
        });
        v.desc = tr("Obsidian C++ is a native clone of Obsidian 1.13.");
        v.searchable = true;
        t << v;
        SettingDef lang = SettingDef::custom(tr("Language"), tr("Change the display language."), [](SettingItem& row) {
            Dropdown* d = row.addDropdown({{u"en"_s, u"English"_s}}, u"en"_s);
            d->setObjectName(u"ocLanguage"_s);
        });
        t << lang;
        QPointer<SettingsModal> modal = this->modal();
        t << SettingDef::custom(tr("Help"), tr("Learn how to use Obsidian and get help from the community."), [modal](SettingItem& row) {
            row.addButton(tr("Open"), Button::Style::Normal, [modal] {
                if (!modal)
                    return;
                if (SettingsController* c = modal->controller())
                    emit c->openHelpRequested();
                if (VaultSession* s = modal->session(); s && s->commands() && s->commands()->find(u"app:open-help"_s))
                    s->commands()->execute(u"app:open-help"_s);
            });
        });
        t << SettingDef::custom(tr("Manage vaults"), tr("Open another vault, create a new one, or remove vaults from the list."),
                                [modal](SettingItem& row) {
                                    row.addButton(tr("Manage"), Button::Style::Normal, [modal] {
                                        if (!modal)
                                            return;
                                        QPointer<SettingsController> c = modal->controller();
                                        modal->close();
                                        if (c)
                                            emit c->manageVaultsRequested();
                                    });
                                });
        return t;
    }
};

// ---------------------------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------------------------

// Obsidian's font chooser page: the chosen fonts (first installed applies; a warning under missing
// ones; × removes) above a searchable list of the installed fonts (click adds) — typing a name that
// isn't listed offers it as a custom entry.
class FontChooser : public QWidget {
public:
    FontChooser(SettingsModal& modal, SettingsTab* tab, QString key)
        : m_modal(&modal)
        , m_tab(tab)
        , m_key(std::move(key))
    {
        setObjectName(u"ocFontChooser"_s);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        m_desc = new QLabel(this); // .setting-page-desc
        m_desc->setWordWrap(true);
        m_desc->setContentsMargins(8, 8, 8, 8);
        m_desc->setFont(currentTheme()->interfaceFont(currentTheme()->px(u"--font-ui-small"_s, -1, 13)));
        v->addWidget(m_desc);
        m_chosen = new SettingGroup(this);
        m_chosen->setList(true);
        m_chosen->setAlwaysVisible(false);
        v->addWidget(m_chosen);
        v->addSpacing(24);
        m_all = new SettingGroup(this);
        m_all->setList(true);
        m_search = m_all->addSearch(tr("Enter font name..."));
        m_search->setObjectName(u"ocFontSearch"_s);
        QObject::connect(m_search, &QLineEdit::textChanged, this, [this] { filter(); });
        m_custom = m_all->addItem();
        m_custom->setProperty("ocActionPlain", true);
        m_custom->setKind(SettingItem::Kind::Action);
        m_custom->setClickHandler([this] {
            const QString name = m_search->text().trimmed();
            if (!name.isEmpty())
                add(name);
        });
        QStringList families = QFontDatabase::families();
        families.removeDuplicates();
        QCollator col;
        col.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(families.begin(), families.end(), [&](const QString& a, const QString& b) { return col.compare(a, b) < 0; });
        for (const QString& f : families) {
            if (QFontDatabase::isPrivateFamily(f))
                continue;
            SettingItem* row = m_all->addItem();
            row->setProperty("ocActionPlain", true); // nameEl.style.color = var(--text-normal)
            row->setKind(SettingItem::Kind::Action);
            row->setName(f);
            row->setNameFamily(f);
            row->setClickHandler([this, f] { add(f); });
            m_rows.insert(f, row);
        }
        v->addWidget(m_all);
        rebuildChosen();
    }
    QStringList values() const { return parseFontList(m_tab ? m_tab->controlValue(m_key).toString() : QString()); }

private:
    void store(const QStringList& list)
    {
        if (!m_tab)
            return;
        m_tab->setControlValue(m_key, list.join(u","_s));
        if (SettingsController* c = controllerOf(*m_tab))
            c->applyAppearance();
        rebuildChosen();
    }
    void add(const QString& family)
    {
        QStringList list = values();
        if (!list.contains(family))
            list << family;
        store(list);
    }
    void rebuildChosen()
    {
        const QStringList list = values();
        m_desc->setText(list.isEmpty() ? tr("No custom font is applied right now. Add one below.")
                                       : tr("The first font from this list that is available on your system will be applied."));
        m_chosen->clearItems();
        for (const QString& f : list) {
            SettingItem* row = m_chosen->addItem();
            row->setName(f);
            row->setNameFamily(f);
            if (!fontInstalled(f)) {
                row->setDesc(tr("This font is not detected on your system."));
                row->setDescWarning(true);
            }
            row->addExtraButton(u"x"_s, tr("Delete"), [this, f] {
                QStringList l = values();
                l.removeAll(f);
                store(l);
            });
        }
        m_chosen->setVisible(!list.isEmpty());
        m_chosen->refresh();
        filter();
    }
    void filter()
    {
        const QStringList chosen = values();
        const QString q = m_search->text().trimmed();
        const SimpleSearch search(q);
        bool exact = false;
        for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
            const bool show = !chosen.contains(it.key()) && (search.isEmpty() || search.match(it.key()));
            it.value()->setVisible(show);
            exact = exact || it.key().compare(q, Qt::CaseInsensitive) == 0;
        }
        const bool offer = !q.isEmpty() && !exact && !chosen.contains(q);
        m_custom->setVisible(offer);
        if (offer) {
            m_custom->setName(q);
            m_custom->setNameFamily(q);
            m_custom->setDesc(fontInstalled(q) ? QString() : tr("This font is not detected on your system."));
            m_custom->setDescWarning(!fontInstalled(q));
        }
        m_all->refresh();
    }

    QPointer<SettingsModal> m_modal;
    QPointer<SettingsTab> m_tab;
    QString m_key;
    QLabel* m_desc;
    SettingGroup* m_chosen;
    SettingGroup* m_all;
    SearchInput* m_search;
    SettingItem* m_custom;
    QHash<QString, SettingItem*> m_rows;
};

class AppearanceTab : public SettingsTab {
public:
    explicit AppearanceTab(SettingsModal* m)
        : SettingsTab(m, u"appearance"_s, tr("Appearance"), u"palette"_s)
    {
    }
    QList<SettingDef> definitions() override
    {
        QList<SettingDef> n;
        QPointer<SettingsTab> self = this;
        auto apply = [self](const QJsonValue&) {
            if (self)
                if (SettingsController* c = controllerOf(*self))
                    c->applyAppearance();
        };
        SettingDef theme = SettingDef::dropdown(tr("Base color scheme"), tr("Choose Obsidian's default color scheme."), u"theme"_s,
                                                {{u"obsidian"_s, tr("Dark")}, {u"moonstone"_s, tr("Light")}, {u"system"_s, tr("Adapt to system")}});
        theme.aliases = {tr("dark light system night mode color scheme")};
        theme.control.afterChange = apply;
        n << theme;

        SettingDef accent = SettingDef::custom(tr("Accent color"), tr("Choose the accent color used throughout the app."), [self](SettingItem& row) {
            if (!self)
                return;
            static const QString kDefault = u"#8a5cf5"_s; // hsl(258, 88%, 66%)
            const QString stored = self->controlValue(u"accentColor"_s).toString();
            IconButton* restore = row.addExtraButton(u"rotate-ccw"_s, tr("Restore default"));
            restore->setObjectName(u"ocRestoreDefault"_s);
            restore->setEnabled(!stored.isEmpty());
            ColorSwatch* swatch = row.addColorPicker(QColor(stored.isEmpty() ? kDefault : stored));
            swatch->setObjectName(u"ocAccentColor"_s);
            QPointer<IconButton> rp = restore;
            QPointer<ColorSwatch> sp = swatch;
            QObject::connect(swatch, &ColorSwatch::colorChanged, swatch, [self, rp](const QColor& c) {
                if (!self)
                    return;
                self->setControlValue(u"accentColor"_s, c.name(QColor::HexRgb));
                if (rp)
                    rp->setEnabled(true);
                if (SettingsController* ctl = controllerOf(*self))
                    ctl->applyAppearance();
            });
            QObject::connect(restore, &IconButton::clicked, restore, [self, rp, sp] {
                if (!self)
                    return;
                self->setControlValue(u"accentColor"_s, u""_s);
                if (sp)
                    sp->setColor(QColor(kDefault));
                if (rp)
                    rp->setEnabled(false);
                if (SettingsController* ctl = controllerOf(*self))
                    ctl->applyAppearance();
            });
        });
        accent.aliases = {tr("highlight brand primary color")};
        n << accent;

        QPointer<SettingsModal> modal = this->modal();
        SettingDef themes = SettingDef::custom(tr("Themes"), tr("Manage installed themes and browse community themes."), [modal](SettingItem& row) {
            row.addDropdown({{QString(), tr("Default")}}, QString());
            row.addButton(tr("Manage"), Button::Style::Normal, [modal] {
                infoModal(modal, tr("Community themes"),
                          tr("Community themes aren't available in Obsidian C++: they are style sheets for Obsidian's Electron app, and "
                             "this native app draws its interface itself.\n\nTo change colors and sizes, use CSS snippets: the CSS "
                             "variables they set (for example --background-primary or --font-text-size) are applied."));
            });
        });
        themes.aliases = {tr("community browse install download")};
        n << themes;
        n << SettingDef::custom(tr("Current community themes"), {}, [](SettingItem& row) {
            row.setDesc(tr("You currently have %1 themes installed.").arg(0) + u" "_s);
        });

        // CSS snippets page.
        const QStringList snippets = css_snippets::list();
        const QStringList enabled = css_snippets::enabled(config());
        int enabledCount = 0;
        QList<SettingDef> items;
        for (const QString& name : snippets) {
            if (enabled.contains(name))
                ++enabledCount;
            SettingDef d = SettingDef::custom(name, {}, [self, name](SettingItem& row) {
                if (!self)
                    return;
                row.addToggle(css_snippets::enabled(self->config()).contains(name), [self, name](bool on) {
                    if (!self)
                        return;
                    css_snippets::setEnabled(self->config(), name, on);
                    if (SettingsController* c = controllerOf(*self))
                        c->applyAppearance();
                });
            });
            items << d;
        }
        SettingDef list = SettingDef::list(QString(), items);
        list.headerButtons = {{u"refresh-cw"_s, tr("Reload snippets"), [self, modal] {
                                   if (!self)
                                       return;
                                   if (SettingsController* c = controllerOf(*self))
                                       c->applyAppearance();
                                   if (modal)
                                       modal->notice(tr("Reloaded CSS snippets."));
                                   self->update();
                               }},
                              {u"folder-open"_s, tr("Open snippets folder"), [] {
                                   QDir().mkpath(css_snippets::folder());
                                   QDesktopServices::openUrl(QUrl::fromLocalFile(css_snippets::folder()));
                               }}};
        list.search = SettingDef::Search{tr("Search snippets..."), {}};
        list.emptyState = tr("CSS snippets let you customize the appearance of Obsidian. Place CSS files in “%1” for them to appear here.")
                              .arg(QDir::toNativeSeparators(css_snippets::folder()));
        SettingDef page = SettingDef::page(tr("CSS snippets"), tr("Manage your collection of CSS files for tweaking the app's appearance."), {list});
        page.displayValue = [enabledCount] { return tr("%1 enabled").arg(enabledCount); };
        n << page;

        // Font group.
        QList<SettingDef> fonts;
        auto fontPage = [this, self](const QString& name, const QString& desc, const QString& key) {
            const QStringList list = parseFontList(controlValue(key).toString());
            SettingDef p = SettingDef::page(name, desc, {});
            const int count = int(list.size());
            p.displayValue = [count] { return count == 0 ? QString() : count == 1 ? tr("%1 font").arg(1) : tr("%1 fonts").arg(count); };
            const bool missing = std::any_of(list.cbegin(), list.cend(), [](const QString& f) { return !fontInstalled(f); });
            p.warning = [missing] { return missing; };
            p.customPage = [self, key](SettingsModal& m) -> QWidget* { return self ? new FontChooser(m, self, key) : nullptr; };
            return p;
        };
        fonts << fontPage(tr("Interface font"), tr("Set base font for all of Obsidian."), u"interfaceFontFamily"_s);
        fonts << fontPage(tr("Text font"), tr("Set font for editing and reading views."), u"textFontFamily"_s);
        fonts << fontPage(tr("Monospace font"), tr("Set font for places like code blocks and frontmatter."), u"monospaceFontFamily"_s);
        SettingDef size = SettingDef::slider(tr("Font size"), tr("Font size in pixels that affects editing and reading views."),
                                             u"baseFontSize"_s, 10, 30, 1, 16);
        size.aliases = {tr("text size zoom")};
        size.control.afterChange = apply;
        fonts << size;
        fonts << SettingDef::toggle(tr("Quick font size adjustment"),
                                    tr("Adjust the font size using Ctrl + Scroll, or using the trackpad pinch-zoom gesture."), u"baseFontSizeAction"_s);
        n << SettingDef::group(tr("Font"), fonts);
        return n;
    }
};

// ---------------------------------------------------------------------------------------------
// Interface
// ---------------------------------------------------------------------------------------------
class InterfaceTab : public SettingsTab {
public:
    explicit InterfaceTab(SettingsModal* m)
        : SettingsTab(m, u"interface"_s, tr("Interface"), u"monitor"_s)
    {
    }
    QList<SettingDef> definitions() override
    {
        QList<SettingDef> n;
        QPointer<SettingsTab> self = this;
        n << SettingDef::toggle(tr("Show tab title bar"), tr("Display the header at the top of every tab."), u"showViewHeader"_s);
        SettingDef ribbon = SettingDef::toggle(tr("Show ribbon"), tr("Display vertical toolbar on the side of the window."), u"showRibbon"_s);
        ribbon.control.afterChange = [self](const QJsonValue&) {
            if (self)
                if (SettingsController* c = controllerOf(*self))
                    c->applyWindowOptions();
        };
        n << ribbon;
        n << ribbonPage();

        QList<SettingDef> adv;
        adv << SettingDef::toggle(tr("Native menus"),
                                  tr("Menus throughout the app will match the operating system. They will not be affected by your theme."),
                                  u"nativeMenus"_s);
        SettingDef frame = SettingDef::custom(
            tr("Window frame style"),
            tr("Determines the styling of the title bar of Obsidian windows. Requires a full restart to take effect."), [self](SettingItem& row) {
                if (!self)
                    return;
                SettingsController* c = controllerOf(*self);
                VaultRegistry* reg = c ? c->vaultRegistry() : nullptr;
                const QString current = reg ? reg->option(u"frame"_s).toString(u"hidden"_s) : u"hidden"_s;
                Button* relaunch = row.addButton(tr("Relaunch"), Button::Style::Cta, [self] {
                    if (self)
                        if (SettingsController* ctl = controllerOf(*self))
                            emit ctl->relaunchRequested();
                });
                relaunch->hide();
                QPointer<Button> rp = relaunch;
                Dropdown* d = row.addDropdown({{u"hidden"_s, tr("Hidden (default)")}, {u"custom"_s, tr("Obsidian frame")}, {u"native"_s, tr("Native frame")}},
                                              current, [self, rp](const QString& v) {
                                                  if (!self)
                                                      return;
                                                  SettingsController* ctl = controllerOf(*self);
                                                  if (VaultRegistry* r = ctl ? ctl->vaultRegistry() : nullptr) {
                                                      r->setOption(u"frame"_s, v);
                                                      r->save();
                                                  }
                                                  if (rp)
                                                      rp->show();
                                                  if (ctl)
                                                      emit ctl->frameStyleChanged(v);
                                              });
                d->setObjectName(u"ocFrameStyle"_s);
            });
        adv << frame;
        n << SettingDef::group(tr("Advanced"), adv);
        return n;
    }

    SettingDef ribbonPage()
    {
        QPointer<SettingsTab> self = this;
        MainWindow* w = window();
        Ribbon* ribbon = w ? w->ribbon() : nullptr;
        QList<SettingDef> shown, hidden;
        QStringList shownIds, hiddenIds;
        if (ribbon) {
            for (const auto& [id, button] : ribbon->actions()) {
                if (id.startsWith(u"app:"))
                    continue; // help / settings aren't plugin items
                SettingDef d;
                d.name = button->toolTip();
                d.icon = button->iconName();
                d.searchable = false;
                if (button->isHidden()) {
                    const QString aid = id;
                    QPointer<MainWindow> wp = w;
                    d.action = [self, wp, aid] {
                        if (!wp || !wp->ribbon())
                            return;
                        wp->ribbon()->setActionHidden(aid, false);
                        if (Workspace* ws = wp->workspace())
                            ws->requestSaveLayout();
                        if (self)
                            self->update();
                    };
                    hidden << d;
                    hiddenIds << id;
                } else {
                    shown << d;
                    shownIds << id;
                }
            }
        }
        QList<SettingDef> items;
        SettingDef visibleList = SettingDef::list(QString(), shown);
        QPointer<MainWindow> wp = w;
        visibleList.onDelete = [self, wp, shownIds](int index) {
            if (!wp || !wp->ribbon() || index < 0 || index >= shownIds.size())
                return;
            wp->ribbon()->setActionHidden(shownIds[index], true);
            if (Workspace* ws = wp->workspace())
                ws->requestSaveLayout();
            if (self)
                self->update();
        };
        items << visibleList;
        if (!hidden.isEmpty())
            items << SettingDef::list(tr("Other ribbon items"), hidden);
        SettingDef page = SettingDef::page(tr("Ribbon menu configuration"), tr("Configure what commands appear in the ribbon menu."), items);
        page.visible = [self] { return self && self->vaultConfig() && self->vaultConfig()->flag(u"showRibbon"_s); };
        return page;
    }
};

// ---------------------------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------------------------
class EditorTab : public SettingsTab {
public:
    explicit EditorTab(SettingsModal* m)
        : SettingsTab(m, u"editor"_s, tr("Editor"), u"pen-line"_s)
    {
    }
    QList<SettingDef> definitions() override
    {
        QList<SettingDef> t;
        QPointer<SettingsTab> self = this;
        t << SettingDef::dropdown(tr("Default view for new tabs"), tr("The default view that a new Markdown tab gets opened in."),
                                  u"defaultViewMode"_s, {{u"source"_s, tr("Editing view")}, {u"preview"_s, tr("Reading view")}});
        t << SettingDef::custom(tr("Default editing mode"), tr("The default editing mode a new tab will start with."), [self](SettingItem& row) {
            if (!self)
                return;
            const bool live = self->controlValue(u"livePreview"_s).toBool(true);
            Dropdown* d = row.addDropdown({{u"live-preview"_s, tr("Live Preview")}, {u"source"_s, tr("Source mode")}},
                                          live ? u"live-preview"_s : u"source"_s, [self](const QString& v) {
                                              if (!self)
                                                  return;
                                              const bool live = v == u"live-preview";
                                              self->setControlValue(u"livePreview"_s, live);
                                              // Obsidian switches every open Markdown tab that is being edited.
                                              if (Workspace* ws = self->window() ? self->window()->workspace() : nullptr) {
                                                  for (WorkspaceLeaf* l : ws->leavesOfType(u"markdown"_s)) {
                                                      auto* mv = qobject_cast<MarkdownView*>(l->view());
                                                      if (mv && mv->mode() != EditorView::Mode::Reading)
                                                          mv->setMode(live ? EditorView::Mode::LivePreview : EditorView::Mode::Source);
                                                  }
                                              }
                                          });
            d->setObjectName(u"ocDefaultEditingMode"_s);
        });
        t << SettingDef::custom(tr("Show editing mode in status bar"), tr("Show the editing mode toggle in the status bar."), [self](SettingItem& row) {
            SettingsController* c = self ? controllerOf(*self) : nullptr;
            row.addToggle(c && c->isCorePluginEnabled(u"editor-status"_s), [self](bool on) {
                if (self)
                    if (SettingsController* ctl = controllerOf(*self))
                        ctl->setCorePluginEnabled(u"editor-status"_s, on);
            });
        });

        QList<SettingDef> display;
        display << SettingDef::toggle(tr("Inline title"), tr("Display the filename as an editable title inline with the file contents."),
                                      u"showInlineTitle"_s);
        display << SettingDef::toggle(tr("Readable line length"),
                                      tr("Limit maximum line length. Less content fits onscreen, but long blocks of text are more readable."),
                                      u"readableLineLength"_s);
        display << SettingDef::toggle(
            tr("Strict line breaks"), tr("Markdown specs ignore single line breaks in reading view. Turn this off to make single line breaks visible."),
            u"strictLineBreaks"_s);
        display << SettingDef::dropdown(
            tr("Properties in document"),
            tr("Choose how properties are displayed at the top of notes. Select “source” to show properties as raw YAML."), u"propertiesInDocument"_s,
            {{u"visible"_s, tr("Visible")}, {u"hidden"_s, tr("Hidden")}, {u"source"_s, tr("Source")}});
        display << SettingDef::toggle(tr("Fold heading"), tr("Lets you fold all content under a heading."), u"foldHeading"_s);
        display << SettingDef::toggle(tr("Fold indent"), tr("Lets you fold part of an indentation, such as lists."), u"foldIndent"_s);
        display << SettingDef::toggle(tr("Line numbers"), tr("Show line numbers in the gutter."), u"showLineNumber"_s);
        display << SettingDef::toggle(tr("Indentation guides"), tr("Show vertical relationship lines between list items."), u"showIndentGuide"_s);
        display << SettingDef::toggle(tr("Right-to-left (RTL)"), tr("Sets the default text direction of notes to right-to-left."), u"rightToLeft"_s);
        t << SettingDef::group(tr("Display"), display);

        QList<SettingDef> behavior;
        SettingDef spell = SettingDef::toggle(tr("Spellcheck"), tr("Turn on the spellchecker."), u"spellcheck"_s);
        spell.control.afterChange = [](const QJsonValue& v) { SpellChecker::instance().setEnabled(v.toBool()); };
        behavior << spell;
        behavior << spellcheckLanguages();
        behavior << SettingDef::toggle(tr("Auto-pair brackets"), tr("Pair brackets and quotes automatically."), u"autoPairBrackets"_s);
        behavior << SettingDef::toggle(tr("Auto-pair Markdown syntax"), tr("Pair symbols automatically for bold, italic, code, and more."),
                                       u"autoPairMarkdown"_s);
        behavior << SettingDef::toggle(tr("Smart lists"), tr("Automatically set indentation and place list items correctly."), u"smartIndentList"_s);
        behavior << SettingDef::toggle(tr("Indent using tabs"),
                                       tr("Use tabs to indent by pressing the “Tab” key. Turn this off to indent using 4 spaces."), u"useTab"_s);
        behavior << SettingDef::slider(tr("Indent visual width"), tr("Number of spaces a tab character will render as."), u"tabSize"_s, 2, 8, 1, 4);
        t << SettingDef::group(tr("Behavior"), behavior);

        QList<SettingDef> adv;
        adv << SettingDef::toggle(tr("Convert pasted HTML to Markdown"),
                                  tr("Automatically convert HTML to Markdown when pasting and drag-and-drop from web pages. Use Ctrl/Cmd+Shift+V to "
                                     "paste HTML without converting."),
                                  u"autoConvertHtml"_s);
        adv << SettingDef::custom(tr("Vim key bindings"), tr("Use Vim key bindings when editing."), [self](SettingItem& row) {
            if (!self)
                return;
            ToggleSwitch* toggle = row.addToggle(self->controlValue(u"vimMode"_s).toBool());
            toggle->setObjectName(u"ocVimToggle"_s);
            QPointer<ToggleSwitch> tp = toggle;
            QObject::connect(toggle, &ToggleSwitch::toggled, toggle, [self, tp](bool on) {
                if (!self)
                    return;
                SettingsController* c = controllerOf(*self);
                if (on && c && !c->property("ocPassedVimTest").toBool()) {
                    if (tp) {
                        QSignalBlocker block(tp);
                        tp->setChecked(false);
                    }
                    startVimTest(self, tp);
                    return;
                }
                self->setControlValue(u"vimMode"_s, on);
            });
        });
        t << SettingDef::group(tr("Advanced"), adv);
        return t;
    }

    // Obsidian's "Confirm entering Vim mode" test: the answer must start with ":q".
    static void startVimTest(QPointer<SettingsTab> tab, QPointer<ToggleSwitch> toggle)
    {
        SettingsModal* settings = tab ? tab->modal() : nullptr;
        if (!settings || !settings->overlay())
            return;
        auto* m = new Modal(settings->overlay());
        m->setObjectName(u"ocVimTestModal"_s);
        m->setTitle(tr("Confirm entering Vim mode"));
        m->addParagraph(tr("Vim mode is for efficient text editing but can be quite counter-intuitive. If you're not familiar with Vim, this option "
                           "might make it look like Obsidian has stopped working."),
                        true);
        m->addParagraph(tr("To verify that you know your way around Vim, please enter the command to quit Vim without saving below:"));
        auto* field = new QWidget;
        auto* fv = new QVBoxLayout(field);
        fv->setContentsMargins(0, 0, 0, 0);
        fv->setSpacing(6);
        auto* label = new QLabel(tr("Your answer"), field);
        fv->addWidget(label);
        auto* input = new QLineEdit(field);
        input->setObjectName(u"ocVimAnswer"_s);
        input->setPlaceholderText(tr("Enter command..."));
        input->setFixedHeight(30);
        fv->addWidget(input);
        m->addContentWidget(field);
        const int ok = m->addButton(tr("Let me enable Vim"), Modal::ButtonStyle::Cta, false);
        m->addButton(tr("Cancel"));
        QPointer<Modal> mp = m;
        auto submit = [mp, input, tab, toggle, settings = QPointer<SettingsModal>(settings)] {
            const QString answer = input->text().toLower();
            if (answer.isEmpty()) {
                if (settings)
                    settings->notice(tr("Please enter the command to enable Vim mode"));
                return;
            }
            if (answer.startsWith(u":q")) {
                if (tab) {
                    if (SettingsController* c = controllerOf(*tab))
                        c->setProperty("ocPassedVimTest", true);
                    tab->setControlValue(u"vimMode"_s, true);
                }
                if (toggle) {
                    QSignalBlocker block(toggle);
                    toggle->setChecked(true);
                }
                if (mp)
                    mp->close(0);
                if (settings)
                    settings->notice(tr("Command is correct. Vim mode now enabled."));
            } else if (settings) {
                settings->notice(tr("Command is incorrect. Vim mode remains disabled to protect you."));
            }
        };
        QObject::connect(m, &Modal::buttonClicked, m, [ok, submit](int index) {
            if (index == ok)
                submit();
        });
        QObject::connect(input, &QLineEdit::returnPressed, m, submit);
        m->setDefaultButton(ok);
        m->open();
        input->setFocus(Qt::OtherFocusReason);
    }

    SettingDef spellcheckLanguages()
    {
        QPointer<SettingsTab> self = this;
        SettingDef d = SettingDef::custom(tr("Spellcheck languages"), tr("Choose the languages for the spellchecker to use."), [self](SettingItem& row) {
            if (!self)
                return;
            Sonnet::Speller speller;
            const QMap<QString, QString> dicts = speller.availableDictionaries(); // name -> code
            QHash<QString, QString> nameOf;
            for (auto it = dicts.cbegin(); it != dicts.cend(); ++it)
                nameOf.insert(it.value(), it.key());
            auto display = [&nameOf](const QString& code) {
                const QString n = nameOf.value(code);
                return n.isEmpty() ? QLocale(code).nativeLanguageName() + tr(" (Not available)") : n;
            };
            QStringList langs;
            for (const QJsonValue& v : self->controlValue(u"spellcheckLanguages"_s).toArray())
                if (v.isString())
                    langs << v.toString();
            auto* chips = new QWidget;
            chips->setObjectName(u"ocSpellcheckLanguages"_s);
            auto* ch = new QHBoxLayout(chips);
            ch->setContentsMargins(0, 0, 0, 0);
            ch->setSpacing(4);
            auto store = [self](const QStringList& list) {
                if (!self)
                    return;
                self->setControlValue(u"spellcheckLanguages"_s, list.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonArray::fromStringList(list));
                if (!list.isEmpty())
                    SpellChecker::instance().setLanguage(list.first());
                self->update();
            };
            if (langs.isEmpty()) {
                const QString sys = QLocale::system().name();
                ch->addWidget(new HotkeyChip(nameOf.value(sys, QLocale::system().nativeLanguageName()), HotkeyChip::State::Normal, false, chips));
            } else {
                for (const QString& code : langs) {
                    auto* chip = new HotkeyChip(display(code), HotkeyChip::State::Normal, true, chips);
                    QObject::connect(chip, &HotkeyChip::deleteClicked, chip, [store, langs, code] {
                        QStringList l = langs;
                        l.removeAll(code);
                        store(l);
                    });
                    ch->addWidget(chip);
                }
            }
            row.addControl(chips);
            if (!langs.isEmpty())
                row.addExtraButton(u"rotate-ccw"_s, tr("Restore default"), [store] { store({}); });
            Options options{{QString(), u"+"_s}};
            QList<QPair<QString, QString>> sorted;
            for (auto it = dicts.cbegin(); it != dicts.cend(); ++it)
                sorted.append({it.value(), it.key()});
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.localeAwareCompare(b.second) < 0; });
            options += sorted;
            Dropdown* add = row.addDropdown(options, QString(), [store, langs](const QString& code) {
                if (code.isEmpty() || langs.contains(code))
                    return;
                QStringList l = langs.isEmpty() ? QStringList{QLocale::system().name()} : langs;
                if (!l.contains(code))
                    l << code;
                store(l);
            });
            add->setFixedWidth(80);
        });
        return d;
    }
};

// ---------------------------------------------------------------------------------------------
// Files and links
// ---------------------------------------------------------------------------------------------
class FilesTab : public SettingsTab {
public:
    explicit FilesTab(SettingsModal* m)
        : SettingsTab(m, u"file"_s, tr("Files and links"), u"folder-cog"_s)
    {
    }

    QString attachmentMode() const
    {
        const QString p = controlValue(u"attachmentFolderPath"_s).toString();
        if (p == u"/")
            return u"root"_s;
        if (p == u"." || p == u"./")
            return u"current"_s;
        if (p.startsWith(u"./"))
            return u"subfolder"_s;
        return u"folder"_s;
    }

    QList<SettingDef> definitions() override
    {
        QList<SettingDef> s;
        QPointer<FilesTab> self = this;
        const QString attachmentsDefault = tr("Attachments");

        // Default file to open ("" last opened, "new", "file:<path>", "daily").
        s << SettingDef::custom(tr("Default file to open"), tr("Choose which file to open when the app starts."), [self](SettingItem& row) {
            if (!self)
                return;
            Options o{{QString(), tr("Last opened")}, {u"new"_s, tr("New note")}, {u"file"_s, tr("Specific file")}};
            SettingsController* c = controllerOf(*self);
            if (c && c->isCorePluginEnabled(u"daily-notes"_s))
                o.append({u"daily"_s, tr("Daily note")});
            QString v = self->controlValue(u"openBehavior"_s).toString();
            if (v.startsWith(u"file:"))
                v = u"file"_s;
            Dropdown* d = row.addDropdown(o, v, [self](const QString& nv) {
                if (!self)
                    return;
                if (nv == u"file") {
                    const QString cur = self->controlValue(u"openBehavior"_s).toString();
                    self->setControlValue(u"openBehavior"_s, cur.startsWith(u"file:") ? cur : u"file:"_s);
                } else {
                    self->setControlValue(u"openBehavior"_s, nv);
                }
                self->refreshState();
            });
            d->setObjectName(u"ocOpenBehavior"_s);
        });
        SettingDef fileToOpen = SettingDef::custom(tr("File to open"), tr("Select a specific file to open by default."), [self](SettingItem& row) {
            if (!self)
                return;
            auto* picker = new FilePicker(self->session(), self->modal() ? self->modal()->overlay() : nullptr);
            const QString cur = self->controlValue(u"openBehavior"_s).toString();
            picker->setValue(cur.startsWith(u"file:") ? cur.mid(5) : QString());
            QObject::connect(picker, &FilePicker::valueChanged, picker, [self](const QString& path) {
                if (self)
                    self->setControlValue(u"openBehavior"_s, QString(u"file:"_s + path));
            });
            row.addControl(picker);
        });
        fileToOpen.visible = [self] { return self && self->controlValue(u"openBehavior"_s).toString().startsWith(u"file:"); };
        s << fileToOpen;
        s << SettingDef::toggle(tr("Always focus new tabs"), tr("When you open a link in a new tab, switch to it immediately."), u"focusNewTab"_s);
        s << SettingDef::dropdown(tr("Default location for new notes"), tr("Where newly created notes are placed."), u"newFileLocation"_s,
                                  {{u"root"_s, tr("Vault folder")}, {u"current"_s, tr("Same folder as current file")},
                                   {u"folder"_s, tr("In the folder specified below")}});
        SettingDef newFolder = SettingDef::folder(tr("Folder to create new notes in"), tr("Newly created notes will appear in this folder."),
                                                  u"newFileFolderPath"_s, tr("Example: folder 1/folder 2"), true);
        newFolder.visible = [self] { return self && self->controlValue(u"newFileLocation"_s).toString() == u"folder"; };
        s << newFolder;

        // Attachments: one app.json key ("attachmentFolderPath") behind three controls.
        s << SettingDef::custom(tr("Default location for new attachments"), tr("Where newly added attachments are placed."), [self, attachmentsDefault](SettingItem& row) {
            if (!self)
                return;
            Dropdown* d = row.addDropdown({{u"root"_s, tr("Vault folder")},
                                           {u"current"_s, tr("Same folder as current file")},
                                           {u"subfolder"_s, tr("In subfolder under current folder")},
                                           {u"folder"_s, tr("In the folder specified below")}},
                                          self->attachmentMode(), [self, attachmentsDefault](const QString& mode) {
                                              if (!self)
                                                  return;
                                              QString value;
                                              if (mode == u"root")
                                                  value = u"/"_s;
                                              else if (mode == u"current")
                                                  value = u"./"_s;
                                              else if (mode == u"subfolder")
                                                  value = u"./"_s + attachmentsDefault;
                                              else
                                                  value = attachmentsDefault;
                                              self->setControlValue(u"attachmentFolderPath"_s, value);
                                              self->update();
                                          });
            d->setObjectName(u"ocAttachmentLocation"_s);
        });
        SettingDef attFolder = SettingDef::custom(
            tr("Attachment folder path"),
            tr("Place newly created attachment files, such as images created via drag-and-drop or audio recordings, in this folder."),
            [self, attachmentsDefault](SettingItem& row) {
                if (!self)
                    return;
                const QString cur = self->controlValue(u"attachmentFolderPath"_s).toString();
                QLineEdit* e = row.addText(cur == attachmentsDefault ? QString() : cur, attachmentsDefault, [self, attachmentsDefault](const QString& v) {
                    if (self)
                        self->setControlValue(u"attachmentFolderPath"_s, v.isEmpty() ? attachmentsDefault : v);
                });
                auto* sug = new FolderSuggest(e, self->session(), self->modal() ? self->modal()->overlay() : nullptr, false);
                QObject::connect(sug, &FolderSuggest::chosen, e, [self](const QString& path) {
                    if (self)
                        self->setControlValue(u"attachmentFolderPath"_s, path);
                });
            });
        attFolder.visible = [self] { return self && self->attachmentMode() == u"folder"; };
        s << attFolder;
        SettingDef attSub = SettingDef::custom(
            tr("Subfolder name"),
            tr("If your file is in “vault/folder”, and you set subfolder name to “attachments”, attachments will be saved to “vault/folder/attachments”."),
            [self, attachmentsDefault](SettingItem& row) {
                if (!self)
                    return;
                const QString sub = self->controlValue(u"attachmentFolderPath"_s).toString().mid(2);
                row.addText(sub == attachmentsDefault ? QString() : sub, attachmentsDefault, [self, attachmentsDefault](const QString& v) {
                    if (self)
                        self->setControlValue(u"attachmentFolderPath"_s, QString(u"./"_s + (v.isEmpty() ? attachmentsDefault : v)));
                });
            });
        attSub.visible = [self] { return self && self->attachmentMode() == u"subfolder"; };
        s << attSub;

        QList<SettingDef> links;
        links << SettingDef::dropdown(tr("New link format"), tr("What links to insert when auto-generating internal links."), u"newLinkFormat"_s,
                                      {{u"shortest"_s, tr("Shortest path when possible")}, {u"relative"_s, tr("Path from current file")},
                                       {u"absolute"_s, tr("Path from vault folder")}});
        links << SettingDef::toggle(tr("Automatically update internal links"), tr("Turn off to be prompted to update links after renaming a file."),
                                    u"alwaysUpdateLinks"_s);
        links << SettingDef::custom(tr("Use [[Wikilinks]]"),
                                    tr("Auto-generate Wikilinks for [[links]] and ![[images]] instead of Markdown links and images. Disable this option to "
                                       "generate Markdown links instead."),
                                    [self](SettingItem& row) {
                                        if (!self)
                                            return;
                                        ToggleSwitch* t = row.addToggle(!self->controlValue(u"useMarkdownLinks"_s).toBool(), [self](bool on) {
                                            if (self)
                                                self->setControlValue(u"useMarkdownLinks"_s, !on);
                                        });
                                        t->setObjectName(u"ocUseWikilinks"_s);
                                    });
        links << SettingDef::toggle(tr("Show all file types"),
                                    tr("Show files with any extension even if Obsidian can't open them natively, so you can link to them and see them "
                                       "in File Explorer and Quick Switcher."),
                                    u"showUnsupportedFiles"_s);
        s << SettingDef::group(tr("Links"), links);

        QList<SettingDef> trash;
        trash << SettingDef::toggle(tr("Confirm before deleting files"), tr("Avoid accidentally deleting files."), u"promptDelete"_s);
        trash << SettingDef::dropdown(tr("Delete attachments when deleting files"),
                                      tr("Automatically remove attachments linked to the deleted file if they're not used elsewhere."),
                                      u"deleteUnlinkedAttachments"_s,
                                      {{u"ask"_s, tr("Ask each time")}, {u"always"_s, tr("Always")}, {u"never"_s, tr("Never")}});
        trash << SettingDef::dropdown(tr("Deleted files"), tr("What happens to a file after you delete it."), u"trashOption"_s,
                                      {{u"system"_s, tr("Move to system trash")}, {u"local"_s, tr("Move to Obsidian trash (.trash folder)")},
                                       {u"none"_s, tr("Permanently delete")}});
        s << SettingDef::group(tr("Trash"), trash);

        QList<SettingDef> adv;
        adv << excludedFilesPage();
        SettingDef uri = SettingDef::toggle(tr("Allow URI callbacks"),
                                            tr("Enable the use of x-callback-url through x-success or x-error when handling Obsidian URIs."),
                                            u"uriCallbacks"_s);
        uri.richDesc = uri.desc.toHtmlEscaped() + u" "_s
                       + link(u"https://help.obsidian.md/Extending+Obsidian/Obsidian+URI#Use+x-callback-url+parameters"_s, tr("Learn more"));
        adv << uri;
        QPointer<SettingsModal> modal = this->modal();
        SettingDef rebuild = SettingDef::custom(
            tr("Rebuild vault cache"),
            tr("Rebuilding the cache could take a few seconds to a few minutes depending on the size of your vault."), [self, modal](SettingItem& row) {
                row.addButton(tr("Rebuild"), Button::Style::Normal, [self, modal] {
                    VaultSession* s = self ? self->session() : nullptr;
                    if (!s || !s->vault() || !s->metadata())
                        return;
                    QStringList paths;
                    for (TFile* f : s->vault()->files())
                        paths << f->path();
                    s->metadata()->indexAll(paths);
                    if (modal)
                        modal->notice(tr("Rebuilding vault cache..."));
                });
            });
        rebuild.descWarning = true;
        adv << rebuild;
        s << SettingDef::group(tr("Advanced"), adv);
        return s;
    }

    SettingDef excludedFilesPage()
    {
        QPointer<FilesTab> self = this;
        QStringList filters;
        for (const QJsonValue& v : controlValue(u"userIgnoreFilters"_s).toArray())
            if (v.isString())
                filters << v.toString();
        auto store = [self](const QStringList& list) {
            if (!self)
                return;
            self->setControlValue(u"userIgnoreFilters"_s, list.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonArray::fromStringList(list));
            self->update();
        };
        QList<SettingDef> items;
        for (const QString& f : filters) {
            SettingDef d;
            d.name = f;
            d.searchable = false;
            if (f.size() > 2 && f.startsWith(u'/') && f.endsWith(u'/'))
                d.render = [](SettingItem& row) {
                    auto* flair = new QLabel(tr("Regex"));
                    flair->setProperty("ocText", u"muted"_s);
                    flair->setFont(currentTheme()->interfaceFont(currentTheme()->px(u"--font-ui-smaller"_s, -1, 12)));
                    row.addControl(flair);
                };
            items << d;
        }
        SettingDef list = SettingDef::list(QString(), items);
        list.emptyState = tr("No exclusions added.");
        list.addItemName = tr("Add exclusion...");
        QPointer<SettingsModal> modal = this->modal();
        list.addItem = [self, modal, filters, store] {
            if (!modal || !modal->overlay() || !self)
                return;
            auto* m = new Modal(modal->overlay());
            m->setObjectName(u"ocAddExclusionModal"_s);
            m->setTitle(tr("Add exclusion"));
            auto* field = new QWidget;
            auto* fv = new QVBoxLayout(field);
            fv->setContentsMargins(0, 0, 0, 0);
            fv->setSpacing(6);
            auto* input = new QLineEdit(field);
            input->setObjectName(u"ocExclusionInput"_s);
            input->setFixedHeight(30);
            fv->addWidget(input);
            auto* help = new QLabel(tr("Enter a path or a regular expression, e.g. /daily-.*/"), field);
            help->setProperty("ocText", u"muted"_s);
            help->setFont(currentTheme()->interfaceFont(currentTheme()->px(u"--font-ui-smaller"_s, -1, 12)));
            fv->addWidget(help);
            m->addContentWidget(field);
            auto* sug = new FolderSuggest(input, self->session(), modal->overlay(), false);
            QObject::connect(sug, &FolderSuggest::chosen, input, [input](const QString& path) { input->setText(path + u'/'); });
            const int add = m->addButton(tr("Add"), Modal::ButtonStyle::Cta, false);
            m->addButton(tr("Cancel"));
            QPointer<Modal> mp = m;
            auto submit = [mp, input, filters, store, modal] {
                const QString v = input->text();
                if (v.trimmed().isEmpty()) {
                    if (modal)
                        modal->notice(tr("Filter cannot be empty"));
                    return;
                }
                if (mp)
                    mp->close(0);
                store(filters + QStringList{v});
            };
            QObject::connect(m, &Modal::buttonClicked, m, [add, submit](int i) {
                if (i == add)
                    submit();
            });
            m->setDefaultButton(add);
            m->open();
            input->setFocus(Qt::OtherFocusReason);
        };
        list.onDelete = [filters, store](int index) {
            QStringList l = filters;
            if (index >= 0 && index < l.size())
                l.removeAt(index);
            store(l);
        };
        SettingDef page = SettingDef::page(
            tr("Excluded files"),
            tr("Excluded files will be hidden in Search, Graph View, and Unlinked Mentions, less noticeable in Quick Switcher and link suggestions."),
            {list});
        const int count = int(filters.size());
        page.displayValue = [count] { return count == 0 ? QString() : count == 1 ? tr("%1 rule").arg(1) : tr("%1 rules").arg(count); };
        return page;
    }
};

// ---------------------------------------------------------------------------------------------
// Keychain
// ---------------------------------------------------------------------------------------------
class KeychainTab : public SettingsTab {
public:
    explicit KeychainTab(SettingsModal* m)
        : SettingsTab(m, u"keychain"_s, tr("Keychain"), u"key-round"_s)
    {
    }
    QList<SettingDef> definitions() override
    {
        SettingDef list = SettingDef::list(tr("Secrets"), {});
        QPointer<SettingsModal> modal = this->modal();
        list.addItemName = tr("Add secret");
        list.addItem = [modal] {
            infoModal(modal, tr("Add secret"),
                      tr("Secrets store API keys and passwords for community plugins. Obsidian C++ doesn't run community plugins, so there is "
                         "nothing that could use them yet."));
        };
        list.search = SettingDef::Search{tr("Find secret..."), {}};
        list.emptyState = tr("No secrets have been added. Secrets are used to store information like API keys and passwords that plugins can use.");
        return {list};
    }
};

// ---------------------------------------------------------------------------------------------
// Core plugins
// ---------------------------------------------------------------------------------------------
class CorePluginsTab : public SettingsTab {
public:
    explicit CorePluginsTab(SettingsModal* m)
        : SettingsTab(m, u"plugins"_s, tr("Core plugins"), u"toy-brick"_s)
    {
    }
    QList<SettingDef> definitions() override
    {
        QPointer<CorePluginsTab> self = this;
        QList<CorePluginInfo> plugins = corePluginInfos();
        plugins.removeIf([](const CorePluginInfo& p) { return p.id == u"editor-status"; }); // hiddenFromList
        QCollator col;
        col.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(plugins.begin(), plugins.end(), [&](const CorePluginInfo& a, const CorePluginInfo& b) { return col.compare(a.name, b.name) < 0; });
        QList<SettingDef> items;
        for (const CorePluginInfo& p : plugins) {
            SettingDef d;
            d.name = p.name;
            d.desc = p.description;
            const QString id = p.id;
            d.render = [self, id](SettingItem& row) {
                if (!self)
                    return;
                row.setProperty("ocPluginId", id);
                SettingsModal* m = self->modal();
                SettingsController* c = controllerOf(*self);
                const bool enabled = c && c->isCorePluginEnabled(id);
                IconButton* gear = row.addExtraButton(u"settings"_s, tr("Options"), [self, id] {
                    if (self && self->modal())
                        self->modal()->openTab(id);
                });
                gear->setObjectName(u"ocPluginOptions"_s);
                gear->setVisible(enabled && m && m->tab(id));
                IconButton* keys = row.addExtraButton(u"plus-circle"_s, tr("Hotkeys"), [self, id] {
                    if (!self || !self->modal())
                        return;
                    if (auto* hk = qobject_cast<HotkeysTab*>(self->modal()->openTab(u"hotkeys"_s)))
                        hk->setQuery(id);
                });
                keys->setObjectName(u"ocPluginHotkeys"_s);
                bool hasCommands = false;
                if (CommandRegistry* r = self->commands())
                    for (const Command* cmd : r->all())
                        hasCommands = hasCommands || cmd->id.startsWith(id + u':') || cmd->id == id;
                keys->setVisible(enabled && hasCommands);
                ToggleSwitch* t = row.addToggle(enabled, [self, id](bool on) {
                    if (!self)
                        return;
                    if (SettingsController* ctl = controllerOf(*self))
                        ctl->setCorePluginEnabled(id, on);
                    self->update();
                });
                t->setObjectName(u"ocPluginToggle"_s);
            };
            items << d;
        }
        SettingDef list = SettingDef::list(QString(), items);
        list.search = SettingDef::Search{tr("Search plugins..."), [](const SettingDef& item, const QString& query) {
                                             const QString q = query.trimmed().toLower();
                                             if (q.isEmpty())
                                                 return true;
                                             const SimpleSearch s(q);
                                             return bool(s.match(item.name)) || bool(s.match(item.desc));
                                         }};
        return {list};
    }
};

// ---------------------------------------------------------------------------------------------
// Community plugins
// ---------------------------------------------------------------------------------------------
class CommunityPluginsTab : public SettingsTab {
public:
    explicit CommunityPluginsTab(SettingsModal* m)
        : SettingsTab(m, u"community-plugins"_s, tr("Community plugins"), u"puzzle"_s)
    {
    }
    QList<SettingDef> definitions() override
    {
        QPointer<CommunityPluginsTab> self = this;
        QList<SettingDef> t;
        SettingDef intro;
        intro.name = tr("Obsidian C++ runs in Restricted mode.");
        intro.desc = tr("Community plugins are JavaScript programs written for Obsidian's Electron app. This native app has no JavaScript engine "
                        "and no plugin API, so community plugins can't be installed or run here. Plugins can be ported natively instead, like "
                        "Greentext Guard below.");
        intro.searchable = true;
        t << intro;
        t << SettingDef::custom(tr("Restricted mode"), tr("Turn on Restricted mode to use the app without community plugins."), [](SettingItem& row) {
            Button* b = row.addButton(tr("Always on"), Button::Style::Normal);
            b->setEnabled(false);
        });
        t << SettingDef::custom(tr("Community plugins"), tr("Browse and install community plugins made by the community."), [](SettingItem& row) {
            Button* b = row.addButton(tr("Browse"), Button::Style::Normal);
            b->setEnabled(false);
        });
        SettingDef gg;
        gg.name = tr("Greentext Guard");
        gg.desc = tr("Keeps lines starting with “>” from turning into blockquotes in chosen notes: pasted greentext is wrapped in a code fence "
                     "and typed “>” is escaped. Built into Obsidian C++.");
        gg.render = [self](SettingItem& row) {
            if (!self)
                return;
            SettingsController* c = controllerOf(*self);
            const bool on = c && c->isCommunityPluginEnabled(u"greentext-guard"_s);
            IconButton* gear = row.addExtraButton(u"settings"_s, tr("Options"), [self] {
                if (self && self->modal())
                    self->modal()->openTab(u"greentext-guard"_s);
            });
            gear->setVisible(on);
            ToggleSwitch* t = row.addToggle(on, [self](bool enabled) {
                if (!self)
                    return;
                if (SettingsController* ctl = controllerOf(*self))
                    ctl->setCommunityPluginEnabled(u"greentext-guard"_s, enabled);
                self->update();
            });
            t->setObjectName(u"ocCommunityPluginToggle"_s);
        };
        SettingDef installed = SettingDef::list(tr("Installed plugins"), {gg});
        installed.search = SettingDef::Search{tr("Search installed plugins..."), {}};
        t << installed;
        return t;
    }
};

// ---------------------------------------------------------------------------------------------
// Plugin option tabs
// ---------------------------------------------------------------------------------------------

PluginSettingsTab* pluginTab(SettingsModal& m, const QString& id, const QString& file, const QString& name, const QString& icon,
                             std::function<QList<SettingDef>(PluginSettingsTab&)> build,
                             SettingsTab::Section section = SettingsTab::Section::CorePlugins)
{
    auto* t = new PluginSettingsTab(&m, id, file, name, icon, section);
    t->build = std::move(build);
    return t;
}

QList<SettingDef> dailyNotesDefs(PluginSettingsTab& tab)
{
    QPointer<PluginSettingsTab> self = &tab;
    static const QString kDefault = u"YYYY-MM-DD"_s;
    static const QStringList presets = {kDefault, u"YYYY.MM.DD"_s, u"YYYY/MM/DD"_s, u"YYYY/MM/YYYY-MM-DD"_s};
    QList<SettingDef> n;
    auto isCustom = [self] {
        const QString f = self ? self->controlValue(u"format"_s).toString() : QString();
        return !presets.contains(f.isEmpty() ? kDefault : f);
    };
    SettingDef fmt = SettingDef::custom(tr("Date format"), tr("Choose how daily note are named in your vault."), [self, isCustom](SettingItem& row) {
        if (!self)
            return;
        const QDateTime now = QDateTime::currentDateTime();
        Options o;
        for (const QString& p : presets)
            o.append({p, formatMoment(now, p)});
        o.append({u"custom"_s, tr("Custom")});
        const QString f = self->controlValue(u"format"_s).toString();
        Dropdown* d = row.addDropdown(o, isCustom() ? u"custom"_s : (f.isEmpty() ? kDefault : f), [self](const QString& v) {
            if (!self)
                return;
            if (v == u"custom") {
                self->setProperty("ocCustomFormat", true);
            } else {
                self->setProperty("ocCustomFormat", false);
                self->setControlValue(u"format"_s, v);
            }
            self->refreshState();
        });
        d->setObjectName(u"ocDailyFormat"_s);
    });
    n << fmt;
    SettingDef custom = SettingDef::custom(tr("Custom format"), {}, [self](SettingItem& row) {
        if (!self)
            return;
        momentFormatRow(row, QString(), self->controlValue(u"format"_s).toString(), kDefault, [self, &row](const QString& v) {
            if (!self)
                return;
            // Obsidian validates: the formatted name must parse back and stay a valid file name.
            const QString sample = formatMoment(QDateTime::currentDateTime(), v.trimmed().isEmpty() ? kDefault : v.trimmed());
            static const QRegularExpression bad(u"[\\\\:*?\"<>|]"_s);
            if (v.trimmed().startsWith(u'/') || bad.match(sample).hasMatch()) {
                row.setErrorMessage(tr("Invalid date format"));
                return;
            }
            row.setErrorMessage({});
            self->setControlValue(u"format"_s, v.trimmed());
        });
    });
    custom.searchable = false;
    custom.visible = [self, isCustom] { return self && (isCustom() || self->property("ocCustomFormat").toBool()); };
    n << custom;
    n << SettingDef::folder(tr("New file location"), tr("New daily notes will be placed here."), u"folder"_s, tr("Example: folder 1/folder 2"), true);
    SettingDef tmpl = SettingDef::file(tr("Template file location"), tr("Choose the file to use as a template."), u"template"_s);
    tmpl.control.filter = [](const QString& path) { return path.endsWith(u".md"_s, Qt::CaseInsensitive); };
    tmpl.control.validate = [self](const QJsonValue& v) {
        const QString p = v.toString();
        if (p.isEmpty() || !self || !self->session() || !self->session()->vault())
            return QString();
        return self->session()->vault()->file(p) ? QString() : tr("File not found.");
    };
    n << tmpl;
    return n;
}

QList<SettingDef> templatesDefs(PluginSettingsTab& tab)
{
    QPointer<PluginSettingsTab> self = &tab;
    QList<SettingDef> t;
    t << SettingDef::folder(tr("Template folder location"), tr("Files in this folder will be available as templates."), u"folder"_s,
                            tr("Example: folder 1/folder 2"), false);
    SettingDef date = SettingDef::custom(tr("Date format"), tr("{{date}} in the template file will be replaced with this value."), [self](SettingItem& row) {
        if (!self)
            return;
        const QString prefix = tr("{{date}} in the template file will be replaced with this value.").toHtmlEscaped() + u"<br>"_s
                               + tr("You can also use {{date:YYYY-MM-DD}} to override the format once.").toHtmlEscaped();
        momentFormatRow(row, prefix, self->controlValue(u"dateFormat"_s).toString(), u"YYYY-MM-DD"_s, [self](const QString& v) {
            if (self)
                self->setControlValue(u"dateFormat"_s, v.trimmed());
        });
    });
    t << date;
    SettingDef time = SettingDef::custom(tr("Time format"), tr("{{time}} in the template file will be replaced with this value."), [self](SettingItem& row) {
        if (!self)
            return;
        const QString prefix = tr("{{time}} in the template file will be replaced with this value.").toHtmlEscaped() + u"<br>"_s
                               + tr("You can also use {{time:HH:mm}} to override the format once.").toHtmlEscaped();
        momentFormatRow(row, prefix, self->controlValue(u"timeFormat"_s).toString(), u"HH:mm"_s, [self](const QString& v) {
            if (self)
                self->setControlValue(u"timeFormat"_s, v.trimmed());
        });
    });
    t << time;
    return t;
}

QList<SettingDef> uniqueNoteDefs(PluginSettingsTab& tab)
{
    QPointer<PluginSettingsTab> self = &tab;
    QList<SettingDef> t;
    t << SettingDef::folder(tr("New file location"), tr("The folder path to create the new unique note in."), u"folder"_s,
                            tr("Example: folder 1/folder 2"), true);
    SettingDef tmpl = SettingDef::file(tr("Template file location"), tr("The file path to use as template."), u"template"_s, tr("Example: folder1/note"));
    tmpl.control.validate = [self](const QJsonValue& v) {
        const QString p = v.toString();
        if (p.isEmpty() || !self || !self->session() || !self->session()->vault())
            return QString();
        return self->session()->vault()->file(p) ? QString() : tr("File not found.");
    };
    t << tmpl;
    t << SettingDef::custom(tr("Unique prefix format"), {}, [self](SettingItem& row) {
        if (!self)
            return;
        momentFormatRow(row, QString(), self->controlValue(u"format"_s).toString(), u"YYYYMMDDHHmm"_s, [self](const QString& v) {
            if (self)
                self->setControlValue(u"format"_s, v.trimmed());
        });
    });
    return t;
}

QList<SettingDef> pagePreviewDefs(PluginSettingsTab&)
{
    // Obsidian's hover link sources (registration order) and whether they need Mod by default.
    const QList<std::tuple<QString, QString, bool>> sources = {
        {u"editor"_s, tr("Editing view"), true},
        {u"preview"_s, tr("Reading view"), false},
        {u"tab-header"_s, tr("Tab header"), true},
        {u"file-explorer"_s, tr("Files"), true},
        {u"search"_s, tr("Search, Backlinks, and Outgoing links"), true},
        {u"graph"_s, tr("Graph view"), true},
        {u"outline"_s, tr("Outline"), true},
        {u"properties"_s, tr("Properties view"), true},
        {u"bases"_s, tr("Bases"), true},
    };
    QList<SettingDef> items;
    for (const auto& [id, display, defMod] : sources) {
        SettingDef d = SettingDef::toggle(display, {}, id);
        d.control.defaultValue = defMod;
        items << d;
    }
    SettingDef list = SettingDef::list(tr("Require %1 to trigger page preview on hover").arg(u"Ctrl"_s), items);
    list.search = SettingDef::Search{tr("Filter..."), {}};
    return {list};
}

QList<SettingDef> fileRecoveryDefs(PluginSettingsTab& tab)
{
    QPointer<PluginSettingsTab> self = &tab;
    auto applyOptions = [self](const QJsonValue&) {
        VaultSession* s = self ? self->session() : nullptr;
        FileRecovery* fr = s ? fileRecoveryOf(s) : nullptr;
        if (!fr || !self)
            return;
        FileRecovery::Options o = fr->options();
        o.intervalMinutes = self->controlValue(u"intervalMinutes"_s).toInt(5);
        o.keepDays = self->controlValue(u"keepDays"_s).toInt(7);
        fr->setOptions(o);
    };
    QList<SettingDef> t;
    SettingDef interval = SettingDef::number(tr("Snapshot interval"), tr("Minimal interval in minutes between two snapshots."), u"intervalMinutes"_s, 0);
    interval.control.defaultValue = 5;
    interval.control.afterChange = applyOptions;
    t << interval;
    SettingDef keep = SettingDef::number(tr("History length"), tr("Number of days the snapshots are kept for."), u"keepDays"_s, 1);
    keep.control.defaultValue = 7;
    keep.control.afterChange = applyOptions;
    t << keep;
    QPointer<SettingsModal> modal = tab.modal();
    t << SettingDef::custom(tr("Snapshots"), tr("View and restore saved snapshots."), [modal](SettingItem& row) {
        row.addButton(tr("View"), Button::Style::Normal, [modal] {
            if (modal && modal->window())
                openFileRecoveryModal(modal->window()); // on top of the settings, like Obsidian
        });
    });
    t << SettingDef::custom(tr("Clear history"), tr("Delete all snapshots."), [self, modal](SettingItem& row) {
        row.addButton(tr("Clear"), Button::Style::Destructive, [self, modal] {
            if (!modal || !modal->overlay())
                return;
            ConfirmModal::ask(modal->overlay(), tr("Clear history"),
                              tr("Are you sure you want to delete all snapshots? Files inside your vault will not be affected."),
                              {{tr("Clear"), Modal::ButtonStyle::Warning}, {tr("Cancel"), Modal::ButtonStyle::Normal}}, QString(),
                              [self, modal](int button, bool) {
                                  if (button != 0 || !self)
                                      return;
                                  VaultSession* s = self->session();
                                  if (FileRecovery* fr = s ? fileRecoveryOf(s) : nullptr)
                                      fr->clearAll();
                                  if (modal)
                                      modal->notice(tr("Local history cleared."));
                              });
        });
    });
    return t;
}

QList<SettingDef> switcherDefs(PluginSettingsTab& tab)
{
    QPointer<PluginSettingsTab> self = &tab;
    QList<SettingDef> t;
    t << SettingDef::toggle(tr("Show existing only"),
                            tr("Only show results from existing files. Links to files that are not yet created will be hidden."), u"showExistingOnly"_s);
    t << SettingDef::toggle(tr("Show attachments"), tr("Show attachment files like images, videos, and PDFs."), u"showAttachments"_s);
    SettingDef all = SettingDef::toggle(
        tr("Show all file types"),
        tr("Show all files including ones that Obsidian can't open. The file will be opened with the default app for that file type."),
        u"showAllFileTypes"_s);
    all.visible = [self] { return self && self->vaultConfig() && self->vaultConfig()->flag(u"showUnsupportedFiles"_s); };
    t << all;
    for (SettingDef& d : t)
        d.control.defaultValue = d.control.key == u"showAttachments";
    return t;
}

QList<SettingDef> backlinkDefs(PluginSettingsTab&)
{
    return {SettingDef::toggle(tr("Show backlinks at the bottom of notes"), tr("Make backlinks visible in new tabs by default."), u"backlinkInDocument"_s)};
}

QList<SettingDef> commandPaletteDefs(PluginSettingsTab& tab)
{
    QPointer<PluginSettingsTab> self = &tab;
    CommandRegistry* r = tab.commands();
    QStringList pinned;
    for (const QJsonValue& v : tab.controlValue(u"pinned"_s).toArray())
        if (v.isString())
            pinned << v.toString();
    QStringList shownIds;
    QList<SettingDef> items;
    for (const QString& id : pinned) {
        const Command* c = r ? r->find(id) : nullptr;
        if (!c)
            continue;
        SettingDef d;
        d.name = c->name;
        d.icon = c->icon.isEmpty() ? u"question-mark-glyph"_s : c->icon;
        d.searchable = false;
        items << d;
        shownIds << id;
    }
    auto store = [self](const QStringList& list) {
        if (!self)
            return;
        self->setControlValue(u"pinned"_s, list.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(QJsonArray::fromStringList(list)));
        self->update();
    };
    SettingDef list = SettingDef::list(tr("Pinned commands"), items);
    list.emptyState = tr("No commands found.");
    list.addItemName = tr("Add a command...");
    QPointer<SettingsModal> modal = tab.modal();
    list.addItem = [self, modal, pinned, store] {
        if (!self || !modal || !modal->overlay())
            return;
        CommandRegistry* reg = self->commands();
        if (!reg)
            return;
        auto* sm = new SuggestModal(modal->overlay());
        sm->setObjectName(u"ocPinCommandModal"_s);
        sm->setPlaceholder(tr("Select a command..."));
        sm->setEmptyText(tr("No commands found."));
        QList<QPair<QString, QString>> cmds; // id, name
        for (const Command* c : reg->all())
            if (!pinned.contains(c->id))
                cmds.append({c->id, c->name});
        sm->setProvider([cmds](const QString& query) {
            QList<QPair<double, Suggestion>> scored;
            const FuzzyQuery q(query);
            for (const auto& [id, name] : cmds) {
                Suggestion s;
                s.title = name;
                s.data = id;
                if (q.isEmpty()) {
                    scored.append({0, s});
                } else if (const auto m = q.match(name)) {
                    for (const MatchRange& mr : matchIndicesToRanges(m->matches))
                        s.titleMatches.append({mr.from, mr.to});
                    scored.append({m->score, s});
                }
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            QList<Suggestion> out;
            for (const auto& [score, s] : scored)
                out << s;
            return out;
        });
        QObject::connect(sm, &SuggestModal::chosen, sm, [pinned, store](const Suggestion& s, Qt::KeyboardModifiers) {
            store(pinned + QStringList{s.data.toString()});
        });
        sm->open();
    };
    list.onDelete = [pinned, shownIds, store](int index) {
        if (index < 0 || index >= shownIds.size())
            return;
        QStringList l = pinned;
        l.removeAll(shownIds[index]);
        store(l);
    };
    return {list};
}

QList<SettingDef> greentextDefs(PluginSettingsTab& tab)
{
    QPointer<PluginSettingsTab> self = &tab;
    auto apply = [self](const QJsonValue&) {
        if (self && self->config())
            GreentextGuard::instance().setSettings(GreentextGuard::Settings::fromJson(self->config()->object(self->configFile())));
    };
    const GreentextGuard::Settings defaults;
    QList<SettingDef> t;
    SettingDef patterns = SettingDef::text(tr("Note name patterns"),
                                           tr("Comma-separated parts of a note's file name (case-insensitive). Greentext Guard only acts in notes "
                                              "whose name contains one of them."),
                                           u"patterns"_s, defaults.patterns);
    patterns.control.defaultValue = defaults.patterns;
    patterns.control.afterChange = apply;
    t << patterns;
    SettingDef paste = SettingDef::toggle(tr("Wrap pasted greentext in a code fence"),
                                          tr("Pasting text with a line that starts with “>” wraps it in a code fence, so it isn't rendered as a "
                                             "blockquote."),
                                          u"onPaste"_s);
    paste.control.defaultValue = defaults.onPaste;
    paste.control.afterChange = apply;
    t << paste;
    SettingDef block = SettingDef::toggle(tr("Include the surrounding block"),
                                          tr("Also wrap the blank-line-delimited block the pasted text lands in."), u"expandToBlock"_s);
    block.control.defaultValue = defaults.expandToBlock;
    block.control.afterChange = apply;
    block.disabled = [self] { return self && !self->controlValue(u"onPaste"_s).toBool(true); };
    t << block;
    SettingDef type = SettingDef::dropdown(tr("Typing “>” at the start of a line"),
                                           tr("What happens when you type “>” at the start of a line outside a code fence."), u"onType"_s,
                                           {{u"inline"_s, tr("Insert `>` (inline code)")}, {u"escape"_s, tr("Insert \\> (escaped)")},
                                            {u"off"_s, tr("Keep it (blockquote)")}});
    type.control.defaultValue = defaults.onType;
    type.control.afterChange = apply;
    t << type;
    return t;
}

QString uniqueNoteIcon() { return IconProvider::instance().has(u"sheets-in-box"_s) ? u"sheets-in-box"_s : u"box"_s; }
} // namespace

void addSettingsTabs(SettingsModal& m)
{
    m.addTab(new GeneralTab(&m));
    m.addTab(new AppearanceTab(&m));
    m.addTab(new InterfaceTab(&m));
    m.addTab(new EditorTab(&m));
    m.addTab(new FilesTab(&m));
    m.addTab(new HotkeysTab(&m));
    m.addTab(new KeychainTab(&m));
    m.addTab(new CorePluginsTab(&m));
    m.addTab(new CommunityPluginsTab(&m));

    m.addTab(pluginTab(m, u"backlink"_s, u"backlink"_s, tr("Backlinks"), u"links-coming-in"_s, backlinkDefs));
    m.addTab(pluginTab(m, u"command-palette"_s, u"command-palette"_s, tr("Command palette"), u"terminal"_s, commandPaletteDefs));
    m.addTab(pluginTab(m, u"daily-notes"_s, u"daily-notes"_s, tr("Daily notes"), u"calendar"_s, dailyNotesDefs));
    m.addTab(pluginTab(m, u"file-recovery"_s, u"file-recovery"_s, tr("File recovery"), u"history"_s, fileRecoveryDefs));
    m.addTab(pluginTab(m, u"page-preview"_s, u"page-preview"_s, tr("Page preview"), u"scan-eye"_s, pagePreviewDefs));
    m.addTab(pluginTab(m, u"switcher"_s, u"switcher"_s, tr("Quick switcher"), u"file-search"_s, switcherDefs));
    m.addTab(pluginTab(m, u"templates"_s, u"templates"_s, tr("Templates"), u"files"_s, templatesDefs));
    m.addTab(pluginTab(m, u"zk-prefixer"_s, u"zk-prefixer"_s, tr("Unique note creator"), uniqueNoteIcon(), uniqueNoteDefs));

    m.addTab(pluginTab(m, u"greentext-guard"_s, u"plugins/greentext-guard"_s, tr("Greentext Guard"), u"quote"_s, greentextDefs,
                       SettingsTab::Section::CommunityPlugins));
}

} // namespace oc
