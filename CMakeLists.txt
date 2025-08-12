cmake_minimum_required(VERSION 3.16)
project(TradingEngine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler-specific flags for performance
if (MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi")
    add_compile_options(/W4)
else ()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
    add_compile_options(-Wall -Wextra)
endif ()

# Find required packages
find_package(Threads REQUIRED)

# Include directories
include_directories(src)

# Source files
set(SOURCES
        src/main.cpp
)

# Create executable
add_executable(TradingEngine ${SOURCES})

# Link libraries
target_link_libraries(TradingEngine
        Threads::Threads
)

# Platform-specific libraries
if (WIN32)
    target_link_libraries(TradingEngine ws2_32)
endif ()
