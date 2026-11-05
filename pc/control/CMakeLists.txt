find_package(unofficial-webview2 CONFIG REQUIRED)
add_executable(pocketvr_control WIN32 main.cpp setup_qr_window.cpp pocketvr.rc)
target_link_libraries(pocketvr_control PRIVATE pocketvr_windows unofficial::webview2::webview2 user32 gdi32 comdlg32 dwmapi d3d11 dxgi)
target_compile_definitions(pocketvr_control PRIVATE UNICODE _UNICODE)
# The interface files sit beside the app, in both the build and the package.
add_custom_command(TARGET pocketvr_control POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:pocketvr_control>/ui"
  VERBATIM)
add_executable(pocketvr_input WIN32 input_pad.cpp)
target_link_libraries(pocketvr_input PRIVATE pocketvr_windows user32 gdi32 xinput9_1_0 winmm)
target_compile_definitions(pocketvr_input PRIVATE UNICODE _UNICODE)
add_executable(pocketvr_usb WIN32 usb_wizard.cpp)
target_link_libraries(pocketvr_usb PRIVATE pocketvr_windows user32 gdi32 comdlg32)
target_compile_definitions(pocketvr_usb PRIVATE UNICODE _UNICODE)
add_executable(pocketvr_benchmark WIN32 benchmark_window.cpp)
target_link_libraries(pocketvr_benchmark PRIVATE pocketvr_windows user32 gdi32 comdlg32 comctl32)
target_compile_definitions(pocketvr_benchmark PRIVATE UNICODE _UNICODE)
add_dependencies(pocketvr_benchmark pocketvr_encoder_bench)

if(BUILD_TESTING)
  add_test(NAME setup-qr-window-startup COMMAND pocketvr_control --qr-smoke-test)
  set_tests_properties(setup-qr-window-startup PROPERTIES TIMEOUT 15)
endif()
