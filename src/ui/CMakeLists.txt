oc_add_library(oc_ui
  SOURCES
    overlay/overlay_layer.cpp
    overlay/suggest_modal.cpp
    picker/vault_picker.cpp
    shell/empty_view.cpp
    shell/main_window.cpp
    shell/ribbon.cpp
    shell/root_placeholder.cpp
    shell/side_dock.cpp
    shell/status_bar.cpp
    shell/window_frame.cpp
    widgets/icon_button.cpp
  PUBLIC_DEPS oc_core oc_render Qt6::Widgets
)
