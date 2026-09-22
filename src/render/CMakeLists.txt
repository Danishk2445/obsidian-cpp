oc_add_library(oc_render
  SOURCES
    layout/display_block.cpp
    layout/doc_layout.cpp
    layout/style_sheet.cpp
    services/code_highlighter.cpp
    services/image_cache.cpp
    services/math_renderer.cpp
    widgets/inline_widgets.cpp
    theme/css_value.cpp
    theme/icons.cpp
    theme/theme.cpp
  PUBLIC_DEPS oc_core oc_text Qt6::Gui Qt6::Widgets Qt6::Svg oc_resources KF6::SyntaxHighlighting
)

if(OC_WITH_MATH)
  target_link_libraries(oc_render PUBLIC JKQTMathText6)
  target_compile_definitions(oc_render PUBLIC OC_HAVE_JKQTMATHTEXT=1)
endif()
