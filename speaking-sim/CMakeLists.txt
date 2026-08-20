cmake_minimum_required(VERSION 3.16)

project(speaking_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emits build/compile_commands.json, which clangd discovers automatically.
# This is what makes #include "sim/config.hpp" resolve in the editor.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Header-only dependencies (Asio, Crow, cpp-httplib) come from FetchContent.
# The two heavyweight native libraries (whisper.cpp, piper) are git submodules
# under third_party/, because they carry their own submodules and build options
# that are easier to pin in-tree.

# The worker pool uses std::thread / mutex / condition_variable, and Crow's
# Findasio.cmake refuses to run without Threads.
find_package(Threads REQUIRED)

# TLS for the Gemini HTTPS calls made through cpp-httplib.
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# Asio (standalone) - Crow's WebSocket support is built on it.
# ---------------------------------------------------------------------------

# Asio ships no CMakeLists of its own, so this only downloads the source tree.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-38-2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(asio)

# Crow finds Asio through its own cmake/Findasio.cmake, which does a
# find_path(ASIO_INCLUDE_DIR asio.hpp). Seeding that cache variable here makes
# the find_path a no-op and pins Crow to the copy just fetched.
#
# NOTE: the repo's asio/include is a symlink to ../include, which Windows git
# checks out as a plain text file. Point at the real directory instead.
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/include"
    CACHE PATH "Standalone Asio include directory" FORCE)
if(NOT EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
    message(FATAL_ERROR "asio.hpp not found under ${ASIO_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Crow - HTTP + WebSocket server, header only.
# ---------------------------------------------------------------------------

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CROW_INSTALL        OFF CACHE BOOL "" FORCE)
set(CROW_USE_BOOST      OFF CACHE BOOL "" FORCE)  # use the standalone Asio above
FetchContent_Declare(Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.3.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Crow)

# The sources include both "crow.h" and "crow/json.h". Crow lays both out under
# a single include root, so one directory covers them - fail loudly here rather
# than in a wall of compiler errors if a future tag moves them.
set(CROW_INCLUDE_DIR "${crow_SOURCE_DIR}/include")
foreach(header IN ITEMS crow.h crow/json.h)
    if(NOT EXISTS "${CROW_INCLUDE_DIR}/${header}")
        message(FATAL_ERROR
            "Crow header ${header} not found under ${CROW_INCLUDE_DIR}. "
            "The pinned Crow tag has changed its include layout.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# cpp-httplib - HTTP client for the Gemini and Ollama calls, header only.
# ---------------------------------------------------------------------------

# REQUIRE_OPENSSL makes cpp-httplib define CPPHTTPLIB_OPENSSL_SUPPORT and link
# OpenSSL::SSL / OpenSSL::Crypto on its interface. The other codecs are pinned
# off so the build does not silently change shape based on what a given machine
# happens to have installed.
set(HTTPLIB_REQUIRE_OPENSSL           ON  CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE                   OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL                   OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST                      OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE     OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE   OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.29.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(httplib)

# ---------------------------------------------------------------------------
# third_party submodules - optional, so the project still configures on a fresh
# clone that has not run: git submodule update --init --recursive
# ---------------------------------------------------------------------------

set(SIM_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(SIM_NATIVE_LIBS "")
set(SIM_PIPER_TARGET "")

# whisper.cpp exposes a `whisper` library target.
if(EXISTS "${SIM_THIRD_PARTY}/whisper.cpp/CMakeLists.txt")
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    add_subdirectory("${SIM_THIRD_PARTY}/whisper.cpp" EXCLUDE_FROM_ALL)
    if(TARGET whisper)
        list(APPEND SIM_NATIVE_LIBS whisper)
    else()
        message(FATAL_ERROR "third_party/whisper.cpp defined no whisper target.")
    endif()
else()
    message(STATUS
        "third_party/whisper.cpp not found - configuring without it. "
        "Run: git submodule update --init --recursive")
endif()

# piper has renamed its library target across releases, so probe the names it
# has used, skipping the `piper` command line executable if that is what it is.
if(EXISTS "${SIM_THIRD_PARTY}/piper/CMakeLists.txt")
    add_subdirectory("${SIM_THIRD_PARTY}/piper" EXCLUDE_FROM_ALL)
    foreach(candidate IN ITEMS piper piper_lib libpiper piper_phonemize)
        if(TARGET ${candidate})
            get_target_property(candidate_type ${candidate} TYPE)
            if(NOT candidate_type STREQUAL "EXECUTABLE")
                set(SIM_PIPER_TARGET ${candidate})
                break()
            endif()
        endif()
    endforeach()
    if(SIM_PIPER_TARGET)
        list(APPEND SIM_NATIVE_LIBS ${SIM_PIPER_TARGET})
    else()
        message(FATAL_ERROR "third_party/piper defined no linkable library target.")
    endif()
else()
    message(STATUS
        "third_party/piper not found - configuring without it. "
        "Run: git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# The server.
# ---------------------------------------------------------------------------

# Listed explicitly rather than file(GLOB): CMake does not notice new files
# added to a glob without a manual reconfigure.
add_executable(speaking-sim
    src/main.cpp
    src/config.cpp
    src/protocol.cpp
    src/server.cpp
    src/session.cpp
    src/worker.cpp
    src/examiner/gemini_examiner.cpp
    src/examiner/hailo_examiner.cpp
    src/stt/whisper_stt.cpp
    src/tts/piper_tts.cpp
)

target_include_directories(speaking-sim
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"   # resolves "sim/..."
        "${CROW_INCLUDE_DIR}"                   # resolves "crow.h" and "crow/json.h"
)

target_link_libraries(speaking-sim
    PRIVATE
        Crow::Crow          # brings in asio::asio, which sets ASIO_STANDALONE
        httplib::httplib    # brings in OpenSSL::SSL / OpenSSL::Crypto
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        ${SIM_NATIVE_LIBS}
)

# Lets the source guard real inference behind the submodules being present.
if(TARGET whisper)
    target_compile_definitions(speaking-sim PRIVATE SIM_HAVE_WHISPER=1)
endif()
if(SIM_PIPER_TARGET)
    target_compile_definitions(speaking-sim PRIVATE SIM_HAVE_PIPER=1)
endif()

# Asio warns and guesses a Windows 7 target if this is unset. Windows 10 = 0x0A00.
if(WIN32)
    target_compile_definitions(speaking-sim PRIVATE _WIN32_WINNT=0x0A00)
endif()

# web/index.html and prompts/examiner_system.txt are opened by relative path, so
# the binary has to be started from the project root. Pinning the output
# directory keeps ./build/speaking-sim predictable across generators.
set_target_properties(speaking-sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
