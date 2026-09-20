cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()
project(ReaWebAPI VERSION 0.2.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(REAWEB_BUILD_EXTENSION "Build the REAPER extension" ON)
option(BUILD_TESTING "Build regression tests (included in source)" OFF)
include(FetchContent)

FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03)
FetchContent_MakeAvailable(json)

# Read-only local App resources. Browser networking remains native to the WebView.
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG 278c2979e8c68468960c3073e28e1c51b098d6a4)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(reaper_sdk
  GIT_REPOSITORY https://github.com/justinfrankel/reaper-sdk.git
  GIT_TAG 08e885d14c87c2973ec301d220ff9fe6b6a7cbeb)
FetchContent_Declare(wdl
  GIT_REPOSITORY https://github.com/justinfrankel/WDL.git
  GIT_TAG 8f4d783de745126ac8c201455dc30818c8613324)
FetchContent_MakeAvailable(reaper_sdk wdl)
# The official SDK uses a sibling ../WDL include.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk-layout/sdk")
configure_file("${reaper_sdk_SOURCE_DIR}/sdk/reaper_plugin.h"
  "${CMAKE_CURRENT_BINARY_DIR}/sdk-layout/sdk/reaper_plugin.h" COPYONLY)
configure_file("${reaper_sdk_SOURCE_DIR}/sdk/reaper_plugin_functions.h"
  "${CMAKE_CURRENT_BINARY_DIR}/sdk-layout/sdk/reaper_plugin_functions.h" COPYONLY)
file(COPY "${wdl_SOURCE_DIR}/WDL/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/sdk-layout/WDL")

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
# Offline and deterministic: builds never fetch or silently update API definitions.
file(GLOB REAWEB_SYNC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/api_sync/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${REAWEB_SYNC_SOURCES} tools/native_bindings.py api/reaper_api.json api/bindings.json src/core/core.cpp
  runtime/reaper.js runtime/reaper.d.ts runtime/reaper-api.generated.js runtime/reaper-api.generated.d.ts
  docs/api-reference.md)
execute_process(COMMAND "${Python3_EXECUTABLE}" -m tools.api_sync export-runtime
  --output "${CMAKE_CURRENT_BINARY_DIR}/generated/api_schema.hpp"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE REAWEB_SCHEMA_RESULT)
if(NOT REAWEB_SCHEMA_RESULT EQUAL 0)
  message(FATAL_ERROR "API schema validation failed. Run python -m tools.api_sync verify.")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" tools/native_bindings.py
  --output "${CMAKE_CURRENT_BINARY_DIR}/generated/native_api.cpp"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE REAWEB_NATIVE_RESULT)
if(NOT REAWEB_NATIVE_RESULT EQUAL 0)
  message(FATAL_ERROR "Native API compilation failed. No incomplete bindings are emitted.")
endif()

