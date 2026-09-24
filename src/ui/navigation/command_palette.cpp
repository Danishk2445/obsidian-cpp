#include "ui/navigation/command_palette.h"

#include "core/commands/command_registry.h"
#include "core/config/clone_paths.h"
#include "core/config/config_store.h"
#include "core/config/json_file.h"
#include "core/fuzzy/fuzzy.h"
#include "core/vault_session.h"
#include "ui/navigation/navigation_util.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"

#include <QApplication>
#include <QCoreApplication>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace oc {

namespace {

QString tr(const char* s) { return QCoreApplication::translate("oc::CommandPalette", s); }

struct Candidate {
    QString id;
    QString name;
};

QList<Candidate> availableCommands(const CommandRegistry& registry)
{
    QList<Candidate> out;
    for (const Command* c : registry.all()) // sorted by name
        if (!c->name.isEmpty() && registry.isAvailable(c->id))
            out.append({c->id, c->name});
    return out;
}

QList<PaletteItem> itemsFrom(const QString& rawQuery, const QList<Candidate>& available, const QStringList& recent,
                             const CommandPaletteOptions& options)
{
    const QSet<QString> pinned(options.pinned.cbegin(), options.pinned.cend());
    QList<PaletteItem> out;
    auto make = [&](const Candidate& c) {
        PaletteItem it;
        it.id = c.id;
        it.name = c.name;
        it.pinned = pinned.contains(c.id);
        return it;
    };
    const QString query = rawQuery.trimmed();
    if (query.isEmpty()) {
        QHash<QString, qsizetype> byId;
        for (qsizetype i = 0; i < available.size(); ++i)
            byId.insert(available[i].id, i);
        QSet<QString> used;
        for (const QString& id : options.pinned)
            if (byId.contains(id) && !used.contains(id)) {
                used.insert(id);
                out.append(make(available[byId.value(id)]));
            }
        for (const QString& id : recent)
            if (byId.contains(id) && !used.contains(id)) {
                used.insert(id);
                out.append(make(available[byId.value(id)]));
            }
        for (const Candidate& c : available)
            if (!used.contains(c.id))
                out.append(make(c));
        return out;
    }
    const FuzzyQuery fuzzy(query);
    for (const Candidate& c : available) {
        if (const std::optional<FuzzyResult> r = fuzzy.match(c.name)) {
            PaletteItem it = make(c);
            it.score = r->score;
            it.matches = matchIndicesToRanges(r->matches);
            out.append(it);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const PaletteItem& a, const PaletteItem& b) { return a.score > b.score; });
    return out;
}

Suggestion toSuggestion(const PaletteItem& it, const CommandRegistry* registry)
{
    Suggestion s;
    // "Plugin: Name" -> .suggestion-prefix "Plugin" + title "Name"; highlights stay relative to the whole name
    // (SuggestionList shifts title matches by the prefix length again).
    const qsizetype colon = it.name.indexOf(QStringLiteral(": "));
    int shift = 0;
    if (colon > 0) {
        s.prefix = it.name.left(colon);
        s.title = it.name.mid(colon + 2);
        shift = int(colon) + 2;
    } else {
        s.title = it.name;
    }
    for (const MatchRange& m : it.matches)
        s.titleMatches.append({m.from - shift, m.to - shift});
    if (registry)
        for (const Hotkey& hk : registry->hotkeys(it.id))
            if (hk.isValid())
                s.hotkeys << hk.toString();
    if (it.pinned)
        s.auxIcon = QStringLiteral("pin");
    s.data = it.id;
    return s;
}

} // namespace

CommandPaletteOptions CommandPaletteOptions::fromJson(const QJsonObject& json)
{
    CommandPaletteOptions o;
    for (const QJsonValue& v : json.value(QStringLiteral("pinned")).toArray())
        if (v.isString())
            o.pinned << v.toString();
    return o;
}

QJsonObject CommandPaletteOptions::toJson() const
{
    return QJsonObject{{QStringLiteral("pinned"), QJsonArray::fromStringList(pinned)}};
}

