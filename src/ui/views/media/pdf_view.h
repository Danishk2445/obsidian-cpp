#pragma once

// Obsidian's PDF view ("pdf", lucide-file-text): the PDF.js viewer over the whole leaf. View state
// {"file", "page", "zoom"} (page 1-based, zoom a PDF.js scale value — kept across restarts, unlike
// Obsidian which always opens at the top); eState "subpath" ("#page=3", from openLinkText) and
// "page". The view reloads when the file changes on disk; Ctrl+F opens the find bar; arrow
// left / right turn pages while a fit-to preset is active.

#include "ui/workspace/view.h"

namespace oc::media {

class PdfViewer;

class PdfView : public FileView {
    Q_OBJECT
public:
    explicit PdfView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~PdfView() override;

    QString viewType() const override { return QStringLiteral("pdf"); }
    QString icon() const override { return QStringLiteral("file-text"); }
    bool canAcceptExtension(const QString& extension) const override;
    void loadFile(TFile* file) override;
    void onRename(TFile* file, const QString& oldPath) override;

    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    QJsonObject ephemeralState() const override;
    void setEphemeralState(const QJsonObject& eState) override;
    void populatePaneMenu(QMenu* menu) override;
    void focusContent() override;

    PdfViewer* viewer() const { return m_viewer; }
    void showSearch(); // editor:open-search equivalent (Ctrl+F)

private:
    PdfViewer* m_viewer;
    QMetaObject::Connection m_modifiedConn;
    bool m_restoring = false;
};

} // namespace oc::media
