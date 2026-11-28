cmake_minimum_required(VERSION 3.16)

# vectus itself, the repository's root, as a component named after its directory, which run.sh mounts as "vectus";
# Wi-Fi for the board comes from ESP-IDF's examples, and main only requires it off the linux target
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../.. $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)
set(COMPONENTS main)  # build main and what it requires, not every component found

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vectus-esp-test)

# vectus's own warnings fail the build here, as they do in its CMake build
idf_component_get_property(vectus_lib vectus COMPONENT_LIB)
target_compile_options(${vectus_lib} PRIVATE -Wall -Wextra -Werror)
