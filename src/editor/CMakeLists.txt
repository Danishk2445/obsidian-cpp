oc_add_library(oc_editor
  SOURCES
    editor_view.cpp
    source_decorator.cpp
  PUBLIC_DEPS oc_render oc_core Qt6::Widgets
)
