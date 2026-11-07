# SPDX-License-Identifier: GPL-3.0-only
# PS5 build. Configure with the SDK's prospero-cmake (scripts/build.sh does this inside Docker);
# third-party libraries come prebuilt from scripts/deps.sh in out/deps.
cmake_minimum_required(VERSION 3.25)
project(ProsperoSunshine LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
# std::jthread/stop_token sit behind libc++'s experimental gate in SDK v0.45.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
set(PROSPERO_WARNINGS -Wall -Wextra -Werror)

set(PROSPERO_OUT "${CMAKE_SOURCE_DIR}/out" CACHE PATH "Where ELFs and libraries are placed")
set(PROSPERO_DEPS "${PROSPERO_OUT}/deps" CACHE PATH "Prefix of the prebuilt third-party libraries")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROSPERO_OUT}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROSPERO_OUT}")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# Prebuilt static archives from out/deps as imported targets.
function(prospero_dep name)
    add_library(dep::${name} STATIC IMPORTED GLOBAL)
    set_target_properties(dep::${name} PROPERTIES
        IMPORTED_LOCATION "${PROSPERO_DEPS}/lib/lib${name}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PROSPERO_DEPS}/include"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PROSPERO_DEPS}/include")
endfunction()
foreach(lib ssl crypto opus enet nanors)
    prospero_dep(${lib})
endforeach()
set_target_properties(dep::ssl PROPERTIES INTERFACE_LINK_LIBRARIES "dep::crypto;pthread")

# Platform layer.
# ResourceArbitrator lookup by NID; never calls into the module.
add_library(prospero_ra STATIC src/platform/ps5/ra_resolve.c src/platform/ps5/diag.c)
target_include_directories(prospero_ra PUBLIC src)
target_compile_options(prospero_ra PRIVATE ${PROSPERO_WARNINGS})

add_library(prospero_capture STATIC src/platform/ps5/capture.c)
target_include_directories(prospero_capture PUBLIC src)
target_link_libraries(prospero_capture PUBLIC prospero_ra)
target_compile_options(prospero_capture PRIVATE ${PROSPERO_WARNINGS})

add_library(prospero_pad STATIC src/platform/ps5/pad.c src/platform/ps5/kbm.c)
target_include_directories(prospero_pad PUBLIC src)
target_compile_options(prospero_pad PRIVATE ${PROSPERO_WARNINGS})

add_library(prospero_core STATIC src/capture/capture_session.cpp src/platform/ps5/capture_backend.cpp
    src/platform/ps5/shared_capture.cpp)
target_include_directories(prospero_core PUBLIC src)
target_link_libraries(prospero_core PUBLIC prospero_capture)
target_compile_options(prospero_core PRIVATE ${PROSPERO_WARNINGS})

# mDNS responder (P9.2): portable packet code plus the socket loop.
add_library(prospero_mdns STATIC src/net/mdns.cpp src/platform/ps5/mdns_responder.cpp)
target_include_directories(prospero_mdns PUBLIC src)
target_compile_options(prospero_mdns PRIVATE ${PROSPERO_WARNINGS})

# Probes: each is a standalone payload that logs its result to klog.
function(prospero_probe name)
    add_executable(${name} src/${name}.cpp)
    target_compile_options(${name} PRIVATE ${PROSPERO_WARNINGS})
    target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()
