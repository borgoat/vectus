include(CMakeFindDependencyMacro)

include("${CMAKE_CURRENT_LIST_DIR}/vectusTargets.cmake")

# Only the curl transport needs libcurl
if(TARGET vectus::curl)
    find_dependency(CURL)
endif()
