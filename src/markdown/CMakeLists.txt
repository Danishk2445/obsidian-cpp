oc_add_library(oc_markdown
  SOURCES
    inline_parser.cpp
    inline_rules.cpp
    md_parser.cpp
    md_refdef.cpp
    md_scan.cpp
    md_update.cpp
    metadata.cpp
    syntax_tree.cpp
  PUBLIC_DEPS oc_text Qt6::Core
  PRIVATE_DEPS yaml-cpp::yaml-cpp
)
