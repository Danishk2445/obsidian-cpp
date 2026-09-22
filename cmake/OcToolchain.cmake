# Compiler launcher / linker / warnings / sanitizers shared by all oc targets.

find_program(OC_CCACHE ccache)
if(OC_CCACHE)
  set(CMAKE_C_COMPILER_LAUNCHER "${OC_CCACHE}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${OC_CCACHE}")
  message(STATUS "oc: using ccache ${OC_CCACHE}")
endif()

find_program(OC_MOLD mold)
if(OC_MOLD)
  add_link_options(-fuse-ld=mold)
  message(STATUS "oc: linking with mold")
endif()

add_library(oc_build_flags INTERFACE)
target_compile_options(oc_build_flags INTERFACE
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-trigraphs
  $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
  $<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>)
target_compile_definitions(oc_build_flags INTERFACE
  QT_NO_CAST_FROM_BYTEARRAY
  QT_USE_QSTRINGBUILDER
  $<$<CONFIG:Debug>:OC_DEBUG=1>)

if(OC_SANITIZE)
  list(JOIN OC_SANITIZE "," _oc_san)
  message(STATUS "oc: sanitizers ${_oc_san}")
  target_compile_options(oc_build_flags INTERFACE -fsanitize=${_oc_san} -fno-sanitize-recover=undefined)
  target_link_options(oc_build_flags INTERFACE -fsanitize=${_oc_san})
endif()
