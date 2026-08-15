cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library kept for the original in-process HyperOS 4 experiment.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)

# Root-side helper used by the hyos_spawner-compatible path. The current
# diagnostic watcher uses ARM64 ptrace hardware execution breakpoints to
# mutate the real DeviceParam object at save_device_param() entry, avoiding
# executable-page writes and remote instruction-cache coherency entirely.
# os4_root_patcher.c remains as a one-shot diagnostic fallback.
add_library(
    hyperceiler_os4_root
    SHARED
    os4_root_patcher.c
    os4_root_hwbreak.c
)

target_compile_features(hyperceiler_os4_root PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_root PRIVATE -Wall -Wextra -Werror)
