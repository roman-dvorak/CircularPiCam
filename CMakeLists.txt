cmake_minimum_required(VERSION 3.14)
project(CircularPiCamProject)

# Nastav standard C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Najdi OpenCV
find_package(OpenCV REQUIRED)

find_package(TIFF REQUIRED)

# Najdi libcamera přes pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# Přidej include adresáře
include_directories(
    ${OpenCV_INCLUDE_DIRS}
    ${LIBCAMERA_INCLUDE_DIRS}
)

message(STATUS "LIBCAMERA_INCLUDE_DIRS: ${LIBCAMERA_INCLUDE_DIRS}")

# Přidej binárku
add_executable(simple_capture src/SimpleCapture/main.cpp)

# Propoj knihovny
target_link_libraries(simple_capture
    ${OpenCV_LIBS}
    ${LIBCAMERA_LIBRARIES}
    ${TIFF_LIBRARIES}
)

