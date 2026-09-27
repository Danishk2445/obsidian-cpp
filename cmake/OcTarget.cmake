# oc_add_library(<name> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
function(oc_add_library name)
  cmake_parse_arguments(A "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(${name} STATIC ${A_SOURCES})
  target_include_directories(${name} PUBLIC "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PUBLIC ${A_PUBLIC_DEPS} PRIVATE oc_build_flags ${A_PRIVATE_DEPS})
endfunction()

# oc_add_test(<name> SOURCES ... DEPS ... [LABELS ...] [QT] [GUI])
#   QT:  QtTest-based test (own main via QTEST_MAIN).
#   GUI: gtest with a QApplication main (offscreen), for fonts/painting/widgets.
#   default: plain gtest_main.
function(oc_add_test name)
  cmake_parse_arguments(A "QT;GUI" "" "SOURCES;DEPS;LABELS" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  target_link_libraries(${name} PRIVATE oc_build_flags ${A_DEPS})
  if(A_QT)
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Widgets)
  elseif(A_GUI)
    target_sources(${name} PRIVATE "${CMAKE_SOURCE_DIR}/tests/support/gtest_qt_main.cpp")
    target_link_libraries(${name} PRIVATE GTest::gtest Qt6::Widgets)
  else()
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
  endif()
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests")
  target_compile_definitions(${name} PRIVATE OC_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OC_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0;TSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/tsan.supp:second_deadlock_stack=1;OC_TESTING=1"
    LABELS "${A_LABELS}")
endfunction()
