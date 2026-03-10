cmake_minimum_required(VERSION 3.16)
project(inconsole_runtime_x86 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL REQUIRED sdl)
pkg_check_modules(FREETYPE2 REQUIRED freetype2)
pkg_check_modules(LIBPNG REQUIRED libpng)

add_executable(inconsole-runtime-x86
    src/main.cpp
    src/runtime_core.cpp
    src/runtime_input.cpp
    src/runtime_renderer.cpp
    src/runtime_scenes.cpp
)

target_compile_options(inconsole-runtime-x86 PRIVATE -O2 -Wall -Wextra)

target_include_directories(inconsole-runtime-x86 PRIVATE
    ${SDL_INCLUDE_DIRS}
    ${FREETYPE2_INCLUDE_DIRS}
    ${LIBPNG_INCLUDE_DIRS}
)

target_link_directories(inconsole-runtime-x86 PRIVATE
    ${SDL_LIBRARY_DIRS}
    ${FREETYPE2_LIBRARY_DIRS}
    ${LIBPNG_LIBRARY_DIRS}
)

target_link_libraries(inconsole-runtime-x86 PRIVATE
    ${SDL_LIBRARIES}
    ${FREETYPE2_LIBRARIES}
    ${LIBPNG_LIBRARIES}
    pthread
)
