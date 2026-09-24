#include "core/bookmarks/bookmarks.h"

#include "core/config/config_store.h"
#include "core/config/json_file.h"
#include "core/vault_session.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>

#include <algorithm>

namespace oc {

namespace {
const QStringList kKnownKeys{QStringLiteral("type"),  QStringLiteral("ctime"), QStringLiteral("path"), QStringLiteral("subpath"),
                             QStringLiteral("query"), QStringLiteral("title"), QStringLiteral("items")};

std::unique_ptr<Bookmark> make(const QString& type)
{
    auto b = std::make_unique<Bookmark>();
    b->type = type;
    b->ctime = QDateTime::currentMSecsSinceEpoch();
    return b;
}

QString baseName(const QString& path)
{
    const QString name = path.mid(path.lastIndexOf(u'/') + 1);
    const qsizetype dot = name.lastIndexOf(u'.');
    return dot > 0 ? name.left(dot) : name;
}
} // namespace

QJsonObject Bookmark::toJson() const
{
    QJsonObject o = extra;
    o.insert(QStringLiteral("type"), type);
    o.insert(QStringLiteral("ctime"), QJsonValue(ctime));
    if (type == u"file" || type == u"folder" || !path.isEmpty())
        o.insert(QStringLiteral("path"), path);
    if (!subpath.isEmpty())
        o.insert(QStringLiteral("subpath"), subpath);
    if (type == u"search" || !query.isEmpty())
        o.insert(QStringLiteral("query"), query);
    if (title)
        o.insert(QStringLiteral("title"), *title);
    if (isGroup()) {
        QJsonArray arr;
        for (const auto& c : items)
            arr.append(c->toJson());
        o.insert(QStringLiteral("items"), arr);
    }
    return o;
}

std::unique_ptr<Bookmark> Bookmark::fromJson(const QJsonObject& o)
{
    auto b = std::make_unique<Bookmark>();
    b->type = o.value(QStringLiteral("type")).toString();
    b->ctime = o.value(QStringLiteral("ctime")).toInteger(qint64(o.value(QStringLiteral("ctime")).toDouble()));
    b->path = o.value(QStringLiteral("path")).toString();
    b->subpath = o.value(QStringLiteral("subpath")).toString();
    b->query = o.value(QStringLiteral("query")).toString();
    if (o.value(QStringLiteral("title")).isString())
        b->title = o.value(QStringLiteral("title")).toString();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!kKnownKeys.contains(it.key()))
            b->extra.insert(it.key(), it.value());
    if (b->isGroup()) {
        for (const QJsonValue& v : o.value(QStringLiteral("items")).toArray()) {
            if (!v.isObject())
                continue;
            auto c = fromJson(v.toObject());
            c->parent = b.get();
            b->items.push_back(std::move(c));
        }
    } else if (o.contains(QStringLiteral("items"))) {
        b->extra.insert(QStringLiteral("items"), o.value(QStringLiteral("items")));
    }
    return b;
}

std::unique_ptr<Bookmark> Bookmark::file(const QString& path, const QString& subpath)
{
    auto b = make(QStringLiteral("file"));
    b->path = path;
    b->subpath = subpath;
    return b;
}

std::unique_ptr<Bookmark> Bookmark::folder(const QString& path)
{
    auto b = make(QStringLiteral("folder"));
    b->path = path;
    return b;
}

std::unique_ptr<Bookmark> Bookmark::search(const QString& query)
{
    auto b = make(QStringLiteral("search"));
    b->query = query;
    return b;
}

std::unique_ptr<Bookmark> Bookmark::group(const QString& title)
{
    auto b = make(QStringLiteral("group"));
    b->title = title;
    return b;
}

// ---------------------------------------------------------------------------------------------

BookmarkModel::BookmarkModel(QObject* parent)
    : QObject(parent)
{
}

BookmarkModel::~BookmarkModel() = default;

bool BookmarkModel::load(const QJsonObject& json)
{
    m_items.clear();
    const QJsonValue items = json.value(QStringLiteral("items"));
    if (!items.isUndefined() && !items.isArray())
        return false;
    for (const QJsonValue& v : items.toArray()) {
        if (!v.isObject())
            continue;
        m_items.push_back(Bookmark::fromJson(v.toObject()));
    }
    return true;
}

QJsonObject BookmarkModel::toJson() const
{
    QJsonArray arr;
    for (const auto& b : m_items)
        arr.append(b->toJson());
    return QJsonObject{{QStringLiteral("items"), arr}};
}

QList<Bookmark*> BookmarkModel::all() const
{
    QList<Bookmark*> out;
    std::function<void(const std::vector<std::unique_ptr<Bookmark>>&)> walk = [&](const std::vector<std::unique_ptr<Bookmark>>& list) {
        for (const auto& b : list) {
            out << b.get();
            if (b->isGroup())
                walk(b->items);
        }
    };
    walk(m_items);
    return out;
}

