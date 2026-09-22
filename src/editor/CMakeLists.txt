oc_add_library(oc_editor
  SOURCES
    editor_hooks.cpp
    editor_view.cpp
    export/pdf_exporter.cpp
    markdown_commands.cpp
    plugins/greentext_guard.cpp
    live_preview_decorator.cpp
    source_decorator.cpp
    widgets/block_widgets.cpp
    widgets/nested_renderer.cpp
  PUBLIC_DEPS oc_render oc_core Qt6::Widgets Qt6::Gui
)
