cmake_minimum_required(VERSION 3.10)
project(wiimote_test)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-libusb)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(wiimote_test main.cpp)
target_include_directories(wiimote_test PRIVATE ${HIDAPI_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS})
target_link_libraries(wiimote_test PRIVATE ${HIDAPI_LIBRARIES} ${SDL2_LIBRARIES} pthread)
