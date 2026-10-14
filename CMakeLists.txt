cmake_minimum_required(VERSION 3.16)
project(hw2_overlay C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)

# Optional: X11/RandR lets the overlay find the game window and its monitor so it
# lands on the right screen and lines up with a windowed / non-native-resolution game.
find_package(X11)
if(X11_FOUND AND X11_Xrandr_FOUND)
    set(HAVE_X11 TRUE)
    message(STATUS "X11 + RandR found - game window auto-detection enabled")
else()
    message(STATUS "X11/RandR not found - overlay will assume it fills the output")
endif()

add_executable(hw2_overlay src/hw2_overlay.cpp)

# Shared sibling libraries (local checkouts, not submodules).
set(OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Wayland_Game_Overlay)
set(PHM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../PhysicalHidMouse)
if(NOT EXISTS ${OVERLAY_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "Wayland_Game_Overlay not found at ${OVERLAY_DIR}")
endif()
if(NOT EXISTS ${PHM_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "PhysicalHidMouse not found at ${PHM_DIR}")
endif()
add_subdirectory(${OVERLAY_DIR} wayland_overlay_build)
add_subdirectory(${PHM_DIR} physical_hid_mouse_build)

target_include_directories(hw2_overlay PRIVATE src)
target_link_libraries(hw2_overlay PRIVATE wayland_overlay physical_hid_mouse pthread)
if(HAVE_X11)
    target_compile_definitions(hw2_overlay PRIVATE HAVE_X11)
    target_include_directories(hw2_overlay PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(hw2_overlay PRIVATE ${X11_LIBRARIES} ${X11_Xrandr_LIB})
endif()
