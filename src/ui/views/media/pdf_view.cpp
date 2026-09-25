#include "ui/views/media/pdf_view.h"

#include "core/files/file_manager.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/views/media/media_views.h"
#include "ui/views/media/pdf_viewer.h"
#include "ui/workspace/file_menu.h"

#include <QJsonObject>
#include <QPointer>
#include <QVBoxLayout>

namespace oc::media {

PdfView::PdfView(WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_viewer(new PdfViewer(PdfViewer::Mode::View, this))
{
    setObjectName(QStringLiteral("ocPdfView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0); // .workspace-leaf-content[data-type="pdf"] .view-content { padding: 0 }
    v->addWidget(m_viewer);
    setFocusProxy(m_viewer->pages());
    connect(m_viewer, &PdfViewer::stateChanged, this, [this] {
        if (!m_restoring)
            emit stateChanged();
    });
    m_viewer->pageLink = [this](int page) -> QString {
        VaultSession* s = session();
        if (!m_file || !s || !s->fileManager())
            return {};
        // Obsidian: "[[doc.pdf#page=3|doc, page 3]]"
        return s->fileManager()->generateMarkdownLink(m_file->path(), QString(), QStringLiteral("#page=%1").arg(page + 1),
                                                       tr("%1, page %2").arg(m_file->basename()).arg(page + 1));
    };
}

PdfView::~PdfView() = default;

bool PdfView::canAcceptExtension(const QString& extension) const { return pdfExtensions().contains(extension.toLower()); }

void PdfView::loadFile(TFile* file)
{
    m_file = file;
    VaultSession* s = session();
    if (s && s->config())
        PdfViewer::bindStore(s->config());
    if (s && s->vault() && !m_modifiedConn) {
        m_modifiedConn = connect(s->vault(), &Vault::modified, this, [this](TFile* f, bool) {
            if (f && f == m_file)
                m_viewer->reload(); // Obsidian's onModify: reload in place
        });
    }
    if (file && s && s->vault())
        m_viewer->load(s->vault()->absolutePath(file->path()));
    emit titleChanged();
}

void PdfView::onRename(TFile* file, const QString& oldPath)
{
    VaultSession* s = session();
    if (s && s->vault() && file == m_file) {
        // the same document under a new path: reopen it there, keeping page and zoom
        const int page = m_viewer->currentPage();
        const QString zoom = m_viewer->zoom();
        m_viewer->load(s->vault()->absolutePath(file->path()));
        m_viewer->setZoom(zoom);
        m_viewer->pages()->goToPage(page);
    }
    FileView::onRename(file, oldPath);
}

QJsonObject PdfView::state() const
{
    QJsonObject o = FileView::state();
    if (m_viewer->file() && m_viewer->file()->isLoaded()) {
        o.insert(QStringLiteral("page"), m_viewer->currentPage() + 1);
        o.insert(QStringLiteral("zoom"), m_viewer->zoom());
    }
    return o;
}

void PdfView::setState(const QJsonObject& st)
{
    m_restoring = true;
    FileView::setState(st);
    if (st.contains(QStringLiteral("zoom")))
        m_viewer->setZoom(st.value(QStringLiteral("zoom")).toVariant().toString());
    if (st.contains(QStringLiteral("page")))
        m_viewer->pages()->goToPage(st.value(QStringLiteral("page")).toInt(1) - 1);
    m_restoring = false;
}

QJsonObject PdfView::ephemeralState() const
{
    QJsonObject o;
    if (m_viewer->file() && m_viewer->file()->isLoaded())
        o.insert(QStringLiteral("page"), m_viewer->currentPage() + 1);
    return o;
}

void PdfView::setEphemeralState(const QJsonObject& e)
{
    if (e.contains(QStringLiteral("subpath")))
        m_viewer->applySubpath(e.value(QStringLiteral("subpath")).toString());
    else if (e.contains(QStringLiteral("page")))
        m_viewer->pages()->goToPage(e.value(QStringLiteral("page")).toInt(1) - 1);
    if (e.value(QStringLiteral("focus")).toBool())
        m_viewer->pages()->setFocus();
}

void PdfView::paneMenuEntries(QList<MenuEntry>& items, const QString& source)
{
    FileView::paneMenuEntries(items, source);
    QPointer<PdfView> self = this;
    items << MenuEntry{QStringLiteral("find"), tr("Find..."), QStringLiteral("file-search"), [self] {
                           if (self)
                               self->showSearch();
                       }};
}

void PdfView::focusContent() { m_viewer->pages()->setFocus(); }

void PdfView::showSearch() { m_viewer->showSearch(); }

} // namespace oc::media
