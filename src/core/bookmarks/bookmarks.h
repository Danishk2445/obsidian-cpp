#pragma once

// Obsidian's Bookmarks (core plugin "bookmarks"): a tree of bookmark items stored as
// {"items": [...]} in the per-vault config file bookmarks.json (the clone's config dir, never the
// vault). Item shapes follow Obsidian 1.13:
//   {"type": "file",   "ctime": ms, "path": "a/b.md", "subpath"?: "#Heading" | "#^block", "title"?}
//   {"type": "folder", "ctime": ms, "path": "a",                                           "title"?}
//   {"type": "search", "ctime": ms, "query": "tag:#x",                                     "title"?}
//   {"type": "graph",  "ctime": ms, "options": {...},                                      "title"}
//   {"type": "url",    "ctime": ms, "url": "https://...",                                  "title"}
//   {"type": "group",  "ctime": ms, "items": [...],                                        "title"}
// Keys this model doesn't know (and older types such as "heading"/"block") round-trip unchanged.

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace oc {

class VaultSession;

struct Bookmark {
    QString type;
    qint64 ctime = 0;
    QString path;
    QString subpath;             // file bookmarks: "#Heading" / "#^id" (empty: none)
    QString query;               // search bookmarks
    std::optional<QString> title; // custom title (may be empty: then the default title shows)
    QJsonObject extra;           // unknown keys (graph "options", url "url", ...)
    std::vector<std::unique_ptr<Bookmark>> items; // groups
    Bookmark* parent = nullptr;

    bool isGroup() const { return type == u"group"; }
    QJsonObject toJson() const;
    static std::unique_ptr<Bookmark> fromJson(const QJsonObject& o);

    // Obsidian's constructors (ctime = now).
    static std::unique_ptr<Bookmark> file(const QString& path, const QString& subpath = {});
    static std::unique_ptr<Bookmark> folder(const QString& path);
    static std::unique_ptr<Bookmark> search(const QString& query);
    static std::unique_ptr<Bookmark> group(const QString& title);
};

// The bookmark list of a vault. Items are owned by the model; pointers stay valid until the item is
// removed (moves keep them).
class BookmarkModel : public QObject {
    Q_OBJECT
public:
    explicit BookmarkModel(QObject* parent = nullptr);
    ~BookmarkModel() override;

    // Loads {"items": [...]}; returns false (and keeps an empty list) when the shape is invalid.
    bool load(const QJsonObject& json);
    QJsonObject toJson() const;

    const std::vector<std::unique_ptr<Bookmark>>& items() const { return m_items; }
    const std::vector<std::unique_ptr<Bookmark>>& itemsOf(const Bookmark* group) const { return group ? group->items : m_items; }
    // Every item, depth first (Obsidian's Q4 walk).
    QList<Bookmark*> all() const;
    int indexOf(const Bookmark* item) const;
    bool contains(const Bookmark* item) const;

    Bookmark* add(std::unique_ptr<Bookmark> item, Bookmark* group = nullptr);
    void remove(Bookmark* item);
    // Obsidian's moveItem(item, group, index): index in the destination list as it was before the item
    // was taken out (adjusted when moving down within the same list).
    void move(Bookmark* item, Bookmark* group, int index);
    // A property of `item` changed (title, group): saves.
    void edited(Bookmark* item);

    // Obsidian's bookmarkLookup: the folder / whole-file (no subpath) bookmark of a path.
    Bookmark* findByPath(const QString& path) const;
    // The vault renamed `oldPath` (a file or folder, its descendants included) to `newPath`.
    void renamePath(const QString& oldPath, const QString& newPath);

    // Obsidian's getItemTitle(): the custom title, else file basename (+ subpath) / folder name /
    // search query. shortName(path) gives a file's display name (its basename for notes).
    static QString title(const Bookmark& item, const std::function<QString(const QString& path)>& shortName = {});

signals:
    void changed(); // items added / removed / moved / edited (the owner saves)

private:
    std::vector<std::unique_ptr<Bookmark>>& listOf(Bookmark* group) { return group ? group->items : m_items; }
    std::vector<std::unique_ptr<Bookmark>> m_items;
};

// The session's bookmarks, loaded from and saved to the config file "bookmarks" (created as a child of
// the session on first use).
BookmarkModel* sessionBookmarks(VaultSession* session);

} // namespace oc
