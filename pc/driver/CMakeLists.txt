find_path(OPENVR_INCLUDE_DIR openvr_driver.h REQUIRED)
add_library(pocketvr_driver_core STATIC state.cpp input.cpp encoded_dump.cpp d3d.cpp hmd.cpp compositor.cpp network.cpp encoder/encoder.cpp encoder/scaled_encoder.cpp encoder/amf_encoder.cpp encoder/nvenc_encoder.cpp encoder/mf_encoder.cpp encoder/mf_decoder.cpp encoder/pipeline.cpp)
target_include_directories(pocketvr_driver_core PUBLIC .)
target_include_directories(pocketvr_driver_core SYSTEM PUBLIC "${OPENVR_INCLUDE_DIR}")
if(NOT POCKETVR_SDK_ROOT OR NOT EXISTS "${POCKETVR_SDK_ROOT}/amf/core/Factory.h")
  message(FATAL_ERROR "Run scripts/fetch_sdks.py and set POCKETVR_SDK_ROOT to the verified SDK header cache (amf/core/Factory.h missing)")
endif()
target_include_directories(pocketvr_driver_core SYSTEM PRIVATE "${POCKETVR_SDK_ROOT}/amf" "${POCKETVR_SDK_ROOT}/nvenc")
target_link_libraries(pocketvr_driver_core PUBLIC pocketvr_windows d3d11 dxgi d3dcompiler mfplat mf mfuuid strmiids oleaut32 wmcodecdspuuid)
add_library(driver_pocketvr SHARED driver.cpp)
target_link_libraries(driver_pocketvr PRIVATE pocketvr_driver_core xinput9_1_0 winmm)
set_target_properties(driver_pocketvr PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/pocketvr/bin/win64")
add_custom_command(TARGET driver_pocketvr POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest" "${CMAKE_BINARY_DIR}/package/pocketvr/driver.vrdrivermanifest"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${CMAKE_BINARY_DIR}/package/pocketvr/resources"
  VERBATIM)
# Refresh legal assets even when only notices/dependency metadata have changed.
add_custom_target(pocketvr_package_notices ALL
  DEPENDS driver_pocketvr
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${CMAKE_BINARY_DIR}/package/pocketvr/"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/third_party/licenses" "${CMAKE_BINARY_DIR}/package/pocketvr/third_party/licenses"
  VERBATIM)
# The driver POST_BUILD copy only runs when the driver itself is rebuilt, so an
# edited input profile or binding file would otherwise ship stale. Refresh the
# resource tree on every build.
add_custom_target(pocketvr_package_resources ALL
  DEPENDS driver_pocketvr
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${CMAKE_BINARY_DIR}/package/pocketvr/resources"
  VERBATIM)
if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  add_executable(pocketvr_driver_tests tests/driver_tests.cpp tests/input_tests.cpp tests/compositor_tests.cpp tests/encoder_tests.cpp tests/encoded_dump_tests.cpp tests/input_profile_tests.cpp)
  target_link_libraries(pocketvr_driver_tests PRIVATE pocketvr_driver_core GTest::gtest_main)
  target_compile_definitions(pocketvr_driver_tests PRIVATE POCKETVR_TEST_DRIVER_DLL="$<TARGET_FILE:driver_pocketvr>")
  target_compile_definitions(pocketvr_driver_tests PRIVATE POCKETVR_TEST_ARTIFACT_ROOT="${CMAKE_BINARY_DIR}/test-artifacts")
  # The input profile and binding files are validated as shipped resources.
  target_compile_definitions(pocketvr_driver_tests PRIVATE POCKETVR_TEST_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
  target_compile_definitions(pocketvr_driver_tests PRIVATE POCKETVR_TEST_PACKAGE_ROOT="${CMAKE_BINARY_DIR}/package/pocketvr")
  add_dependencies(pocketvr_driver_tests driver_pocketvr)
  add_dependencies(pocketvr_driver_tests pocketvr_package_resources)
  include(GoogleTest)
  gtest_discover_tests(pocketvr_driver_tests DISCOVERY_TIMEOUT 60)
endif()
