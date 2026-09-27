# Runs obsidian-cpp headless on a fresh copy of a fixture vault with isolated XDG dirs (never the user's config).
#   cmake -DAPP=<binary> -DFIXTURE=<vault dir> -DWORK=<scratch dir> -DSCRIPT=<scenario file> "-DARGS=<extra;args>" -P run_app.cmake
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${FIXTURE}/" DESTINATION "${WORK}/vault")
set(ENV{XDG_CONFIG_HOME} "${WORK}/xdg/config")
set(ENV{XDG_DATA_HOME} "${WORK}/xdg/data")
set(ENV{XDG_CACHE_HOME} "${WORK}/xdg/cache")
set(ENV{QT_QPA_PLATFORM} "offscreen")
execute_process(COMMAND "${APP}" --vault "${WORK}/vault" --no-register --deterministic --size 1000x700 ${ARGS} --script "${SCRIPT}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "obsidian-cpp exited with ${rc}\n${out}\n${err}")
endif()
