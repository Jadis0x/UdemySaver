message(STATUS "DEBUG: VCPKG_ROOT is $ENV{VCPKG_ROOT}")
message(STATUS "DEBUG: CMAKE_TOOLCHAIN_FILE is ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "DEBUG: VCPKG_TARGET_TRIPLET is ${VCPKG_TARGET_TRIPLET}")

# Add source to this project's executable.
set (SOURCES 
    "src/main.cpp"
    "src/server/HttpServer.cpp"
    "src/server/HttpServer.h"
    "src/server/RequestHandler.h"
    "src/server/RequestHandler.cpp"
    "utils/FFmpegHelper.h"
    "utils/FFmpegHelper.cpp"
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

# Curl
find_package(CURL QUIET)
if(NOT CURL_FOUND)
    message(FATAL_ERROR 
        "\n[ERROR] CURL library not found!\n"
        "To fix this, please run the following command in your terminal:\n"
        "  vcpkg install curl:x64-windows\n"
    )
endif()
target_link_libraries(UdemySaver PRIVATE CURL::libcurl)

# boost-beast
find_package(Boost QUIET)
if(NOT Boost_FOUND)
    message(FATAL_ERROR 
        "\n[ERROR] Boost (Beast) library not found!\n"
        "To fix this, please run the following command in your terminal:\n"
        "  vcpkg install boost-beast:x64-windows\n"
    )
endif()
target_link_libraries(UdemySaver PRIVATE Boost::boost)

# nlohmann_json
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(FATAL_ERROR 
        "\n[ERROR] nlohmann_json not found!\n"
        "To fix this, please run the following command in your terminal:\n"
        "  vcpkg install nlohmann-json:x64-windows\n"
    )
endif()
target_link_libraries(UdemySaver PRIVATE nlohmann_json::nlohmann_json)

find_package(FFMPEG QUIET)
if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR 
        "\n[ERROR] FFMPEG not found!\n"
        "To fix this, please run the following command in your terminal:\n"
        "  vcpkg install ffmpeg[avcodec,avformat,avutil]:x64-windows\n"
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
