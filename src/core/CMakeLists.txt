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
    metadata/link_resolver.cpp
    metadata/metadata_cache.cpp
    vault/fs_scan.cpp
    vault/fs_watcher.cpp
    vault/ignore_rules.cpp
    vault/trash.cpp
    vault/vault.cpp
    vault/vault_io.cpp
    vault/vault_sync.cpp
    vault_session.cpp
  PUBLIC_DEPS oc_text oc_markdown Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent
  PRIVATE_DEPS KF6::KIOCore
)
