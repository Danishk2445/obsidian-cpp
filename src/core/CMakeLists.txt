oc_add_library(oc_core
  SOURCES
    config/clone_paths.cpp
    config/config_store.cpp
    config/json_file.cpp
    config/vault_registry.cpp
    vault_session.cpp
  PUBLIC_DEPS Qt6::Core
)
