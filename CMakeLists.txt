cmake_minimum_required(VERSION 3.14)
project(CircularPiCamProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(TIFF REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)
pkg_check_modules(LIBGPIOD  REQUIRED libgpiod)

include_directories(
    ${LIBCAMERA_INCLUDE_DIRS}
)

add_executable(simple_capture src/SimpleCapture/main.cpp)

target_link_libraries(simple_capture
    ${LIBCAMERA_LIBRARIES}
    ${LIBGPIOD_LIBRARIES}
    ${TIFF_LIBRARIES}
)
