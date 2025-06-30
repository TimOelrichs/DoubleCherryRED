cmake_minimum_required(VERSION 3.15)
project(DoubleCherryRED LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler-Warnungen setzen
if (MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Quellen und Header finden
file(GLOB_RECURSE PROJECT_SOURCES
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/src/*.c
  ${CMAKE_SOURCE_DIR}/include/*.hpp
  ${CMAKE_SOURCE_DIR}/include/*.h
)

# Weitere externe Includes (z. B. libretro-common)
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/external/libretro-common/include
  ${CMAKE_CURRENT_SOURCE_DIR}/external/DoubleCherryEngine/include
)

# Library-Ziel definieren
add_library(${PROJECT_NAME} SHARED ${PROJECT_SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "DoubleCherryRED_libretro"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/cores
)

# Includes für dein Projekt
target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Verlinke mit header-only Engine
target_link_libraries(${PROJECT_NAME} PRIVATE DoubleCherryEngine)

# Windows-spezifisch: Warnung unterdrücken
if(MSVC)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
