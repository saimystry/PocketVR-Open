find_package(imgui CONFIG REQUIRED)
find_path(OPENVR_OVERLAY_INCLUDE openvr.h REQUIRED)
find_library(OPENVR_API_LIBRARY NAMES openvr_api REQUIRED)
add_library(pocketvr_dashboard_model STATIC model.cpp)
target_link_libraries(pocketvr_dashboard_model PUBLIC pocketvr_windows)
target_include_directories(pocketvr_dashboard_model PUBLIC . "${OPENVR_OVERLAY_INCLUDE}")
add_executable(pocketvr_overlay WIN32 main.cpp graphics.cpp ui.cpp)
target_compile_definitions(pocketvr_overlay PRIVATE UNICODE _UNICODE)
target_link_libraries(pocketvr_overlay PRIVATE pocketvr_dashboard_model imgui::imgui
  "${OPENVR_API_LIBRARY}" d3d11 dxgi d3dcompiler user32 gdi32 dwmapi windowscodecs shlwapi)
add_custom_command(TARGET pocketvr_overlay POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/app.vrmanifest" "$<TARGET_FILE_DIR:pocketvr_overlay>/app.vrmanifest"
  VERBATIM)
get_filename_component(_openvr_libdir "${OPENVR_API_LIBRARY}" DIRECTORY)
find_file(OPENVR_OVERLAY_DLL openvr_api.dll PATHS "${_openvr_libdir}/../bin" "${_openvr_libdir}/../../bin" REQUIRED NO_DEFAULT_PATH)
add_custom_target(pocketvr_package_overlay ALL
  DEPENDS pocketvr_overlay
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/app.vrmanifest" "$<TARGET_FILE_DIR:pocketvr_overlay>/app.vrmanifest"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/package/pocketvr/overlay"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:pocketvr_overlay>" "${OPENVR_OVERLAY_DLL}" "${CMAKE_CURRENT_SOURCE_DIR}/app.vrmanifest" "${CMAKE_BINARY_DIR}/package/pocketvr/overlay/"
  VERBATIM)
if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  add_executable(pocketvr_dashboard_tests tests.cpp)
  target_link_libraries(pocketvr_dashboard_tests PRIVATE pocketvr_dashboard_model GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(pocketvr_dashboard_tests DISCOVERY_TIMEOUT 60)
  add_test(NAME dashboard-offscreen-render COMMAND pocketvr_overlay --self-test "${CMAKE_BINARY_DIR}/test-artifacts/dashboard")
  set_tests_properties(dashboard-offscreen-render PROPERTIES TIMEOUT 60)
  add_test(NAME dashboard-preview-startup COMMAND pocketvr_overlay --preview-smoke-test)
  set_tests_properties(dashboard-preview-startup PROPERTIES TIMEOUT 20)
endif()
