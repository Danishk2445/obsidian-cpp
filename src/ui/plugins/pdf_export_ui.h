#pragma once

#include "editor/export/pdf_exporter.h"

#include <QString>

#include <functional>

namespace oc {

class CommandRegistry;
class MainWindow;
class MarkdownView;
class Modal;
class VaultConfig;

// app.json "pdfExportSettings" ({includeName, pageSize, landscape, margin, downscalePercent}).
PdfExportSettings loadPdfExportSettings(const VaultConfig* config);
void savePdfExportSettings(VaultConfig* config, const PdfExportSettings& settings);

// Obsidian's "Export to PDF" modal for the note of `view`: the options (saved back to app.json on
// export), then a save dialog ("<basename>.pdf"); cancelling the dialog keeps the modal open. The note
// is rendered like the reading view in light colours; "Preparing PDF..." shows meanwhile, then "PDF
// Saved to <path>" and the PDF opens in the default viewer.
Modal* openExportPdfModal(MainWindow* window, MarkdownView* view);

// Where the PDF goes: the default asks with a file dialog. Tests (and headless runs) replace it; an
// empty result means cancelled.
std::function<QString(MainWindow* window, const QString& suggestedName)>& pdfSavePathProvider();

// workspace:export-pdf ("Export to PDF..."), available while a markdown view is active.
void registerExportPdfCommand(CommandRegistry& registry, MainWindow* window);

} // namespace oc
