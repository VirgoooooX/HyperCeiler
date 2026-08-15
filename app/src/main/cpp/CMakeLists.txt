cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library kept for the original in-process HyperOS 4 experiment.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)

# Root-side helper used by the hyos_spawner-compatible path. It is loaded by a
# tiny app_process CLI running as uid 0 and accesses /proc/<pid>/mem with
# pread/pwrite so 64-bit launcher virtual addresses are handled natively.
add_library(hyperceiler_os4_root SHARED os4_root_patcher.c)

target_compile_features(hyperceiler_os4_root PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_root PRIVATE -Wall -Wextra -Werror)
