cmake_minimum_required(VERSION 3.20)
project(tickforge VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TF_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(TF_NATIVE "Tune for the build machine (-march=native)" OFF)

# -Werror is on by default for GCC/Clang (CI-verified). For MSVC it defaults
# off: /W4 coverage differs, so pass -DTF_WERROR=ON to opt in.
if(MSVC)
  option(TF_WERROR "Treat compiler warnings as errors" OFF)
else()
  option(TF_WERROR "Treat compiler warnings as errors" ON)
endif()

# ---------------------------------------------------------------- warnings
add_library(tf_warnings INTERFACE)
if(MSVC)
  # /utf-8: sources contain UTF-8 punctuation in comments and strings.
  target_compile_options(tf_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(TF_WERROR)
    target_compile_options(tf_warnings INTERFACE /WX)
  endif()
  # fopen/strtoll etc. are standard C++; silence MSVC's "use _s variants" nags.
  target_compile_definitions(tf_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tf_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wnull-dereference
    -Wdouble-promotion -Wformat=2)
  if(TF_WERROR)
    target_compile_options(tf_warnings INTERFACE -Werror)
  endif()
endif()

if(TF_SANITIZE)
  if(MSVC)
    # MSVC ships AddressSanitizer only (no UBSan).
    add_compile_options(/fsanitize=address /Zi)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()
if(TF_NATIVE)
  if(MSVC)
    add_compile_options(/arch:AVX2)
  else()
    add_compile_options(-march=native)
  endif()
endif()

# ---------------------------------------------------------------- library
add_library(tickforge INTERFACE)
target_include_directories(tickforge INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(tickforge INTERFACE cxx_std_20)

# C ABI shared library consumed by the Python package via ctypes.
# On Windows a DLL is a RUNTIME artifact, not a LIBRARY one; the `$<0:>`
# generator expression stops multi-config generators (Visual Studio) from
# appending a Release/ or Debug/ subdirectory, so Python always finds it.
set(TF_PY_PKG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/python/tickforge")
add_library(tickforge_c SHARED src/c_api.cpp)
target_link_libraries(tickforge_c PRIVATE tickforge tf_warnings)
target_compile_definitions(tickforge_c PRIVATE TF_BUILDING_DLL)
set_target_properties(tickforge_c PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${TF_PY_PKG_DIR}$<0:>"
  RUNTIME_OUTPUT_DIRECTORY "${TF_PY_PKG_DIR}$<0:>"
  CXX_VISIBILITY_PRESET hidden)
if(MINGW)
  # Bundle libstdc++/libgcc so the DLL loads without MinGW on PATH.
  target_link_options(tickforge_c PRIVATE -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------- tools
foreach(tool tf_gen tf_bench tf_replay)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE tickforge tf_warnings)
  # Put tools in the build root for every generator, so ./build/tf_bench
  # (or build\tf_bench.exe) works the same with Make, Ninja or Visual Studio.
  set_target_properties(${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}$<0:>")
endforeach()

# ---------------------------------------------------------------- tests
include(CTest)
if(BUILD_TESTING)
  foreach(t test_order_book test_structures test_differential test_validation)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE tickforge tf_warnings)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
