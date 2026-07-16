cmake_minimum_required (VERSION 3.20...4.2)

project ("UdemySaver")

include(FetchContent)

# Add custom CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

# Sub-projects
add_subdirectory ("UdemySaver")
