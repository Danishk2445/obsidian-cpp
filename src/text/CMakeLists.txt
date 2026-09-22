oc_add_library(oc_text
  SOURCES
    changeset.cpp
    codec.cpp
    diff.cpp
    history.cpp
    selection.cpp
    text.cpp
  PUBLIC_DEPS Qt6::Core
  PRIVATE_DEPS PkgConfig::XXHASH
)
