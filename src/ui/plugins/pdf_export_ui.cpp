#include "ui/plugins/pdf_export_ui.h"

#include "core/commands/command_registry.h"
#include "core/config/vault_config.h"
#include "core/documents/document.h"
#include "core/metadata/link_resolver.h"
#include "core/metadata/metadata_cache.h"
#include "core/util/busy.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/settings/setting_item.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/widgets/value_slider.h"
#include "ui/workspace/workspace.h"

#include <QFileDialog>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace oc {

PdfExportSettings loadPdfExportSettings(const VaultConfig* config)
{
    PdfExportSettings s;
    if (!config)
        return s;
    const QJsonObject o = config->get(u"pdfExportSettings"_s).toObject();
    s.includeName = o.value(u"includeName"_s).toBool(s.includeName);
    s.pageSize = o.value(u"pageSize"_s).toString(s.pageSize);
    s.landscape = o.value(u"landscape"_s).toBool(s.landscape);
    s.margin = o.value(u"margin"_s).toString(s.margin);
    s.downscalePercent = std::clamp(o.value(u"downscalePercent"_s).toInt(s.downscalePercent), 10, 100);
    return s;
}

void savePdfExportSettings(VaultConfig* config, const PdfExportSettings& s)
{
    if (!config)
        return;
    config->set(u"pdfExportSettings"_s, QJsonObject{{u"includeName"_s, s.includeName},
                                                     {u"pageSize"_s, s.pageSize},
                                                     {u"landscape"_s, s.landscape},
                                                     {u"margin"_s, s.margin},
                                                     {u"downscalePercent"_s, s.downscalePercent}});
}

std::function<QString(MainWindow*, const QString&)>& pdfSavePathProvider()
{
    static std::function<QString(MainWindow*, const QString&)> provider = [](MainWindow* w, const QString& suggested) {
        // Obsidian: defaultPath "<basename>.pdf", filters "PDF Files" and "All Files", overwrite confirmation.
        return QFileDialog::getSaveFileName(w, QObject::tr("Export to PDF"), suggested,
                                            QObject::tr("PDF Files (*.pdf);;All Files (*)"));
    };
    return provider;
}

namespace {
PdfExportContext contextFor(VaultSession* s, const QString& sourcePath)
{
    PdfExportContext ctx;
    QPointer<VaultSession> session = s;
    ctx.isResolved = [session, sourcePath](const QString& target) {
        MetadataCache* mc = session ? session->metadata() : nullptr;
        const QString path = LinkResolver::splitSubpath(target).first;
        return !mc || path.isEmpty() || !mc->resolver().resolve(path, sourcePath).isEmpty();
    };
    ctx.resolveAttachment = [session, sourcePath](const QString& linkpath) -> QString {
        MetadataCache* mc = session ? session->metadata() : nullptr;
        if (!mc || !session->vault())
            return {};
        const QString dest = mc->resolver().resolve(LinkResolver::splitSubpath(linkpath).first, sourcePath);
        return dest.isEmpty() ? QString() : session->vault()->absolutePath(dest);
    };
    return ctx;
}

void runExport(QPointer<MainWindow> w, const Text& text, const QString& title, const QString& sourcePath, const QString& out,
               const PdfExportSettings& settings)
{
    NoticeManager* notices = w ? w->notices() : nullptr;
    QPointer<Notice> preparing = notices ? notices->show(QObject::tr("Preparing PDF..."), 0) : nullptr;
    Busy::begin();
    // Let the notice paint before the (synchronous) render.
    QTimer::singleShot(0, w, [w, text, title, sourcePath, out, settings, preparing] {
        QString error;
        const bool ok = w && exportPdf(text, title, out, settings, contextFor(w->session(), sourcePath), &error);
        if (preparing)
            preparing->deleteLater();
        if (w && w->notices()) {
            if (ok)
                w->notices()->show(QObject::tr("PDF Saved to %1").arg(out));
            else
                w->notices()->show(QObject::tr("Failed to save PDF."));
        }
        if (!error.isEmpty())
            qWarning("oc: PDF export failed: %s", qPrintable(error));
        if (ok && desktopHandlers().openUrl)
            desktopHandlers().openUrl(QUrl::fromLocalFile(out));
        Busy::end();
    });
}
} // namespace

