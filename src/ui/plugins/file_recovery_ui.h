#pragma once

#include "core/recovery/snapshot_store.h"

#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace oc {

class CommandRegistry;
class FileRecovery;
class MainWindow;
class Modal;
class RecoveryDiffView;
class RecoveryTitle;
class RecoveryVersionList;
class SearchInput;
class SuggestPopup;
class ToggleSwitch;
class VaultSession;
struct RecoveryDiffRow;

// The File recovery core plugin's UI: Obsidian's "Local history" modal (.mod-sync-history.
// mod-sidebar-layout). The sidebar has a "Choose a file..." input suggesting every file that has
// snapshots and, below it, the chosen file's snapshots (newest first: moment "llll" time and size).
// The right side shows the selected snapshot as text or, with "Show changes", as its diff against the
// previous snapshot; "Copy" puts it on the clipboard and "Restore" writes its exact bytes back to the
// vault (creating the folder if needed; an unsaved open editor is saved first so nothing is lost) and
// snapshots them again. `path` preselects a file (the command passes the active markdown file).
Modal* openFileRecoveryModal(MainWindow* window, const QString& path = {});
FileRecovery* fileRecoveryOf(VaultSession* session);

// "file-recovery:open" ("Open local history"), when the core plugin is enabled.
void registerFileRecoveryCommands(CommandRegistry& registry, MainWindow* window);

// Obsidian's diff view as rows: equal lines, removed ("left", red) and added ("right", green) lines —
// pairs of similar lines carry word-level [start, end) highlights — and folded runs of equal lines
// (more than 5 at the start or end, more than 10 in between; 3 lines of context stay visible).
struct RecoveryDiffRow {
    enum Kind { Equal, Left, Right, Folded };
    Kind kind = Equal;
    QString text;                   // empty for Folded
    QList<QPair<int, int>> changed; // Left/Right: highlighted ranges
    QList<RecoveryDiffRow> folded;  // Folded: the hidden rows ("N lines folded")
};
QList<RecoveryDiffRow> recoveryDiff(const QString& before, const QString& after);

// Obsidian's byte size text: "0 B", "512 B", "1.50 KB", "1,023.99 KB", "12.00 MB".
QString formatByteSize(qint64 bytes);

// The modal's content (sidebar + snapshot pane).
class FileRecoveryPanel : public QWidget {
    Q_OBJECT
public:
    FileRecoveryPanel(MainWindow* window, FileRecovery* recovery, Modal* modal);
    ~FileRecoveryPanel() override;

    QStringList paths() const { return m_paths; } // files with snapshots, natural order
    // Choose a file: the input shows it, its snapshots are listed and the newest is selected.
    void switchToPath(const QString& path);
    QString currentPath() const { return m_path; }
    const QList<SnapshotStore::Info>& versions() const { return m_versions; }
    void selectVersion(int index);
    int selectedVersion() const { return m_selected; }
    QString selectedText() const { return m_text; }
    QString title() const;

    void setShowChanges(bool on);
    bool showChanges() const;
    QList<RecoveryDiffRow> diffRows() const;
    void copy();
    bool restore(QString* error = nullptr);

    SearchInput* input() const { return m_input; }
    SuggestPopup* suggestPopup() const;
    void focusInput();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void updateSuggestions();
    void chooseSuggestion(int index);
    void hidePopup();
    void loadVersions(const QString& path);
    void openHistory();
    void applyToggle();
    void notice(const QString& text);

    QPointer<MainWindow> m_window;
    QPointer<FileRecovery> m_recovery;
    Modal* m_modal;
    QStringList m_paths;
    QStringList m_suggested;
    QString m_path;
    QList<SnapshotStore::Info> m_versions;
    int m_selected = -1;
    QByteArray m_bytes;
    QString m_text;
    bool m_diffBuilt = false;

    QWidget* m_sidebar = nullptr;
    SearchInput* m_input = nullptr;
    RecoveryVersionList* m_list = nullptr;
    QLabel* m_empty = nullptr;
    QLabel* m_noHistory = nullptr;
    QWidget* m_content = nullptr;
    RecoveryTitle* m_title = nullptr;
    ToggleSwitch* m_diffToggle = nullptr;
    QPushButton* m_copyButton = nullptr;
    QPushButton* m_restoreButton = nullptr;
    QPlainTextEdit* m_textView = nullptr;
    RecoveryDiffView* m_diffView = nullptr;
    QPointer<SuggestPopup> m_popup;
};

} // namespace oc
