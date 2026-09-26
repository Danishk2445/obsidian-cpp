#pragma once

// Obsidian's "bases" view (MZ, a TextFileView for .base files): the base's controller filling the
// tab. The file is read when opened and re-read when it changes on disk; edits made in the UI
// (views, sort, filters, properties, column widths...) are written back as Obsidian serializes the
// base, only when that text differs from the file — opening a base never rewrites it. The view
// state carries the selected view ("viewName"); the "#View name" subpath of a link selects a view.
// "this" in formulas is the base file itself, then follows the files opened in the workspace
// (like Obsidian), unless the tab is pinned.

#include "ui/workspace/view.h"

#include <QTimer>

namespace oc::basesui {

class BasesController;

class BasesFileView : public FileView {
    Q_OBJECT
public:
    explicit BasesFileView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~BasesFileView() override;

    QString viewType() const override { return QStringLiteral("bases"); }
    QString icon() const override;
    bool canAcceptExtension(const QString& extension) const override;
    void loadFile(TFile* file) override;
    void onRename(TFile* file, const QString& oldPath) override;
    void onClose() override;
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    QJsonObject ephemeralState() const override;
    void setEphemeralState(const QJsonObject& eState) override;
    void focusContent() override;

    BasesController* controller() const { return m_controller; }
    // Write pending changes now (the view writes 500 ms after the last change).
    bool save();
    bool hasPendingSave() const { return m_saveTimer.isActive(); }
    QString fileText() const { return m_lastText; }

private:
    void reload();
    void onEdited();

    BasesController* m_controller;
    QString m_lastText;   // the file's text as last read or written
    QString m_pending;    // text to write
    QTimer m_saveTimer;
    QMetaObject::Connection m_modifiedConn;
    QMetaObject::Connection m_fileOpenedConn;
    QString m_pendingView;
};

} // namespace oc::basesui
