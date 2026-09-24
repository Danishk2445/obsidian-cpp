#pragma once

#include "text/text.h"

#include <QString>

#include <functional>

namespace oc {

// Obsidian's "Export to PDF" (workspace:export-pdf) with its app.json pdfExportSettings:
// {"includeName": true, "pageSize": "Letter", "landscape": false, "margin": "0", "downscalePercent": 100}
struct PdfExportSettings {
    bool includeName = true;
    QString pageSize = QStringLiteral("Letter"); // Letter, Legal, Tabloid, A3, A4, A5
    bool landscape = false;
    QString margin = QStringLiteral("0");        // Electron marginsType: "0" Default, "1" None, "2" Minimal
    int downscalePercent = 100;
};

struct PdfExportContext {
    std::function<bool(const QString& target)> isResolved;
    std::function<QString(const QString& linkpath)> resolveAttachment;
};

// Renders `text` like the reading view (light colours) and writes a paginated PDF.
bool exportPdf(const Text& text, const QString& title, const QString& outPath, const PdfExportSettings& settings,
               const PdfExportContext& ctx = {}, QString* error = nullptr);

} // namespace oc
