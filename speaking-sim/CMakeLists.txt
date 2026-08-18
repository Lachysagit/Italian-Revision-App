cmake_minimum_required(VERSION 3.24)

project(speaking_sim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emits build/compile_commands.json, which clangd discovers automatically.
# This is what makes `#include "sim/config.hpp"` resolve in the editor.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Crow needs standalone Asio, which ships no CMake config of its own, so fetch
# it first and hand Crow the path via the cache variable its FindAsio module reads.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-38-2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(asio)
# NOTE: the repo's asio/include is a symlink to ../include, which Windows git
# checks out as a plain text file. Point at the real directory instead.
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/include" CACHE PATH "" FORCE)
if(NOT EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
    message(FATAL_ERROR "asio.hpp not found under ${ASIO_INCLUDE_DIR}")
endif()

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.3.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Crow)

# Add each .cpp here as it is written. Deliberately not a file(GLOB) — CMake
# would not notice new files without a manual reconfigure.
add_library(sim
    src/config.cpp
)

target_include_directories(sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sim PUBLIC Crow::Crow)

# Asio warns and guesses a Windows 7 target if this is unset. Windows 10 = 0x0A00.
if(WIN32)
    target_compile_definitions(sim PUBLIC _WIN32_WINNT=0x0A00)
endif()

# No entry point exists yet. Once src/main.cpp is written:
# add_executable(speaking_sim src/main.cpp)
# target_link_libraries(speaking_sim PRIVATE sim)
