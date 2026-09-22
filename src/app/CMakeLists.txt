qt_add_executable(obsidian-cpp
  main.cpp
  application.cpp
  cli.cpp
)
target_include_directories(obsidian-cpp PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(obsidian-cpp PRIVATE oc_build_flags oc_ui oc_editor)
target_compile_definitions(obsidian-cpp PRIVATE OC_VERSION="${PROJECT_VERSION}")
set_target_properties(obsidian-cpp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