prospero_probe(runtime_probe)
prospero_probe(jthread_probe)
prospero_probe(capture_probe prospero_core)
prospero_probe(capture_soak prospero_core)
prospero_probe(pad_probe prospero_pad)
prospero_probe(pad_output_probe)
prospero_probe(kbm_probe prospero_pad)
prospero_probe(pad_user_probe prospero_pad SceUserService)
prospero_probe(relisten_probe)
# Not a probe: the host embeds it and hands it to the loader for "Restart host" on the web page.
prospero_probe(relaunch_helper)
prospero_probe(port_hold_probe)
prospero_probe(rest_probe)
prospero_probe(mdns_probe prospero_mdns)
prospero_probe(avcap_spike prospero_capture)
prospero_probe(open_probe prospero_capture)
prospero_probe(uhd_spike prospero_capture)
prospero_probe(hevc_memory_spike prospero_capture)
prospero_probe(hdr_spike prospero_capture)
prospero_probe(mode_spike prospero_capture)
prospero_probe(small_mode_probe prospero_capture)
prospero_probe(slice_rate_probe prospero_capture)
prospero_probe(surround_probe prospero_capture)
prospero_probe(channel_map_probe prospero_capture SceAudioOut SceUserService)
prospero_probe(hevc_spike prospero_capture)
prospero_probe(close_probe prospero_ra SceSystemService SceUserService)
prospero_probe(firmware_probe prospero_capture SceSystemService)
target_compile_definitions(firmware_probe PRIVATE FIRMWARE_PROBE_COMMIT="$ENV{PROSPERO_COMMIT}")
prospero_probe(host_restart)
prospero_probe(udp_burst_spike)
prospero_probe(audio_spike prospero_capture prospero_pad)
prospero_probe(bitrate_spike prospero_capture)
prospero_probe(bitrate_steps_probe prospero_capture)
prospero_probe(dual_open_probe prospero_capture)
prospero_probe(ra_nid_probe prospero_ra)
prospero_probe(deps_probe dep::ssl dep::opus dep::enet dep::nanors)

if(EXISTS "${PROSPERO_DEPS}/lib/libboost_log.a")
    prospero_probe(boost_probe dep::ssl
        "${PROSPERO_DEPS}/lib/libboost_log.a" "${PROSPERO_DEPS}/lib/libboost_filesystem.a"
        "${PROSPERO_DEPS}/lib/libboost_thread.a" "${PROSPERO_DEPS}/lib/libboost_atomic.a")
    prospero_probe(eh_probe "${PROSPERO_DEPS}/lib/libboost_filesystem.a" "${PROSPERO_DEPS}/lib/libboost_atomic.a")
    target_include_directories(boost_probe SYSTEM PRIVATE "${PROSPERO_DEPS}/include")
    target_include_directories(eh_probe SYSTEM PRIVATE "${PROSPERO_DEPS}/include")
endif()

# cbs_probe links Sunshine's own, unmodified cbs.cpp. It is copied next to the build so that its
# quoted #include "logging.h" finds the probe stub instead of Sunshine's Boost.Log header.
if(EXISTS "${PROSPERO_DEPS}/lib/libavcodec.a")
    set(CBS_COPY "${CMAKE_BINARY_DIR}/cbs_probe/cbs.cpp")
    configure_file("${CMAKE_SOURCE_DIR}/.cache/deps/src/sunshine/src/cbs.cpp" "${CBS_COPY}" COPYONLY)
    set_source_files_properties("${CBS_COPY}" PROPERTIES COMPILE_OPTIONS "-w")
    add_executable(cbs_probe src/cbs_probe.cpp "${CBS_COPY}")
    target_include_directories(cbs_probe PRIVATE src/probe_stubs)
    target_include_directories(cbs_probe SYSTEM PRIVATE "${PROSPERO_DEPS}/include")
    target_compile_options(cbs_probe PRIVATE "SHELL:-idirafter ${CMAKE_SOURCE_DIR}/.cache/deps/src/sunshine/src")
    set_source_files_properties(src/cbs_probe.cpp PROPERTIES COMPILE_OPTIONS "${PROSPERO_WARNINGS}")
    target_link_libraries(cbs_probe PRIVATE "${PROSPERO_DEPS}/lib/libavcodec.a" "${PROSPERO_DEPS}/lib/libavutil.a" pthread)
endif()

