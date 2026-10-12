cmake_minimum_required(VERSION 3.16)
project(a70_thermal VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- options
set(A70_WITH_ARAVIS    AUTO CACHE STRING "GigE Vision via Aravis (Linux): AUTO|ON|OFF")
set(A70_WITH_SPINNAKER AUTO CACHE STRING "GigE Vision via FLIR/Teledyne Spinnaker SDK: AUTO|ON|OFF")
option(A70_WITH_HDF5      "Radiometric video recording (HDF5)"            ON)
option(A70_WITH_MODBUS    "Modbus TCP server (libmodbus)"                 ON)
option(A70_BUILD_GUI      "Qt GUI"                                        ON)
option(A70_BUILD_TESTS    "Unit tests"                                    ON)
set(SPINNAKER_DIR "" CACHE PATH "Spinnaker SDK root (e.g. /opt/spinnaker or C:/Program Files/Teledyne/Spinnaker)")

if(MSVC)
  add_compile_options(/W3 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig QUIET)

# ---------------------------------------------------------------- core
add_library(a70core STATIC
  core/src/types.cpp
  core/src/json.cpp
  core/src/backend.cpp
  core/src/sim_backend.cpp
  core/src/aravis_backend.cpp
  core/src/spinnaker_backend.cpp
  core/src/camera.cpp
  core/src/thermal.cpp
  core/src/io.cpp
  core/src/recorder.cpp
  core/src/engine.cpp
  core/src/modbus_server.cpp
  core/src/watchdog.cpp
)
target_include_directories(a70core PUBLIC core/include)
target_link_libraries(a70core PUBLIC Threads::Threads ZLIB::ZLIB)
target_compile_definitions(a70core PUBLIC A70_VERSION="${PROJECT_VERSION}")

# Aravis
if(NOT A70_WITH_ARAVIS STREQUAL "OFF" AND PKG_CONFIG_FOUND)
  pkg_check_modules(ARAVIS IMPORTED_TARGET aravis-0.8)
  if(ARAVIS_FOUND)
    target_link_libraries(a70core PUBLIC PkgConfig::ARAVIS)
    target_compile_definitions(a70core PUBLIC A70_HAVE_ARAVIS=1)
    message(STATUS "a70: Aravis ${ARAVIS_VERSION} backend enabled")
  endif()
endif()
if(A70_WITH_ARAVIS STREQUAL "ON" AND NOT ARAVIS_FOUND)
  message(FATAL_ERROR "A70_WITH_ARAVIS=ON but aravis-0.8 not found (apt install libaravis-dev)")
endif()

# Spinnaker
if(NOT A70_WITH_SPINNAKER STREQUAL "OFF")
  find_path(SPINNAKER_INCLUDE_DIR Spinnaker.h
    HINTS ${SPINNAKER_DIR} ENV SPINNAKER_DIR
    PATH_SUFFIXES include include/spinnaker
    PATHS /opt/spinnaker /usr/include/spinnaker "C:/Program Files/Teledyne/Spinnaker" "C:/Program Files/FLIR Systems/Spinnaker")
  find_library(SPINNAKER_LIBRARY NAMES Spinnaker Spinnaker_v140 Spinnaker_v143
    HINTS ${SPINNAKER_DIR} ENV SPINNAKER_DIR
    PATH_SUFFIXES lib lib64 lib64/vs2015 lib64/vs2017 lib64/vs2019 lib64/vs2022
    PATHS /opt/spinnaker "C:/Program Files/Teledyne/Spinnaker" "C:/Program Files/FLIR Systems/Spinnaker")
  if(SPINNAKER_INCLUDE_DIR AND SPINNAKER_LIBRARY)
    target_include_directories(a70core PUBLIC ${SPINNAKER_INCLUDE_DIR})
    target_link_libraries(a70core PUBLIC ${SPINNAKER_LIBRARY})
    target_compile_definitions(a70core PUBLIC A70_HAVE_SPINNAKER=1)
    message(STATUS "a70: Spinnaker backend enabled (${SPINNAKER_LIBRARY})")
  elseif(A70_WITH_SPINNAKER STREQUAL "ON")
    message(FATAL_ERROR "A70_WITH_SPINNAKER=ON but Spinnaker SDK not found; set SPINNAKER_DIR")
  endif()
endif()

# HDF5
if(A70_WITH_HDF5)
  find_package(HDF5 COMPONENTS C)
  if(HDF5_FOUND)
    target_include_directories(a70core PUBLIC ${HDF5_INCLUDE_DIRS})
    target_link_libraries(a70core PUBLIC ${HDF5_C_LIBRARIES})
    target_compile_definitions(a70core PUBLIC A70_HAVE_HDF5=1 ${HDF5_DEFINITIONS})
    message(STATUS "a70: HDF5 ${HDF5_VERSION} recording enabled")
  else()
    message(WARNING "a70: HDF5 not found — video recording disabled (apt install libhdf5-dev / vcpkg install hdf5)")
  endif()
endif()

# libmodbus
if(A70_WITH_MODBUS)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(MODBUS IMPORTED_TARGET libmodbus)
  endif()
  if(MODBUS_FOUND)
    target_link_libraries(a70core PUBLIC PkgConfig::MODBUS)
    target_compile_definitions(a70core PUBLIC A70_HAVE_MODBUS=1)
    message(STATUS "a70: Modbus TCP server enabled (libmodbus ${MODBUS_VERSION})")
  else()
    find_path(MODBUS_INCLUDE_DIR modbus.h PATH_SUFFIXES modbus)
    find_library(MODBUS_LIBRARY NAMES modbus libmodbus)
    if(MODBUS_INCLUDE_DIR AND MODBUS_LIBRARY)
      target_include_directories(a70core PUBLIC ${MODBUS_INCLUDE_DIR})
      target_link_libraries(a70core PUBLIC ${MODBUS_LIBRARY})
      target_compile_definitions(a70core PUBLIC A70_HAVE_MODBUS=1)
      if(WIN32)
        target_link_libraries(a70core PUBLIC ws2_32)
      endif()
      message(STATUS "a70: Modbus TCP server enabled (${MODBUS_LIBRARY})")
    else()
      message(WARNING "a70: libmodbus not found — Modbus server disabled")
    endif()
  endif()
endif()

# ---------------------------------------------------------------- apps
add_executable(a70_cli cli/main.cpp)
target_link_libraries(a70_cli PRIVATE a70core)

if(A70_BUILD_GUI)
  find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets QUIET)
  if(QT_FOUND)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets REQUIRED)
    set(CMAKE_AUTOMOC ON)
    add_executable(a70_gui WIN32
      gui/main.cpp gui/main_window.cpp gui/main_window.h
      gui/thermal_view.cpp gui/thermal_view.h
      gui/feature_browser.cpp gui/feature_browser.h)
    target_link_libraries(a70_gui PRIVATE a70core Qt${QT_VERSION_MAJOR}::Widgets)
    message(STATUS "a70: GUI with Qt ${QT_VERSION}")
  else()
    message(WARNING "a70: Qt5/Qt6 Widgets not found — GUI not built")
  endif()
endif()

if(A70_BUILD_TESTS)
  enable_testing()
  add_executable(a70_tests tests/test_core.cpp)
  target_link_libraries(a70_tests PRIVATE a70core)
  add_test(NAME a70_tests COMMAND a70_tests)
endif()

install(TARGETS a70_cli RUNTIME DESTINATION bin)
if(TARGET a70_gui)
  install(TARGETS a70_gui RUNTIME DESTINATION bin)
endif()