QList<PaletteItem> commandPaletteItems(const QString& query, const CommandRegistry& registry, const CommandPaletteOptions& options)
{
    return itemsFrom(query, availableCommands(registry), registry.recent(), options);
}

SuggestModal* openCommandPalette(VaultSession* session, OverlayLayer* layer)
{
    CommandRegistry* registry = session ? session->commands() : nullptr;
    if (!layer || !registry)
        return nullptr;
    if (auto* open = layer->findChild<SuggestModal*>(QStringLiteral("ocCommandPalette"), Qt::FindDirectChildrenOnly); open && open->isOpen())
        return open;
    const CommandPaletteOptions options =
        CommandPaletteOptions::fromJson(session->config() ? session->config()->object(QStringLiteral("command-palette")) : QJsonObject());
    // Availability is evaluated once, for the context the palette was opened from.
    const QList<Candidate> available = availableCommands(*registry);
    const QStringList recent = registry->recent();
    QPointer<CommandRegistry> reg = registry;
    QPointer<QWidget> previousFocus = QApplication::focusWidget();

    auto* modal = new SuggestModal(layer);
    modal->setObjectName(QStringLiteral("ocCommandPalette"));
    containKeyEvents(modal);
    modal->setPlaceholder(tr("Select a command..."));
    modal->setEmptyText(tr("No commands found."));
    modal->setInstructions({{QStringLiteral("↑↓"), tr("to navigate")}, {QStringLiteral("↵"), tr("to use")}, {QStringLiteral("esc"), tr("to dismiss")}});
    modal->setLimit(0); // every command can be scrolled to
    modal->setProvider([available, recent, options, reg](const QString& q) {
        QList<Suggestion> out;
        for (const PaletteItem& it : itemsFrom(q, available, recent, options))
            out.append(toSuggestion(it, reg));
        return out;
    });
    QObject::connect(modal, &SuggestModal::closed, modal, [previousFocus] {
        if (previousFocus)
            previousFocus->setFocus(Qt::OtherFocusReason);
    });
    QObject::connect(modal, &SuggestModal::chosen, modal, [reg](const Suggestion& s, Qt::KeyboardModifiers) {
        if (reg)
            reg->execute(s.data.toString());
    });
    modal->open();
    return modal;
}

// ---------------------------------------------------------------------------------------------
// recent commands persistence
// ---------------------------------------------------------------------------------------------

namespace {
class RecentCommandsStore : public QObject {
public:
    RecentCommandsStore(CommandRegistry* registry, QString file, QObject* parent)
        : QObject(parent)
        , m_registry(registry)
        , m_file(std::move(file))
    {
        setObjectName(QStringLiteral("ocRecentCommands"));
        QStringList ids;
        for (const QJsonValue& v : JsonFile::read(m_file).value(QStringLiteral("recent")).toArray())
            if (v.isString())
                ids << v.toString();
        if (!ids.isEmpty() && registry->recent().isEmpty())
            registry->setRecent(ids);
        m_timer.setSingleShot(true);
        m_timer.setInterval(1000);
        connect(&m_timer, &QTimer::timeout, this, &RecentCommandsStore::save);
        connect(registry, &CommandRegistry::executed, this, [this] { m_timer.start(); });
    }
    ~RecentCommandsStore() override
    {
        if (m_timer.isActive())
            save();
    }
    void save()
    {
        m_timer.stop();
        if (m_registry)
            JsonFile::write(m_file, QJsonObject{{QStringLiteral("recent"), QJsonArray::fromStringList(m_registry->recent())}});
    }

private:
    QPointer<CommandRegistry> m_registry;
    QString m_file;
    QTimer m_timer;
};
} // namespace

void persistRecentCommands(CommandRegistry& registry, VaultSession* session)
{
    if (!session || session->findChild<QObject*>(QStringLiteral("ocRecentCommands"), Qt::FindDirectChildrenOnly))
        return;
    const QString file = ClonePaths::vaultDataDir(session->id()) + QStringLiteral("/recent-commands.json");
    new RecentCommandsStore(&registry, file, session);
}

} // namespace oc
