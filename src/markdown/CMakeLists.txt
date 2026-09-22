oc_add_library(oc_markdown
  SOURCES
    metadata.cpp
    syntax_tree_stub.cpp
  PUBLIC_DEPS oc_text Qt6::Core
  PRIVATE_DEPS yaml-cpp::yaml-cpp
)