Modal* openExportPdfModal(MainWindow* window, MarkdownView* view)
{
    if (!window || !view || !view->file() || !view->document())
        return nullptr;
    VaultSession* session = window->session();
    VaultConfig* config = session ? session->vaultConfig() : nullptr;
    auto* m = new Modal(window->overlay());
    m->setObjectName(u"ocExportPdfModal"_s);
    m->setTitle(QObject::tr("Export to PDF"));
    const QString basename = view->file()->basename();
    m->addParagraph(QObject::tr("Export “%1” to PDF with the settings below.").arg(basename));
    if (QWidget* p = m->contentArea()->findChild<QWidget*>(u"ocModalParagraph"_s))
        p->setProperty("ocMuted", true); // p.u-muted

    auto state = std::make_shared<PdfExportSettings>(loadPdfExportSettings(config));
    auto* rows = new SettingGroup;
    rows->setPlain(true);
    SettingItem* r = rows->addItem();
    r->setName(QObject::tr("Include file name as title"));
    r->addToggle(state->includeName, [state](bool on) { state->includeName = on; });
    r = rows->addItem();
    r->setName(QObject::tr("Page size"));
    r->addDropdown({{u"A3"_s, u"A3"_s}, {u"A4"_s, u"A4"_s}, {u"A5"_s, u"A5"_s}, {u"Legal"_s, u"Legal"_s}, {u"Letter"_s, u"Letter"_s},
                    {u"Tabloid"_s, u"Tabloid"_s}},
                   state->pageSize, [state](const QString& v) { state->pageSize = v; });
    r = rows->addItem();
    r->setName(QObject::tr("Landscape"));
    r->addToggle(state->landscape, [state](bool on) { state->landscape = on; });
    r = rows->addItem();
    r->setName(QObject::tr("Margin"));
    r->addDropdown({{u"0"_s, QObject::tr("Default")}, {u"2"_s, QObject::tr("Minimal")}, {u"1"_s, QObject::tr("None")}}, state->margin,
                   [state](const QString& v) { state->margin = v; });
    r = rows->addItem();
    r->setName(QObject::tr("Downscale percent"));
    r->addSlider(10, 100, 1, state->downscalePercent, [state](double v) { state->downscalePercent = int(v); });
    m->addContentWidget(rows);

    const int exportButton = m->addButton(QObject::tr("Export to PDF"), Modal::ButtonStyle::Cta, false);
    m->addButton(QObject::tr("Cancel"));
    m->setDefaultButton(exportButton);

    QPointer<MainWindow> w = window;
    QPointer<MarkdownView> v = view;
    QPointer<Modal> modal = m;
    QObject::connect(m, &Modal::buttonClicked, m, [=](int index) {
        if (index != exportButton || !w || !v || !v->file() || !v->document())
            return;
        savePdfExportSettings(w->session() ? w->session()->vaultConfig() : nullptr, *state);
        const QString out = pdfSavePathProvider() ? pdfSavePathProvider()(w, v->file()->basename() + u".pdf"_s) : QString();
        if (out.isEmpty())
            return; // cancelled: the modal stays
        const Text text = v->document()->text();
        const QString title = v->file()->basename();
        const QString source = v->file()->path();
        if (modal)
            modal->close();
        runExport(w, text, title, source, out, *state);
    });
    m->open();
    return m;
}

void registerExportPdfCommand(CommandRegistry& registry, MainWindow* window)
{
    QPointer<MainWindow> w = window;
    Command c;
    c.id = u"workspace:export-pdf"_s;
    c.name = QObject::tr("Export to PDF...");
    c.checkCallback = [w](bool checking) {
        Workspace* ws = w ? w->workspace() : nullptr;
        auto* mv = ws ? ws->activeViewOfType<MarkdownView>() : nullptr;
        if (!mv) {
            WorkspaceLeaf* l = ws ? ws->activeRootLeaf() : nullptr;
            mv = l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
        }
        if (!mv || !mv->file())
            return false;
        if (!checking)
            openExportPdfModal(w, mv);
        return true;
    };
    registry.add(c);
}

} // namespace oc
