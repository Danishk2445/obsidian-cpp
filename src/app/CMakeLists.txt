qt_add_executable(obsidian-cpp
  main.cpp
  application.cpp
  scenario_runner.cpp
  cli.cpp
  single_instance.cpp
)
target_include_directories(obsidian-cpp PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(obsidian-cpp PRIVATE oc_build_flags oc_ui oc_editor Qt6::Network)
target_compile_definitions(obsidian-cpp PRIVATE OC_VERSION="${PROJECT_VERSION}")
set_target_properties(obsidian-cpp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Read-only vault indexing summary (acceptance checks).
qt_add_executable(oc-vault-stats ../tools/vault_stats.cpp)
target_include_directories(oc-vault-stats PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(oc-vault-stats PRIVATE oc_build_flags oc_core)
set_target_properties(oc-vault-stats PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# `cmake --install build/rel --prefix ~/.local`: the binary, a launcher entry and the app icon.
set_target_properties(obsidian-cpp PROPERTIES INSTALL_RPATH "$ORIGIN/../lib/obsidian-cpp")
install(TARGETS obsidian-cpp RUNTIME DESTINATION bin)
if(OC_WITH_MATH)
  # the LGPL math renderer is a shared library next to the binary's private lib dir
  install(TARGETS JKQTMathText6 JKQTCommon6 LIBRARY DESTINATION lib/obsidian-cpp NAMELINK_SKIP)
endif()
install(FILES "${CMAKE_SOURCE_DIR}/resources/app/obsidian-cpp.desktop" DESTINATION share/applications)
install(FILES "${CMAKE_SOURCE_DIR}/resources/app/obsidian-cpp.svg" DESTINATION share/icons/hicolor/scalable/apps)
