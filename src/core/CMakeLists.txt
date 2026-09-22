oc_add_library(oc_core
  SOURCES
    commands/command_registry.cpp
    commands/hotkey.cpp
    config/clone_paths.cpp
    config/config_store.cpp
    config/json_file.cpp
    config/vault_config.cpp
    config/vault_registry.cpp
    documents/document.cpp
    documents/document_manager.cpp
    documents/local_storage.cpp
    metadata/link_resolver.cpp
    metadata/link_updater.cpp
    metadata/metadata_cache.cpp
    recovery/file_recovery.cpp
    recovery/snapshot_store.cpp
    vault_session.cpp
  PUBLIC_DEPS oc_text oc_markdown Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent
  PRIVATE_DEPS SQLite::SQLite3 zstd::libzstd
)
