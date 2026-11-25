cmake_minimum_required(VERSION 3.21)
project(vectus VERSION 0.1.0 DESCRIPTION "tus resumable upload client" LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(CURL REQUIRED)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(VECTUS_WARNINGS -Wall -Wextra -Wpedantic)
endif()

add_library(vectus src/client.c src/upload.c src/uploader.c)
add_library(vectus::vectus ALIAS vectus)
target_include_directories(vectus PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(vectus PUBLIC CURL::libcurl)
target_compile_options(vectus PRIVATE ${VECTUS_WARNINGS})

install(TARGETS vectus EXPORT vectusTargets)
install(DIRECTORY include/vectus TYPE INCLUDE)
install(EXPORT vectusTargets NAMESPACE vectus:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vectus)
write_basic_package_version_file(vectusConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES cmake/vectusConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/vectusConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vectus)

# shortcut: the .pc file bakes in the configure-time prefix, so `cmake --install --prefix` elsewhere breaks it; make it relocatable if packagers need that
configure_file(cmake/vectus.pc.in vectus.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vectus.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Tests only for the top-level project, so projects embedding vectus don't need Check
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CHECK REQUIRED IMPORTED_TARGET check)

    # The tests compile src/client.c themselves, so this sanitizes the code under test without touching the installed library
    include(CheckCSourceCompiles)
    set(VECTUS_SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    list(JOIN VECTUS_SANITIZE " " CMAKE_REQUIRED_FLAGS)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${VECTUS_SANITIZE})
    check_c_source_compiles("int main(void) { return 0; }" VECTUS_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT VECTUS_HAVE_SANITIZERS)
        message(WARNING "The compiler cannot build with ASan and UBSan, so the tests run unsanitized")
        set(VECTUS_SANITIZE "")
    endif()

    add_executable(vectus-test tests/suite.c)
    target_include_directories(vectus-test PRIVATE include)
    target_link_libraries(vectus-test PRIVATE CURL::libcurl PkgConfig::CHECK)
    target_compile_options(vectus-test PRIVATE ${VECTUS_WARNINGS} ${VECTUS_SANITIZE})
    target_link_options(vectus-test PRIVATE ${VECTUS_SANITIZE})
    add_test(NAME vectus-test COMMAND vectus-test)
endif()
