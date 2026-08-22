cmake_minimum_required(VERSION 3.25)

project(mcsquared VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MC_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(MC_ENABLE_COVERAGE "Enable compiler coverage instrumentation" OFF)

if(MC_ENABLE_SANITIZERS AND MC_ENABLE_COVERAGE)
    message(FATAL_ERROR "Sanitizers and coverage cannot be enabled together")
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)

add_library(mc_project_options INTERFACE)
target_compile_features(mc_project_options INTERFACE cxx_std_20)
target_compile_options(mc_project_options INTERFACE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
)

if(MC_ENABLE_SANITIZERS)
    target_compile_options(mc_project_options INTERFACE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fsanitize=address,undefined;-fno-omit-frame-pointer>
        $<$<CXX_COMPILER_ID:MSVC>:/fsanitize=address>
    )
    target_link_options(mc_project_options INTERFACE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fsanitize=address,undefined>
        $<$<CXX_COMPILER_ID:MSVC>:/fsanitize=address>
    )
endif()

if(MC_ENABLE_COVERAGE)
    target_compile_options(mc_project_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU>:--coverage;-fno-omit-frame-pointer>
        $<$<CXX_COMPILER_ID:AppleClang,Clang>:-fprofile-instr-generate;-fcoverage-mapping>
    )
    target_link_options(mc_project_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU>:--coverage>
        $<$<CXX_COMPILER_ID:AppleClang,Clang>:-fprofile-instr-generate>
    )
endif()

add_subdirectory(include)
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()