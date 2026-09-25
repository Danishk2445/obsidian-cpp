#pragma once

// Obsidian's views and embeds for non-Markdown files (plan M8):
//  - views: "image" (ImageView), "audio" (AudioView), "video" (VideoView), "pdf" (PdfView), each
//    registered for Obsidian's extension lists, so Workspace::openFile, the file explorer, the quick
//    switcher and links open those files in them;
//  - embeds (EmbedRegistry, editor/embeds): "![[doc.pdf#page=3]]", "![[doc.pdf#height=400]]",
//    "![[a.mp3]]", "![[v.mp4]]" (and "![](…)") in Live Preview, Reading view and hover previews, as
//    hosted widgets (PDF viewer, audio / video players). Image embeds stay inline images.
// Obsidian registers no commands of its own for these views (Ctrl+F in a PDF opens its find bar).

#include <QStringList>

namespace oc {

class ViewRegistry;

namespace media {
QStringList imageExtensions(); // bmp png jpg jpeg gif svg webp avif
QStringList audioExtensions(); // mp3 wav m4a 3gp flac ogg oga opus
QStringList videoExtensions(); // mp4 webm ogv mov mkv
QStringList pdfExtensions();   // pdf

// The PDF / audio / video embed providers (idempotent; registerMediaViews() calls it).
void registerMediaEmbeds();
} // namespace media

// Registers the "image", "audio", "video" and "pdf" view types with their extensions, and the
// media embed providers.
void registerMediaViews(ViewRegistry& registry);

} // namespace oc
