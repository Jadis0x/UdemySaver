# Add source to this project's executable.
set (SOURCES 
    "src/main.cpp"
    "src/server/HttpServer.cpp"
    "src/server/HttpServer.h"
    "src/server/RequestHandler.h"
    "src/server/RequestHandler.cpp"
    "utils/FFmpegHelper.h"
    "utils/FFmpegHelper.cpp"
    "utils/EncryptedDashHelper.h"
    "utils/EncryptedDashHelper.cpp"
    )

if (WIN32)
    list(APPEND SOURCES "../resources/app.rc")
endif()

add_executable(UdemySaver ${SOURCES})

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET UdemySaver PROPERTY CXX_STANDARD 20)
endif()

target_include_directories(UdemySaver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# Third-party C++ dependencies are fetched at configure time. This keeps the
# build independent from a vcpkg triplet and uses the active toolchain's CPU
# architecture (including arm64 on Apple Silicon).
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_10_1
    GIT_SHALLOW TRUE
)

# Beast and Asio are header-only for this application. Fetch only the Boost
# libraries needed to expose those headers.
set(BOOST_INCLUDE_LIBRARIES asio;beast;config;core;system;throw_exception;type_traits CACHE STRING "" FORCE)
FetchContent_Declare(boost
    GIT_REPOSITORY https://github.com/boostorg/boost.git
    GIT_TAG boost-1.86.0
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(curl boost nlohmann_json)

target_link_libraries(UdemySaver PRIVATE
    libcurl
    Boost::headers
    nlohmann_json::nlohmann_json
)

find_package(FFMPEG QUIET)
if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR 
        "\n[ERROR] FFMPEG not found!\n"
        "Install a native FFmpeg development package, then configure again.\n"
        "  macOS (Apple Silicon): brew install ffmpeg pkg-config\n"
        "  Ubuntu/Debian:          sudo apt install libavcodec-dev libavformat-dev libavutil-dev pkg-config\n"
        "  Windows:                install FFmpeg headers and libraries, then set CMAKE_PREFIX_PATH\n"
    )
endif()
target_include_directories(UdemySaver PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(UdemySaver PRIVATE ${FFMPEG_LIBRARIES})

set(WEB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/www)
file(MAKE_DIRECTORY ${WEB_ROOT})


add_custom_target(CopyWWWFiles ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:UdemySaver>/www
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/www
            $<TARGET_FILE_DIR:UdemySaver>/www
    COMMENT "Copying www to output dir"
)

add_dependencies(UdemySaver CopyWWWFiles)
