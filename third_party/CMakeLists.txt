# Third-party code built as part of the project.

if(OC_WITH_MATH)
  # JKQTMathText (LGPL-2.1) — native LaTeX math renderer, vendored subset (see jkqtplotter/VENDORED.md).
  # Built SHARED (LGPL); these variables are directory-scoped so they don't leak into src/.
  set(BUILD_SHARED_LIBS ON)
  set(JKQtPlotter_BUILD_EXAMPLES OFF)
  set(JKQtPlotter_BUILD_TOOLS OFF)
  set(JKQtPlotter_BUILD_TESTS OFF)
  set(JKQtPlotter_BUILD_LIB_JKQTMATH OFF)
  set(JKQtPlotter_BUILD_LIB_JKQTMATHTEXT ON)
  set(JKQtPlotter_BUILD_LIB_JKQTFASTPLOTTER OFF)
  set(JKQtPlotter_BUILD_LIB_JKQTPLOTTER OFF)
  set(JKQtPlotter_BUILD_INCLUDE_XITS_FONTS ON)
  set(JKQtPlotter_BUILD_INCLUDE_FIRAMATH_FONTS OFF)
  set(JKQtPlotter_BUILD_DECORATE_LIBNAMES_WITH_BUILDTYPE OFF)
  set(JKQtPlotter_BUILD_WITH_PRECOMPILED_HEADERS OFF)
  set(JKQtPlotter_BUILD_WITH_TIMING_INFO_OUTPUT OFF)
  add_subdirectory(jkqtplotter EXCLUDE_FROM_ALL SYSTEM)
endif()
