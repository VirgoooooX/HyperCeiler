cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library kept for the original in-process HyperOS 4 experiment.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)

# v13 keeps the proven v12 ptrace/SWBP implementation but fixes spawner
# discovery.  Xiaomi's Rust spawner logs with the hyos_spawner tag without
# necessarily exposing that name through /proc/PID/comm.  Generate a tiny
# v13 translation unit that resolves the spawner from the live launcher's PPid
# before the controller restarts com.miui.home.
set(OS4_V13_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/os4_root_softbreak_v13.c")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/os4_root_softbreak_v12.c" OS4_V13_TEXT)
string(REPLACE
    "spawner = hc_find_spawner_v12();"
    "spawner = hc_find_spawner_v13();"
    OS4_V13_TEXT
    "${OS4_V13_TEXT}"
)
string(REPLACE "SWBP v12" "SWBP v13" OS4_V13_TEXT "${OS4_V13_TEXT}")
string(REPLACE
    "arming hyos_spawner pre-fork gate"
    "arming launcher-parent pre-fork gate"
    OS4_V13_TEXT
    "${OS4_V13_TEXT}"
)
string(REPLACE
    "hyos_spawner not found"
    "launcher spawner parent not found"
    OS4_V13_TEXT
    "${OS4_V13_TEXT}"
)
string(REPLACE
    "gate armed on hyos_spawner"
    "gate armed on launcher spawner parent"
    OS4_V13_TEXT
    "${OS4_V13_TEXT}"
)
file(WRITE "${OS4_V13_SOURCE}" "${OS4_V13_TEXT}")

add_library(
    hyperceiler_os4_root
    SHARED
    os4_root_patcher.c
    "${OS4_V13_SOURCE}"
)

# The generated source still includes helper C files by their source-tree names.
target_include_directories(hyperceiler_os4_root PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_source_files_properties(
    "${OS4_V13_SOURCE}"
    PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/os4_root_v13_compat.h"
)

target_compile_features(hyperceiler_os4_root PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_root PRIVATE -Wall -Wextra -Werror)
