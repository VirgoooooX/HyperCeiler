cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library kept for the original in-process HyperOS 4 experiment.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)

# Root-side helper used by the hyos_spawner-compatible path. v12 arms the
# persistent hyos_spawner before Launcher restart, catches the forked child
# under ptrace, then gates that child syscall-by-syscall until the exact Rust
# launcher mapping appears. This applies code/data patches before the child can
# execute Rust launcher initialization.
add_library(
    hyperceiler_os4_root
    SHARED
    os4_root_patcher.c
    os4_root_softbreak_v12.c
)

target_compile_features(hyperceiler_os4_root PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_root PRIVATE -Wall -Wextra -Werror)
