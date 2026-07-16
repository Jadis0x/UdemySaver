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
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
if(WIN32)
    # Use the TLS implementation built into Windows. This avoids OpenSSL and
    # keeps the fetched curl build completely independent from vcpkg.
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Beast and Asio are header-only for this application. Fetch only the Boost
# libraries needed to expose those headers.
set(BOOST_INCLUDE_LIBRARIES asio;beast;config;core;system;throw_exception;type_traits CACHE STRING "" FORCE)
FetchContent_Declare(boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(curl boost nlohmann_json)

target_link_libraries(UdemySaver PRIVATE
    libcurl
    Boost::asio
    Boost::beast
    nlohmann_json::nlohmann_json
)

if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The fetched FFmpeg package supports Windows x64 only.")
    endif()

    # FFmpeg itself does not provide a CMake build. Fetch its LGPL shared
    # development package (headers, import libraries and runtime DLLs) instead.
    FetchContent_Declare(ffmpeg
        URL https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(ffmpeg)

    file(GLOB FFMPEG_INCLUDE_HINTS LIST_DIRECTORIES TRUE
        "${ffmpeg_SOURCE_DIR}/include"
        "${ffmpeg_SOURCE_DIR}/*/include"
    )
    file(GLOB FFMPEG_LIBRARY_HINTS LIST_DIRECTORIES TRUE
        "${ffmpeg_SOURCE_DIR}/lib"
        "${ffmpeg_SOURCE_DIR}/*/lib"
    )
    file(GLOB FFMPEG_RUNTIME_DLLS
        "${ffmpeg_SOURCE_DIR}/bin/*.dll"
        "${ffmpeg_SOURCE_DIR}/*/bin/*.dll"
    )

    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
        HINTS ${FFMPEG_INCLUDE_HINTS}
        NO_DEFAULT_PATH
        REQUIRED
    )
    find_library(FFMPEG_AVFORMAT_LIBRARY NAMES avformat
        HINTS ${FFMPEG_LIBRARY_HINTS} NO_DEFAULT_PATH REQUIRED)
    find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec
        HINTS ${FFMPEG_LIBRARY_HINTS} NO_DEFAULT_PATH REQUIRED)
    find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil
        HINTS ${FFMPEG_LIBRARY_HINTS} NO_DEFAULT_PATH REQUIRED)

    target_include_directories(UdemySaver PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(UdemySaver PRIVATE
        ${FFMPEG_AVFORMAT_LIBRARY}
        ${FFMPEG_AVCODEC_LIBRARY}
        ${FFMPEG_AVUTIL_LIBRARY}
    )

    if(NOT FFMPEG_RUNTIME_DLLS)
        message(FATAL_ERROR "Fetched FFmpeg package does not contain runtime DLLs.")
    endif()
    add_custom_command(TARGET UdemySaver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:UdemySaver>
        COMMENT "Copying fetched FFmpeg runtime DLLs"
    )
else()
    # On Unix platforms FFmpeg is built using its configure/make build rather
    # than CMake. Keep the native package fallback until a hermetic external
    # build is provided for those platforms.
    find_package(FFMPEG REQUIRED)
    target_include_directories(UdemySaver PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(UdemySaver PRIVATE ${FFMPEG_LIBRARIES})
endif()

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
