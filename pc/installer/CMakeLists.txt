# Stage only product executables, resources, reviewed notices and first-party helpers.
add_custom_target(pocketvr_package_apps ALL
  DEPENDS pocketvr_control pocketvr_input pocketvr_usb pocketvr_benchmark pocketvr_encoder_bench
          pocketvr_loopback pocketvr_netsim pocketvr_diagnostics pocketvr_stereo_scene pocketvr_package_overlay pocketvr_package_notices
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/package/pocketvr/control" "${CMAKE_BINARY_DIR}/package/pocketvr/tools" "${CMAKE_BINARY_DIR}/package/pocketvr/setup" "${CMAKE_BINARY_DIR}/package/pocketvr/docs"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:pocketvr_control>" "$<TARGET_FILE:pocketvr_input>" "$<TARGET_FILE:pocketvr_usb>" "$<TARGET_FILE:pocketvr_benchmark>" "${CMAKE_BINARY_DIR}/package/pocketvr/control/"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:pocketvr_encoder_bench>" "$<TARGET_FILE:pocketvr_loopback>" "$<TARGET_FILE:pocketvr_netsim>" "$<TARGET_FILE:pocketvr_diagnostics>" "$<TARGET_FILE:pocketvr_stereo_scene>" "${OPENVR_OVERLAY_DLL}" "${CMAKE_BINARY_DIR}/package/pocketvr/tools/"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/setup.ps1" "${CMAKE_CURRENT_SOURCE_DIR}/Registration.psm1" "${CMAKE_BINARY_DIR}/package/pocketvr/setup/"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/pc/control/ui" "${CMAKE_BINARY_DIR}/package/pocketvr/control/ui"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/docs" "${CMAKE_BINARY_DIR}/package/pocketvr/docs"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/scripts/install_driver.ps1" "${PROJECT_SOURCE_DIR}/scripts/uninstall_driver.ps1" "${PROJECT_SOURCE_DIR}/scripts/register_driver.ps1" "${CMAKE_BINARY_DIR}/package/pocketvr/setup/"
  VERBATIM)
if(BUILD_TESTING)
  find_program(POCKETVR_POWERSHELL NAMES powershell REQUIRED)
  add_test(NAME installer-transactions COMMAND "${POCKETVR_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests.ps1" -WorkRoot "${CMAKE_BINARY_DIR}/test-artifacts/installer")
  set_tests_properties(installer-transactions PROPERTIES TIMEOUT 45)
endif()
