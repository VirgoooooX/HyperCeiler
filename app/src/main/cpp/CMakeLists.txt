cmake_minimum_required(VERSION 3.22.1)
project(hyperceiler_os4_launcher C)

# Native hook library for the build-specific HyperOS 4 launcher compatibility path.
add_library(hyperceiler_os4_launcher SHARED os4_launcher_patch.c)

target_compile_features(hyperceiler_os4_launcher PRIVATE c_std_11)
target_compile_options(hyperceiler_os4_launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hyperceiler_os4_launcher PRIVATE dl log)
