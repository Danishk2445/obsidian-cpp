oc_add_library(oc_markdown
  SOURCES
    inline_parser.cpp
    inline_rules.cpp
    metadata.cpp
    syntax_tree_stub.cpp
  PUBLIC_DEPS oc_text Qt6::Core
  PRIVATE_DEPS yaml-cpp::yaml-cpp
)
