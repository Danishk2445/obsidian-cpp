oc_add_library(oc_render
  SOURCES
    layout/display_block.cpp
    layout/doc_layout.cpp
    layout/style_sheet.cpp
    services/code_highlighter.cpp
    theme/css_value.cpp
    theme/icons.cpp
    theme/theme.cpp
  PUBLIC_DEPS oc_core oc_text Qt6::Gui Qt6::Widgets Qt6::Svg oc_resources KF6::SyntaxHighlighting
)
