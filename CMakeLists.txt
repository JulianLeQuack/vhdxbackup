# Build mit MSVC (Visual Studio 2022 / Build Tools) und Windows SDK:
#   cmake -B build -A x64
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(VhdxBackup LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Statische Laufzeit (/MT): eine einzelne exe ohne VC-Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(VB_WERROR "Warnungen als Fehler (/WX)" OFF)
option(VB_ASAN "AddressSanitizer (nur Test-Builds)" OFF)

file(GLOB SOURCES src/*.cpp)
add_executable(vhdxbackup ${SOURCES} src/vhdxbackup.rc)
target_compile_definitions(vhdxbackup PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX)
target_link_libraries(vhdxbackup PRIVATE vssapi virtdisk bcrypt ole32 oleaut32 wbemuuid odbc32 uuid advapi32)

if(MSVC)
  target_compile_options(vhdxbackup PRIVATE /W4 /permissive- /utf-8 /EHsc)
  if(VB_WERROR)
    target_compile_options(vhdxbackup PRIVATE /WX)
  endif()
  if(VB_ASAN)
    target_compile_options(vhdxbackup PRIVATE /fsanitize=address)
  endif()
  # Manifest kommt aus der .rc-Datei (requireAdministrator).
  target_link_options(vhdxbackup PRIVATE /MANIFEST:NO)
endif()
