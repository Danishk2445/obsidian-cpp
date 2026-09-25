#pragma once

#include "core/canvas/canvas_model.h"
#include "core/vault/vault.h"
#include "ui/workspace/view.h"

#include <QTimer>

#include <optional>

namespace oc {

class CanvasWidget;
class ViewRegistry;

// Obsidian's canvas view (view type "canvas") for .canvas files: a CanvasWidget over the file's
// JSON Canvas data. Saving follows Obsidian's TextFileView (2 s after an edit, and on close/switch),
// byte-stable (see canvas_model.h) and never when nothing was edited. Changes of the file on disk are
// reloaded (keeping the viewport and selection). View state: {"file", "viewState": {x, y, zoom}};
// ephemeral state: {"focus"}, {"match": {"nodeId"}} (select and show a card).
class CanvasView : public FileView {
    Q_OBJECT
public:
    explicit CanvasView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~CanvasView() override;

    QString viewType() const override { return QStringLiteral("canvas"); }
    QString icon() const override { return QStringLiteral("layout-dashboard"); }
    bool canAcceptExtension(const QString& extension) const override;
    void loadFile(TFile* file) override;
    void onRename(TFile* file, const QString& oldPath) override;

    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    QJsonObject ephemeralState() const override;
    void setEphemeralState(const QJsonObject& eState) override;
    void populatePaneMenu(QMenu* menu) override;
    void onClose() override;
    void focusContent() override;

    CanvasWidget* canvas() const { return m_canvas; }
    bool save();          // now; a no-op when nothing was edited or the bytes would not change
    void requestSave();   // Obsidian's requestSave(): 2 s debounce
    bool isDirty() const { return m_dirty; }
    bool isInvalid() const { return m_invalid; } // the file could not be parsed (shown read-only, never saved)
    // Renders the whole canvas (Obsidian's "Export as image"), at `zoom` (log2, at most 1).
    QImage renderImage(double zoom = 0) const;

signals:
    void saved();

private:
    void reloadFromDisk();
    void onVaultModified(TFile* file);
    void applyLocalData();
    void saveLocalData();

    CanvasWidget* m_canvas;
    QByteArray m_bytes;          // what we last read or wrote
    DiskState m_disk;
    canvas::FileFormat m_format;
    canvas::CanvasData m_saved;  // data equal to m_bytes
    bool m_dirty = false;
    bool m_invalid = false;
    QTimer m_saveTimer;
    std::optional<QJsonObject> m_pendingViewState;
    QMetaObject::Connection m_vaultConn;
    QMetaObject::Connection m_configConn;
};

} // namespace oc
