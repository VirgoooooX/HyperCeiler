cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library kept for the original in-process HyperOS 4 experiment.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)

# Root-side helper used by the hyos_spawner-compatible path. The resident
# watcher catches the Rust launcher before its DeviceParam/GridConfig startup
# path runs; os4_root_patcher.c is kept as a one-shot diagnostic fallback.
add_library(
    hyperceiler_os4_root
    SHARED
    os4_root_patcher.c
    os4_root_watcher.c
)

target_compile_features(hyperceiler_os4_root PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_root PRIVATE -Wall -Wextra -Werror)