int BookmarkModel::indexOf(const Bookmark* item) const
{
    if (!item)
        return -1;
    const auto& list = itemsOf(item->parent);
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].get() == item)
            return int(i);
    return -1;
}

bool BookmarkModel::contains(const Bookmark* item) const { return item && all().contains(const_cast<Bookmark*>(item)); }

Bookmark* BookmarkModel::add(std::unique_ptr<Bookmark> item, Bookmark* group)
{
    if (!item)
        return nullptr;
    if (group && !group->isGroup())
        group = nullptr;
    Bookmark* raw = item.get();
    raw->parent = group;
    listOf(group).push_back(std::move(item));
    emit changed();
    return raw;
}

void BookmarkModel::remove(Bookmark* item)
{
    const int i = indexOf(item);
    if (i < 0)
        return;
    auto& list = listOf(item->parent);
    list.erase(list.begin() + i); // deletes the item (and a group's children)
    emit changed();
}

void BookmarkModel::move(Bookmark* item, Bookmark* group, int index)
{
    const int from = indexOf(item);
    if (from < 0)
        return;
    if (group && !group->isGroup())
        group = nullptr;
    for (const Bookmark* p = group; p; p = p->parent)
        if (p == item)
            return; // into itself
    auto& src = listOf(item->parent);
    std::unique_ptr<Bookmark> taken = std::move(src[size_t(from)]);
    src.erase(src.begin() + from);
    auto& dst = listOf(group);
    if (&src == &dst && from < index)
        --index;
    index = std::clamp(index, 0, int(dst.size()));
    taken->parent = group;
    dst.insert(dst.begin() + index, std::move(taken));
    emit changed();
}

void BookmarkModel::edited(Bookmark*) { emit changed(); }

Bookmark* BookmarkModel::findByPath(const QString& path) const
{
    for (Bookmark* b : all())
        if ((b->type == u"folder" || (b->type == u"file" && b->subpath.isEmpty())) && b->path == path)
            return b;
    return nullptr;
}

void BookmarkModel::renamePath(const QString& oldPath, const QString& newPath)
{
    bool any = false;
    for (Bookmark* b : all()) {
        if (b->type != u"file" && b->type != u"folder")
            continue;
        if (b->path == oldPath) {
            b->path = newPath;
            any = true;
        } else if (b->path.startsWith(oldPath + QLatin1Char('/'))) {
            b->path = newPath + b->path.mid(oldPath.size());
            any = true;
        }
    }
    if (any)
        emit changed();
}

QString BookmarkModel::title(const Bookmark& item, const std::function<QString(const QString&)>& shortName)
{
    if (item.title && !item.title->isEmpty())
        return *item.title;
    if (item.type == u"file" && !item.subpath.isEmpty())
        return baseName(item.path) + item.subpath;
    if (item.type == u"file" || item.type == u"folder") {
        const QString s = shortName ? shortName(item.path) : QString();
        if (!s.isEmpty())
            return s;
        if (item.type == u"folder")
            return item.path.mid(item.path.lastIndexOf(u'/') + 1);
        return baseName(item.path);
    }
    if (item.type == u"search")
        return item.query;
    return item.title.value_or(QString());
}

// ---------------------------------------------------------------------------------------------

namespace {
// Keeps the session's bookmarks in sync with bookmarks.json. A file that can't be parsed is left alone
// (Obsidian's "Failed to load bookmarks." state) until the user changes something.
class SessionBookmarks : public BookmarkModel {
public:
    explicit SessionBookmarks(VaultSession* session)
        : BookmarkModel(session)
        , m_session(session)
    {
        setObjectName(QStringLiteral("ocBookmarks"));
        ConfigStore* cfg = session->config();
        QString err;
        const QString file = cfg ? cfg->dir() + QStringLiteral("/bookmarks.json") : QString();
        if (!file.isEmpty() && QFileInfo::exists(file)) {
            JsonFile::read(file, &err);
            m_valid = err.isEmpty();
        }
        if (m_valid && cfg)
            m_valid = load(cfg->object(QStringLiteral("bookmarks")));
        setProperty("ocValid", m_valid);
        connect(this, &BookmarkModel::changed, this, [this] {
            if (m_session && m_session->config()) {
                m_session->config()->setObject(QStringLiteral("bookmarks"), toJson());
                m_valid = true;
                setProperty("ocValid", true);
            }
        });
    }

private:
    QPointer<VaultSession> m_session;
    bool m_valid = true;
};
} // namespace

BookmarkModel* sessionBookmarks(VaultSession* session)
{
    if (!session)
        return nullptr;
    if (auto* existing = session->findChild<BookmarkModel*>(QStringLiteral("ocBookmarks"), Qt::FindDirectChildrenOnly))
        return existing;
    return new SessionBookmarks(session);
}

} // namespace oc
