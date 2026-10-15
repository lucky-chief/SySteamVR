cmake_minimum_required(VERSION 3.7.1)
project(shiyan_nakedmonitor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The OpenVR SDK is expected next to this folder. Override with -DOPENVR_ROOT=...
if(NOT DEFINED OPENVR_ROOT)
  get_filename_component(OPENVR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../openvr-master" ABSOLUTE)
endif()

if(NOT EXISTS "${OPENVR_ROOT}/headers/openvr_driver.h")
  message(FATAL_ERROR "OpenVR SDK not found at '${OPENVR_ROOT}'. Pass -DOPENVR_ROOT=<path>.")
endif()

if(CMAKE_SIZEOF_VOID_P MATCHES 8)
  set(ARCH_SUFFIX win64)
else()
  set(ARCH_SUFFIX win32)
endif()

find_library(OPENVR_API_LIBRARY
  NAMES openvr_api
  PATHS "${OPENVR_ROOT}/lib"
  PATH_SUFFIXES ${ARCH_SUFFIX}
  NO_DEFAULT_PATH)

if(NOT OPENVR_API_LIBRARY)
  message(FATAL_ERROR "openvr_api static library not found under '${OPENVR_ROOT}/lib/${ARCH_SUFFIX}'.")
endif()

set(OPENVR_UTILS "${OPENVR_ROOT}/samples/drivers/utils")

file(GLOB DRIVER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(driver_shiyan_nakedmonitor SHARED
  ${DRIVER_SOURCES}
  "${OPENVR_UTILS}/driverlog/driverlog.cpp")

target_include_directories(driver_shiyan_nakedmonitor PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${OPENVR_ROOT}/headers"
  "${OPENVR_UTILS}/driverlog")

target_link_libraries(driver_shiyan_nakedmonitor PRIVATE "${OPENVR_API_LIBRARY}")

if(WIN32)
  # EnumDisplayMonitors / GetMonitorInfo (window auto-placement).
  target_link_libraries(driver_shiyan_nakedmonitor PRIVATE user32)
endif()

# A driver DLL must live at <driver_name>/bin/<platform><arch>/driver_<driver_name>.dll
set_target_properties(driver_shiyan_nakedmonitor PROPERTIES
  OUTPUT_NAME "driver_shiyan_nakedmonitor"
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/${ARCH_SUFFIX}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/${ARCH_SUFFIX}")

# Multi-config generators (Visual Studio) otherwise append /<Config> to the
# output directory. Pin every configuration to the same folder so the DLL sits
# directly in bin/<platform><arch>/ as SteamVR expects.
foreach(OUTPUT_CONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(driver_shiyan_nakedmonitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${OUTPUT_CONFIG} "${CMAKE_CURRENT_SOURCE_DIR}/bin/${ARCH_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY_${OUTPUT_CONFIG} "${CMAKE_CURRENT_SOURCE_DIR}/bin/${ARCH_SUFFIX}")
endforeach()

# Eye-tracking SDK (BinocularCameraConfig.dll + its hv.dll dependency) must sit
# next to the driver DLL so the loader (and LOAD_WITH_ALTERED_SEARCH_PATH for
# hv.dll) finds them inside vrserver.
add_custom_command(TARGET driver_shiyan_nakedmonitor POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/eye_tracking/BinocularCameraConfig.dll"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/eye_tracking/hv.dll"
    "$<TARGET_FILE_DIR:driver_shiyan_nakedmonitor>"
  COMMENT "Copying eye-tracking SDK DLLs next to the driver")
