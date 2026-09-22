oc_add_library(oc_render
  SOURCES
    theme/css_value.cpp
    theme/icons.cpp
    theme/theme.cpp
  PUBLIC_DEPS oc_core Qt6::Gui Qt6::Widgets Qt6::Svg oc_resources
)
