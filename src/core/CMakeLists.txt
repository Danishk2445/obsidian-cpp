oc_add_library(oc_core
  SOURCES
    commands/command_registry.cpp
    commands/hotkey.cpp
    config/clone_paths.cpp
    config/config_store.cpp
    config/json_file.cpp
    config/vault_registry.cpp
    documents/document.cpp
    documents/local_storage.cpp
    fuzzy/fuzzy.cpp
    time/moment_format.cpp
    vault_session.cpp
  PUBLIC_DEPS oc_text oc_markdown Qt6::Core Qt6::Gui Qt6::Widgets
)
