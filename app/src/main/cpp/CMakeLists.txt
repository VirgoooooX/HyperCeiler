cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library kept for the original in-process HyperOS 4 experiment.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)

# Root-side helper used by the hyos_spawner-compatible path. The v11 watcher
# seizes the launcher before libapp_launcher.so returns from mmap, patches the
# Rust x-count bounds through PTRACE_POKETEXT, then intercepts the first
# DeviceConfigs::save_device_param call before GridConfig consumes 5x8.
# os4_root_patcher.c remains as a one-shot diagnostic fallback.
add_library(
    hyperceiler_os4_root
    SHARED
    os4_root_patcher.c
    os4_root_softbreak_v11.c
)

set_source_files_properties(
    os4_root_softbreak_v11.c
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/os4_root_v11_compat.h"
)

target_compile_features(hyperceiler_os4_root PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_root PRIVATE -Wall -Wextra -Werror)
