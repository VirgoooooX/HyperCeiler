cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library kept for the original in-process HyperOS 4 experiment.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)

# Root-side helper used by the hyos_spawner-compatible path. For the current
# diagnostic build the resident watcher uses data-only DeviceParam pinning so
# timing and ARM64 instruction-cache effects can be separated from address/
# signature correctness. os4_root_patcher.c remains as a one-shot fallback.
add_library(
    hyperceiler_os4_root
    SHARED
    os4_root_patcher.c
    os4_root_data_probe.c
)

target_compile_features(hyperceiler_os4_root PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_root PRIVATE -Wall -Wextra -Werror)
