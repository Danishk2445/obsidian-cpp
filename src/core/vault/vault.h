#pragma once

// FROZEN INTERFACE (M1) — the vault: an in-memory model of a folder of notes + attachments, with
// file operations, byte-exact compare-and-swap writes, external change detection (inotify, with a
// polling safety net on FUSE filesystems) and offline handling for removable drives.
//
// Paths are vault-relative, '/'-separated, case-preserving, without leading '/'; the root folder's
// path is "". Every file or folder whose name starts with '.' (".obsidian", ".git", ".trash", ...) is
// ignored entirely, as are temp files of atomic savers (e.g. "x.md.tmp.<pid>.<hex>", "*.swp", "*~",
// "4913", our own ".<name>.oc~*" temps).

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace oc {

class TFolder;
class Vault;

class TAbstractFile {
public:
    virtual ~TAbstractFile() = default;
    const QString& path() const { return m_path; }
    const QString& name() const { return m_name; }
    TFolder* parent() const { return m_parent; }
    virtual bool isFolder() const = 0;

protected:
    friend class Vault;
    QString m_path;
    QString m_name;
    TFolder* m_parent = nullptr;
};

class TFile : public TAbstractFile {
public:
    bool isFolder() const override { return false; }
    const QString& basename() const { return m_basename; }   // name without the last extension
    const QString& extension() const { return m_extension; } // without dot, case preserved ("" if none)
    bool isMarkdown() const { return m_extension.compare(QLatin1String("md"), Qt::CaseInsensitive) == 0; }
    qint64 size() const { return m_size; }
    qint64 mtimeMs() const { return m_mtimeMs; }
    qint64 ctimeMs() const { return m_ctimeMs; } // birth time when available, else mtime

private:
    friend class Vault;
    QString m_basename;
    QString m_extension;
    qint64 m_size = 0;
    qint64 m_mtimeMs = 0;
    qint64 m_ctimeMs = 0;
};

class TFolder : public TAbstractFile {
public:
    bool isFolder() const override { return true; }
    bool isRoot() const { return m_path.isEmpty(); }
    const QList<TAbstractFile*>& children() const { return m_children; } // unsorted

private:
    friend class Vault;
    QList<TAbstractFile*> m_children;
};

// On-disk identity of a file's content, used for compare-and-swap writes and self-write suppression.
struct DiskState {
    qint64 size = -1;     // -1 = does not exist
    qint64 mtimeNs = 0;
    QByteArray hash;      // contentHash() of the bytes (32 hex chars), may be empty if unknown
    bool exists() const { return size >= 0; }
    bool sameAs(const DiskState& o) const { return size == o.size && mtimeNs == o.mtimeNs && (hash.isEmpty() || o.hash.isEmpty() || hash == o.hash); }
};

// Storage abstraction used by Document (implemented by Vault; tests may use a plain directory).
class DocumentStorage {
public:
    virtual ~DocumentStorage() = default;
    virtual std::optional<QByteArray> read(const QString& path, DiskState* state, QString* error) = 0;
    enum class WriteResult { Ok, Conflict, Error };
    // Writes atomically unless `expected` doesn't match the file on disk (then Conflict, nothing written).
    // expected.size == -1 means "must not exist" (create).
    virtual WriteResult write(const QString& path, const QByteArray& bytes, const DiskState& expected, DiskState* newState,
                              QString* error) = 0;
    virtual DiskState stat(const QString& path) = 0;
};

class Vault : public QObject, public DocumentStorage {
    Q_OBJECT
public:
    enum class TrashMode { System, Local, Permanent }; // system trash (KIO), <vault>/.trash, unlink

    explicit Vault(QString rootDir, QObject* parent = nullptr);
    ~Vault() override;

    const QString& root() const;                       // absolute, no trailing slash
    QString name() const;                              // folder name
    QString absolutePath(const QString& vaultPath) const;
    std::optional<QString> vaultPath(const QString& absolutePath) const; // nullopt if outside the vault

    // Initial scan (synchronous) + start watching. Emits loaded().
    void load();
    bool isLoaded() const;
    bool isOnline() const; // false while the vault's folder is unavailable (e.g. drive unmounted)

    TFolder* rootFolder() const;
    TAbstractFile* item(const QString& path) const;      // exact (case-sensitive) lookup
    TFile* file(const QString& path) const;
    TFolder* folder(const QString& path) const;
    QList<TFile*> files() const;
    QList<TFile*> markdownFiles() const;
    QList<TFolder*> folders() const;
    int fileCount() const;

    // DocumentStorage. read() is GUI-thread only (metadata workers read files directly). write() returns Ok
    // without touching the file when the bytes are identical. While offline, stat() reports the last
    // known state.
    std::optional<QByteArray> read(const QString& path, DiskState* state, QString* error) override;
    WriteResult write(const QString& path, const QByteArray& bytes, const DiskState& expected, DiskState* newState,
                      QString* error) override;
    DiskState stat(const QString& path) override;

    // File operations. All emit the corresponding signal synchronously (external = false) and the
    // watcher suppresses their echo. Parent folders are created as needed. Folder operations emit one
    // signal per item: renamed() for the folder first, then each descendant; deleted() deepest first.
    TFile* createFile(const QString& path, const QByteArray& bytes, QString* error = nullptr);
    TFolder* createFolder(const QString& path, QString* error = nullptr); // an existing folder is returned as is
    bool rename(TAbstractFile* item, const QString& newPath, QString* error = nullptr); // rename and/or move
    bool trash(TAbstractFile* item, TrashMode mode, QString* error = nullptr);
    TFile* copy(TFile* file, const QString& newPath, QString* error = nullptr);

    // "Untitled.md" -> "Untitled 1.md" -> "Untitled 2.md" ... (folderPath "" = root; ext without dot)
    QString availablePath(const QString& folderPath, const QString& baseName, const QString& ext) const;
    static bool isIgnoredName(const QString& name); // dot files and temp patterns

    // Re-stat everything (e.g. on window focus); asynchronous: the differences arrive as signals later.
    void rescan();

signals:
    void loaded(); // also emitted when the vault folder is unavailable (then the vault starts offline)
    void created(oc::TAbstractFile* item, bool external);
    void modified(oc::TFile* file, bool external);            // content changed on disk
    void deleted(oc::TAbstractFile* item, bool external);     // object is destroyed after handlers return
    void renamed(oc::TAbstractFile* item, const QString& oldPath, bool external);
    void onlineChanged(bool online);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    friend struct VaultImplAccess;
};

} // namespace oc
