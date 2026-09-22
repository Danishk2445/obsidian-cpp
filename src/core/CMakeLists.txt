oc_add_library(oc_core
  SOURCES
    config/clone_paths.cpp
    config/config_store.cpp
    config/json_file.cpp
    config/vault_registry.cpp
    vault/fs_scan.cpp
    vault/fs_watcher.cpp
    vault/ignore_rules.cpp
    vault/trash.cpp
    vault/vault.cpp
    vault/vault_io.cpp
    vault/vault_sync.cpp
    vault_session.cpp
  PUBLIC_DEPS Qt6::Core oc_text
  PRIVATE_DEPS KF6::KIOCore
)