# The host: Sunshine's protocol core (pinned, patched sources from .cache/deps/src) with the PS5
# replacements in src/sunshine. Built once Boost and FFmpeg are present in out/deps.
set(SUNSHINE_SRC "${CMAKE_SOURCE_DIR}/.cache/deps/src/sunshine" CACHE PATH "Extracted Sunshine tree")
set(MOONLIGHT_SRC "${CMAKE_SOURCE_DIR}/.cache/deps/src/moonlight-common-c" CACHE PATH "moonlight-common-c tree")
if(EXISTS "${PROSPERO_DEPS}/include/boost" AND EXISTS "${PROSPERO_DEPS}/lib/libavcodec.a")
    file(GLOB BOOST_LIBS "${PROSPERO_DEPS}/lib/libboost_*.a")
    set(SUNSHINE_KEPT
        nvhttp.cpp httpcommon.cpp rtsp.cpp stream.cpp crypto.cpp network.cpp audio.cpp input.cpp config.cpp
        logging.cpp cbs.cpp file_handler.cpp globals.cpp stat_trackers.cpp)
    list(TRANSFORM SUNSHINE_KEPT PREPEND "${SUNSHINE_SRC}/src/")
    # Cover art for the single app, embedded so the host can install it without a separate upload.
    set(COVER_PNG "${CMAKE_SOURCE_DIR}/assets/ps5_screen.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COVER_PNG}")
    file(READ "${COVER_PNG}" COVER_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," COVER_BYTES "${COVER_HEX}")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/cover_png.cpp" CONTENT
        "// Generated from assets/ps5_screen.png by CMakeLists.txt\n#include <cstddef>\nextern const unsigned char prospero_cover_png[] = {@COVER_BYTES@};\nextern const std::size_t prospero_cover_png_size = sizeof prospero_cover_png;\n")
    # The web page's files (src/web/page), embedded the same way; web_server.cpp serves them by path.
    set(WEB_PAGE_FILES index.html text/html app.js text/javascript style.css text/css)
    set(WEB_ARRAYS "")
    set(WEB_TABLE "")
    set(WEB_INDEX 0)
    list(LENGTH WEB_PAGE_FILES WEB_PAGE_LENGTH)
    while(WEB_INDEX LESS WEB_PAGE_LENGTH)
        list(GET WEB_PAGE_FILES ${WEB_INDEX} WEB_NAME)
        math(EXPR WEB_TYPE_INDEX "${WEB_INDEX} + 1")
        list(GET WEB_PAGE_FILES ${WEB_TYPE_INDEX} WEB_TYPE)
        set(WEB_FILE "${CMAKE_SOURCE_DIR}/src/web/page/${WEB_NAME}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WEB_FILE}")
        file(READ "${WEB_FILE}" WEB_HEX HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," WEB_BYTES "${WEB_HEX}")
        string(APPEND WEB_ARRAYS "static const unsigned char web_${WEB_INDEX}[] = {${WEB_BYTES}};
")
        string(APPEND WEB_TABLE "    {\"/${WEB_NAME}\", \"${WEB_TYPE}; charset=utf-8\", web_${WEB_INDEX}, sizeof web_${WEB_INDEX}},
")
        math(EXPR WEB_INDEX "${WEB_INDEX} + 2")
    endwhile()
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/web_page.cpp" CONTENT
        "// Generated from src/web/page by CMakeLists.txt
#include <cstddef>
struct prospero_web_asset {
    const char *path, *type;
    const unsigned char *data;
    std::size_t size;
};
@WEB_ARRAYS@extern const prospero_web_asset prospero_web_assets[] = {
@WEB_TABLE@};
extern const std::size_t prospero_web_asset_count = sizeof prospero_web_assets / sizeof prospero_web_assets[0];
")
    # The relaunch helper payload, embedded for "Restart host" on the web page (src/relaunch_helper.cpp).
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/relaunch_helper_elf.cpp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:relaunch_helper>"
            "-DOUTPUT=${CMAKE_BINARY_DIR}/generated/relaunch_helper_elf.cpp" -DSYMBOL=prospero_relaunch_helper
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake"
        DEPENDS relaunch_helper "${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake")
    add_executable(prospero_sunshine "${CMAKE_BINARY_DIR}/generated/cover_png.cpp"
        "${CMAKE_BINARY_DIR}/generated/relaunch_helper_elf.cpp"
        "${CMAKE_BINARY_DIR}/generated/web_page.cpp" src/web/web_server.cpp src/web/web_login.cpp src/web/web_report.cpp
        src/web/web_cert.cpp
        app/src/psapp/conf.cpp app/src/psapp/diag.cpp
        ${SUNSHINE_KEPT}
        "${MOONLIGHT_SRC}/src/RtspParser.c"
        src/sunshine/main_ps5.cpp src/sunshine/video_ps5.cpp src/sunshine/process_ps5.cpp
        src/sunshine/platform_ps5.cpp src/sunshine/stubs_ps5.cpp src/control/control_server.cpp
        src/control/autoload_host.cpp src/control/file_bridge.cpp app/src/psapp/autoload.cpp src/apps/titles.cpp src/platform/ps5/title_launch.c
        src/platform/ps5/console_users.c src/platform/ps5/profile_seat.c)
    # Sunshine includes its own headers as "src/..." and "..."; FFmpeg's internal headers include a
    # "config.h" that must not resolve to Sunshine's, hence -idirafter for the Sunshine tree.
    target_include_directories(prospero_sunshine PRIVATE src "${SUNSHINE_SRC}" "${CMAKE_BINARY_DIR}/third-party")
    target_include_directories(prospero_sunshine SYSTEM PRIVATE "${PROSPERO_DEPS}/include" "${PROSPERO_DEPS}/include/nanors")
    target_compile_options(prospero_sunshine PRIVATE "SHELL:-idirafter ${SUNSHINE_SRC}/src")
    # Sunshine expects moonlight-common-c headers under <moonlight-common-c/src/...>.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third-party")
    file(CREATE_LINK "${MOONLIGHT_SRC}" "${CMAKE_BINARY_DIR}/third-party/moonlight-common-c" SYMBOLIC)
    # The autoload list's text edits are the app's; it includes them as "app/psapp/...".
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third-party/app")
    file(CREATE_LINK "${CMAKE_SOURCE_DIR}/app/src/psapp" "${CMAKE_BINARY_DIR}/third-party/app/psapp" SYMBOLIC)
    # scripts/build.sh finds the commit; host_diag.log and the app's report name it.
    set(PROSPERO_COMMIT "$ENV{PROSPERO_COMMIT}")
    if(NOT PROSPERO_COMMIT)
        set(PROSPERO_COMMIT "unknown")
    endif()
    target_compile_definitions(prospero_sunshine PRIVATE
        PROJECT_NAME="ProsperoSunshine" PROJECT_VERSION="0.4.1" PROJECT_VERSION_MAJOR="0"
        PROJECT_VERSION_MINOR="4" PROJECT_VERSION_PATCH="1" PROJECT_VERSION_COMMIT="${PROSPERO_COMMIT}"
        PROJECT_FQDN="dev.prosperosunshine" SUNSHINE_ASSETS_DIR="/data/prosperosunshine/assets"
        SUNSHINE_PUBLISHER_NAME="ProsperoSunshine" SUNSHINE_PUBLISHER_WEBSITE=""
        SUNSHINE_PUBLISHER_ISSUE_URL="" SUNSHINE_TRAY=0 BOOST_ALL_NO_LIB)
    target_link_libraries(prospero_sunshine PRIVATE prospero_core prospero_pad prospero_mdns
        "-Wl,--start-group" ${BOOST_LIBS} "${PROSPERO_DEPS}/lib/libdisplaydevice_common.a"
        "${PROSPERO_DEPS}/lib/libavcodec.a" "${PROSPERO_DEPS}/lib/libswscale.a" "${PROSPERO_DEPS}/lib/libavutil.a"
        dep::ssl dep::crypto dep::opus dep::enet dep::nanors "-Wl,--end-group" pthread SceSystemService SceUserService)
endif()