find_package(Threads REQUIRED)
add_library(reaweb_core STATIC
  src/core/core.cpp
  src/core/batch.cpp
  src/core/native.cpp
  src/core/worker.cpp
  src/runtime/services.cpp
  src/runtime/app.cpp
  src/runtime/audio.cpp
  src/runtime/drag_drop.cpp
  src/runtime/fs.cpp
  src/runtime/system.cpp
  src/runtime/theme.cpp
  src/web/web_resources.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/native_api.cpp")
target_include_directories(reaweb_core PUBLIC src)
target_include_directories(reaweb_core SYSTEM PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sdk-layout/sdk")
target_compile_definitions(reaweb_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(NOT WIN32)
  target_compile_definitions(reaweb_core PRIVATE SWELL_PROVIDED_BY_APP)
endif()
target_include_directories(reaweb_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(reaweb_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads PRIVATE httplib::httplib)
target_compile_definitions(reaweb_core PUBLIC REAWEB_VERSION="${PROJECT_VERSION}")

# Share the same session implementation between the plugin and native tests.
add_library(reaweb_runtime STATIC
  src/runtime/runtime.cpp
  src/runtime/window.cpp
  src/runtime/events.cpp
  src/runtime/lifecycle.cpp
  src/runtime/debug.cpp
  src/runtime/transaction.cpp)
target_link_libraries(reaweb_runtime PUBLIC reaweb_core)
target_include_directories(reaweb_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(REAWEB_BUILD_EXTENSION)

  add_library(reawebapi MODULE src/plugin/plugin.cpp)
  target_link_libraries(reawebapi PRIVATE reaweb_runtime)
  target_compile_definitions(reawebapi PRIVATE NOMINMAX)
  target_include_directories(reawebapi PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_include_directories(reawebapi SYSTEM PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sdk-layout/sdk")
  target_include_directories(reawebapi SYSTEM PRIVATE "${wdl_SOURCE_DIR}/WDL")
  set_target_properties(reawebapi PROPERTIES PREFIX "")

  if(WIN32)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
      message(FATAL_ERROR "Windows builds require the MSVC x64 toolchain")
    endif()
    FetchContent_Declare(webview2
      URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
      URL_HASH SHA256=ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(webview2)
    target_sources(reawebapi PRIVATE src/platform/windows/platform_win.cpp)
    target_include_directories(reawebapi PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
    target_link_libraries(reawebapi PRIVATE
      "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
      ole32 oleaut32 user32 shlwapi shell32 version advapi32 uuid)
    target_compile_definitions(reawebapi PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    set_target_properties(reawebapi PROPERTIES OUTPUT_NAME reaper_reawebapi-x64)
  elseif(APPLE)
    enable_language(OBJCXX)
    set_target_properties(reawebapi PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_compile_definitions(reawebapi PRIVATE SWELL_PROVIDED_BY_APP)
    target_sources(reawebapi PRIVATE src/platform/macos/platform_mac.mm src/platform/shared/swell_window.cpp "${wdl_SOURCE_DIR}/WDL/swell/swell-modstub.mm")
    set_source_files_properties(src/platform/macos/platform_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(reawebapi PRIVATE "-framework Cocoa" "-framework WebKit")
    if(CMAKE_OSX_ARCHITECTURES)
      set(REAWEB_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    else()
      set(REAWEB_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT REAWEB_ARCH MATCHES "^(arm64|x86_64)$")
      message(FATAL_ERROR "Build one macOS architecture at a time: arm64 or x86_64")
    endif()
    set_target_properties(reawebapi PROPERTIES OUTPUT_NAME "reaper_reawebapi-${REAWEB_ARCH}" SUFFIX .dylib)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1>=2.40)
    find_package(X11 REQUIRED)
    add_executable(reaweb_webkit src/platform/linux/linux_webkit.cpp)
    target_link_libraries(reaweb_webkit PRIVATE reaweb_core PkgConfig::WEBKIT X11::X11)
    target_compile_definitions(reawebapi PRIVATE SWELL_PROVIDED_BY_APP)
    target_sources(reawebapi PRIVATE src/platform/linux/platform_linux.cpp src/platform/shared/swell_window.cpp
      "${wdl_SOURCE_DIR}/WDL/swell/swell-modstub-generic.cpp")
    target_link_libraries(reawebapi PRIVATE ${CMAKE_DL_LIBS})
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      set(REAWEB_ARCH aarch64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
      set(REAWEB_ARCH x86_64)
    else()
      message(FATAL_ERROR "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set_target_properties(reawebapi PROPERTIES OUTPUT_NAME "reaper_reawebapi-${REAWEB_ARCH}")
    set_target_properties(reaweb_webkit PROPERTIES OUTPUT_NAME "reawebapi-webview-${REAWEB_ARCH}")
    target_compile_definitions(reawebapi PRIVATE REAWEB_HELPER_NAME="reawebapi-webview-${REAWEB_ARCH}")
    install(TARGETS reaweb_webkit RUNTIME DESTINATION UserPlugins)
  endif()
  install(TARGETS reawebapi LIBRARY DESTINATION UserPlugins RUNTIME DESTINATION UserPlugins)
  install(DIRECTORY web/ DESTINATION Scripts/ReaWebAPI/Example)
  # A single packager keeps installed docs, the standalone SDK and release bundles identical.
  install(CODE "
    execute_process(COMMAND \"${Python3_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/tools/package_sdk.py\"
      --stage \"\${CMAKE_INSTALL_PREFIX}\" RESULT_VARIABLE sdk_result)
    if(NOT sdk_result EQUAL 0)
      message(FATAL_ERROR \"SDK/documentation staging failed\")
    endif()
  ")
  install(FILES "${httplib_SOURCE_DIR}/LICENSE" DESTINATION ReaWebAPI/licenses RENAME cpp-httplib.txt)
  install(FILES "${json_SOURCE_DIR}/LICENSE.MIT" DESTINATION ReaWebAPI/licenses RENAME nlohmann-json.txt)
  install(FILES "${reaper_sdk_SOURCE_DIR}/sdk/reaper_plugin.h" DESTINATION ReaWebAPI/licenses)
  install(FILES "${wdl_SOURCE_DIR}/WDL/swell/swell.h" DESTINATION ReaWebAPI/licenses)
  if(WIN32)
    install(FILES "${webview2_SOURCE_DIR}/LICENSE.txt" DESTINATION ReaWebAPI/licenses RENAME WebView2.txt OPTIONAL)
    install(FILES "${webview2_SOURCE_DIR}/NOTICE.txt" DESTINATION ReaWebAPI/licenses RENAME WebView2-NOTICE.txt OPTIONAL)
  endif()
endif()

if(MSVC)
  target_compile_options(reaweb_core PRIVATE /W4 /utf-8)
  target_compile_options(reaweb_runtime PRIVATE /W4 /utf-8)
  if(TARGET reawebapi)
    target_compile_options(reawebapi PRIVATE /W4 /utf-8)
  endif()
else()
  target_compile_options(reaweb_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(reaweb_runtime PRIVATE -Wall -Wextra -Wpedantic)
  if(TARGET reawebapi)
    target_compile_options(reawebapi PRIVATE -Wall -Wextra)
  endif()
endif()

if(BUILD_TESTING)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    message(FATAL_ERROR "Missing regression test sources. Restore tests/ or configure with -DBUILD_TESTING=OFF.")
  endif()
  include(CTest)
  add_subdirectory(tests)
endif()
