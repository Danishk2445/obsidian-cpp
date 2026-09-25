# Application resources (compiled into oc_resources; consumers get the resource objects automatically).
add_library(oc_resources STATIC oc_resources_dummy.cpp)
target_link_libraries(oc_resources PUBLIC Qt6::Core)
qt_add_resources(oc_resources "oc_resources"
  PREFIX "/oc"
  FILES
    themes/obsidian-default.json
    qss/app.qss
    icons/lucide.json
    icons/custom.json
    icons/LUCIDE_LICENSE
    app/obsidian-cpp.svg
)
