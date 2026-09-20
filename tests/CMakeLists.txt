add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE reaweb_core)
add_test(NAME core COMMAND core_tests)
add_executable(runtime_tests runtime_tests.cpp)
target_link_libraries(runtime_tests PRIVATE reaweb_runtime)
target_include_directories(runtime_tests PRIVATE "${PROJECT_BINARY_DIR}/generated")
add_test(NAME runtime COMMAND runtime_tests)
add_executable(window_icons window_icons.cpp)
target_link_libraries(window_icons PRIVATE reaweb_core)
add_test(NAME window_icons COMMAND window_icons)
if(WIN32)
  target_link_libraries(window_icons PRIVATE user32 gdi32)
  target_compile_definitions(window_icons PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(window_icons PRIVATE /utf-8)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_abi_generated.cpp"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_native_abi.py" "${CMAKE_CURRENT_BINARY_DIR}/native_abi_generated.cpp"
  DEPENDS generate_native_abi.py "${PROJECT_SOURCE_DIR}/api/reaper_api.json" "${PROJECT_SOURCE_DIR}/api/bindings.json"
          "${PROJECT_SOURCE_DIR}/tools/native_bindings.py"
  VERBATIM)
add_executable(native_abi "${CMAKE_CURRENT_BINARY_DIR}/native_abi_generated.cpp")
target_link_libraries(native_abi PRIVATE reaweb_core)
target_include_directories(native_abi SYSTEM PRIVATE "${PROJECT_BINARY_DIR}/sdk-layout/sdk")
target_compile_definitions(native_abi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(NOT WIN32)
  target_compile_definitions(native_abi PRIVATE SWELL_PROVIDED_BY_APP)
  # The generated test driver is intentionally huge. Test the optimized library,
  # without spending minutes optimizing the linear list of test assertions.
  target_compile_options(native_abi PRIVATE -O0)
endif()
add_test(NAME native_abi COMMAND native_abi)
if(MSVC)
  target_compile_options(core_tests PRIVATE /utf-8)
  target_compile_options(runtime_tests PRIVATE /utf-8)
endif()
find_program(NODE_EXECUTABLE node REQUIRED)
add_test(NAME bridge COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bridge.test.cjs")
add_test(NAME demo COMMAND "${NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/demo.test.cjs")
if(REAWEB_BUILD_EXTENSION)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME plugin_abi COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/plugin_smoke.py"
    --dll "$<TARGET_FILE:reawebapi>")
endif()

add_executable(host_features host_features.cpp)
target_link_libraries(host_features PRIVATE reaweb_core)
if(MSVC)
  target_compile_options(host_features PRIVATE /utf-8)
endif()
add_test(NAME host_features COMMAND host_features)
add_executable(audio_tests audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE reaweb_core)
target_include_directories(audio_tests SYSTEM PRIVATE "${PROJECT_BINARY_DIR}/sdk-layout/sdk")
target_compile_definitions(audio_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(NOT WIN32)
  target_compile_definitions(audio_tests PRIVATE SWELL_PROVIDED_BY_APP)
endif()

# Native UI integration tests are opt-in and excluded from ctest.
if(UNIX AND NOT APPLE AND REAWEB_BUILD_EXTENSION)
  add_executable(linux_window_icons EXCLUDE_FROM_ALL linux_window_icons.cpp)
  target_link_libraries(linux_window_icons PRIVATE reaweb_core PkgConfig::WEBKIT X11::X11 ${CMAKE_DL_LIBS})
  add_executable(linux_devtools EXCLUDE_FROM_ALL linux_devtools.cpp)
  target_link_libraries(linux_devtools PRIVATE reaweb_core PkgConfig::WEBKIT X11::X11)
endif()
if(WIN32 AND REAWEB_BUILD_EXTENSION)
  add_executable(windows_devtools EXCLUDE_FROM_ALL windows_devtools.cpp
    "${PROJECT_SOURCE_DIR}/src/platform/windows/platform_win.cpp")
  target_link_libraries(windows_devtools PRIVATE reaweb_core
    "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    ole32 oleaut32 user32 gdi32 shlwapi shell32 version advapi32 uuid)
  target_include_directories(windows_devtools PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
  target_compile_definitions(windows_devtools PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(windows_devtools PRIVATE /utf-8)
  add_executable(windows_native_drag EXCLUDE_FROM_ALL windows_native_drag.cpp
    "${PROJECT_SOURCE_DIR}/src/platform/windows/platform_win.cpp")
  target_link_libraries(windows_native_drag PRIVATE reaweb_runtime
    "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    ole32 oleaut32 user32 gdi32 shlwapi shell32 version advapi32 uuid)
  target_include_directories(windows_native_drag PRIVATE "${PROJECT_BINARY_DIR}/generated" "${webview2_SOURCE_DIR}/build/native/include")
  target_compile_definitions(windows_native_drag PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(windows_native_drag PRIVATE /utf-8)
endif()
add_test(NAME audio COMMAND audio_tests)
add_test(NAME python_contracts COMMAND "${Python3_EXECUTABLE}" -X utf8 -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}" -p "*_test.py")
set_tests_properties(python_contracts PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" ENVIRONMENT "PYTHONUTF8=1")

add_executable(web_resources_driver web_resources_driver.cpp)
target_link_libraries(web_resources_driver PRIVATE reaweb_core)
add_test(NAME web_resources COMMAND "${Python3_EXECUTABLE}" -X utf8
  "${CMAKE_CURRENT_SOURCE_DIR}/web_resources_contract.py" "$<TARGET_FILE:web_resources_driver>")
