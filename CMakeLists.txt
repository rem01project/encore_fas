cmake_minimum_required(VERSION 3.18)

project(encore_fas C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(FAS_WITH_BPF "Build libbpf from source and embed the eBPF backend" ON)

if(ANDROID)
    set(BPF_CLANG_DEFAULT "${CMAKE_C_COMPILER}")
else()
    set(BPF_CLANG_DEFAULT "clang")
endif()

set(BPF_CLANG "${BPF_CLANG_DEFAULT}" CACHE STRING "Clang executable used to compile the eBPF program")

add_subdirectory(lib)
add_subdirectory(example)
