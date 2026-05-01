cmake_minimum_required(VERSION 3.22)
project(Prodigy LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries by default")

# ──────────────────────────────────────────────────────────────────────────────
# SQLCipher — transparent AES-256 encryption for all SQLite databases
#
# Source: https://github.com/sqlcipher/sqlcipher  (BSD licence, SQLite public domain)
#
# HOW THE AMALGAMATION IS PRODUCED
#   SQLCipher's GitHub repository contains raw C source files, not the final
#   sqlite3.c amalgamation.  The amalgamation is built by the SQLCipher autoconf
#   toolchain (./configure && make sqlite3.c) which requires tclsh and the
#   OpenSSL headers.  This two-step process cannot run inside CMake itself, so
#   it is handled by runmetobuildeverything before cmake is invoked:
#
#     runmetobuildeverything  →  build_sqlcipher_amalgamation()
#       1. git clone / pull  https://github.com/sqlcipher/sqlcipher  (master)
#       2. ./configure --with-crypto-lib=openssl  CPPFLAGS=-I<openssl>/include
#       3. make sqlite3.c          (generates the single-file amalgamation)
#       4. copy sqlite3.c + sqlite3.h → third_party/sqlcipher/
#       5. record git SHA in third_party/sqlcipher/.sqlcipher_sha (skip rebuild
#          if SHA unchanged and --force not passed)
#
#   CMakeLists.txt then references third_party/sqlcipher/sqlite3.c directly —
#   no network access at cmake time.
#
# WHY NOT brew install sqlcipher?
#   We require a STATICALLY linked, self-contained binary with zero Homebrew
#   runtime dependencies.  Linking against Homebrew's .dylib would break that.
#   Building from source gives full control over compile flags.
#
# HOW THE HEADER CONFLICT IS RESOLVED
#   SQLCipher's sqlite3.h is API-compatible with standard sqlite3.h but adds
#   sqlite3_key() / sqlite3_rekey() declarations required by db_key.h.
#   Both `frontend` and `tomedo-crawl` list ${SQLCIPHER_DIR} as their FIRST
#   target_include_directories entry, so the compiler finds SQLCipher's
#   sqlite3.h before the project-root copy.  The bundled sqlite3.c / sqlite3.h
#   in the project root are intentionally kept (editors/tools depend on them)
#   but are silently shadowed for these two targets.
#
# COMPILE DEFINITIONS (applied to both targets)
#   SQLITE_HAS_CODEC           — activates SQLCipher codec hooks in sqlite3.c
#   SQLCIPHER_CRYPTO_OPENSSL   — use OpenSSL EVP for AES-256-CBC
#   SQLITE_TEMP_STORE=2        — temp tables in RAM (never plaintext on disk)
#   SQLITE_ENABLE_COLUMN_METADATA — required by SQLCipher internals
#
# APPLE SILICON PERFORMANCE
#   OpenSSL 3.x auto-detects ARMv8.2 AES instructions (AESE/AESMC) on M chips
#   and uses hardware acceleration.  Overhead vs. plain SQLite: < 3%.
#
# KEY MANAGEMENT  (see db_key.h)
#   get_db_key() retrieves (or generates) a 256-bit key stored in the macOS
#   Keychain under kSecAttrAccessibleWhenUnlockedThisDeviceOnly.  On Apple
#   Silicon this item is hardware-rooted in the Secure Enclave — the raw key
#   material cannot be extracted by any other process or via backup.
# ──────────────────────────────────────────────────────────────────────────────
set(SQLCIPHER_DIR ${CMAKE_SOURCE_DIR}/third_party/sqlcipher)

if(NOT EXISTS "${SQLCIPHER_DIR}/sqlite3.c")
    message(FATAL_ERROR
        "SQLCipher amalgamation not found at ${SQLCIPHER_DIR}/sqlite3.c\n"
        "Run  ./runmetobuildeverything  (or  ./runmetobuildeverything --force)\n"
        "to build the SQLCipher amalgamation from source before invoking cmake.")
endif()
message(STATUS "SQLCipher amalgamation: ${SQLCIPHER_DIR}/sqlite3.c")

# FetchContent is used for optional googletest
include(FetchContent)

# Testing support
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Dependencies
find_package(Threads REQUIRED)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(OPUS opus IMPORTED_TARGET)
else()
    find_library(OPUS_LIB opus)
    find_path(OPUS_INC opus/opus.h)
    if(OPUS_LIB AND OPUS_INC)
        set(OPUS_FOUND TRUE)
        add_library(PkgConfig::OPUS UNKNOWN IMPORTED)
        set_target_properties(PkgConfig::OPUS PROPERTIES
            IMPORTED_LOCATION "${OPUS_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INC}")
    else()
        message(WARNING "pkg-config not found and opus library not found via find_library — moshi-service will be DISABLED")
    endif()
endif()

# OpenSSL — statically linked for all services.
# Required for: mongoose TLS (frontend/tomedo-crawl), interconnect AES-256-GCM
# encryption, Tomedo mTLS client, and TLS HTTP clients.
# Build-time only: requires openssl@3 headers + .a files (brew install openssl@3).
find_package(OpenSSL REQUIRED)
get_filename_component(_OPENSSL_LIB_DIR "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
if(EXISTS "${_OPENSSL_LIB_DIR}/libssl.a" AND EXISTS "${_OPENSSL_LIB_DIR}/libcrypto.a")
    set(OPENSSL_STATIC_SSL    "${_OPENSSL_LIB_DIR}/libssl.a")
    set(OPENSSL_STATIC_CRYPTO "${_OPENSSL_LIB_DIR}/libcrypto.a")
else()
    message(FATAL_ERROR "Static OpenSSL libraries not found in ${_OPENSSL_LIB_DIR}")
endif()

# Headers
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/llama-cpp/include
    ${CMAKE_SOURCE_DIR}/llama-cpp/ggml/include
    ${CMAKE_SOURCE_DIR}/whisper-cpp/include
    ${CMAKE_SOURCE_DIR}/whisper-cpp/ggml/include
)

# External libraries (imported)
set(DEFAULT_WHISPER_LIB ${CMAKE_SOURCE_DIR}/whisper-cpp/build/src/libwhisper.a)
set(DEFAULT_LLAMA_LIB   ${CMAKE_SOURCE_DIR}/llama-cpp/build/src/libllama.a)
set(DEFAULT_GGML_LIB    ${CMAKE_SOURCE_DIR}/llama-cpp/build/ggml/src/libggml.a)
set(DEFAULT_GGML_BASE_LIB ${CMAKE_SOURCE_DIR}/llama-cpp/build/ggml/src/libggml-base.a)
set(DEFAULT_GGML_CPU_LIB  ${CMAKE_SOURCE_DIR}/llama-cpp/build/ggml/src/libggml-cpu.a)
set(DEFAULT_GGML_BLAS_LIB ${CMAKE_SOURCE_DIR}/llama-cpp/build/ggml/src/ggml-blas/libggml-blas.a)
set(DEFAULT_GGML_METAL_LIB ${CMAKE_SOURCE_DIR}/llama-cpp/build/ggml/src/ggml-metal/libggml-metal.a)

set(WHISPER_CPP_LIB "${DEFAULT_WHISPER_LIB}" CACHE FILEPATH "Path to whisper-cpp library")
set(LLAMA_CPP_LIB   "${DEFAULT_LLAMA_LIB}"   CACHE FILEPATH "Path to llama.cpp library")
set(GGML_LIB        "${DEFAULT_GGML_LIB}"    CACHE FILEPATH "Path to ggml library")
set(GGML_BASE_LIB   "${DEFAULT_GGML_BASE_LIB}" CACHE FILEPATH "Path to ggml-base library")
set(GGML_CPU_LIB    "${DEFAULT_GGML_CPU_LIB}"  CACHE FILEPATH "Path to ggml-cpu library")
set(GGML_BLAS_LIB   "${DEFAULT_GGML_BLAS_LIB}" CACHE FILEPATH "Path to ggml-blas library")
set(GGML_METAL_LIB  "${DEFAULT_GGML_METAL_LIB}" CACHE FILEPATH "Path to ggml-metal library")

set(DEFAULT_WHISPER_GGML_LIB       ${CMAKE_SOURCE_DIR}/whisper-cpp/build/ggml/src/libggml.a)
set(DEFAULT_WHISPER_GGML_BASE_LIB  ${CMAKE_SOURCE_DIR}/whisper-cpp/build/ggml/src/libggml-base.a)
set(DEFAULT_WHISPER_GGML_CPU_LIB   ${CMAKE_SOURCE_DIR}/whisper-cpp/build/ggml/src/libggml-cpu.a)
set(DEFAULT_WHISPER_GGML_BLAS_LIB  ${CMAKE_SOURCE_DIR}/whisper-cpp/build/ggml/src/ggml-blas/libggml-blas.a)
set(DEFAULT_WHISPER_GGML_METAL_LIB ${CMAKE_SOURCE_DIR}/whisper-cpp/build/ggml/src/ggml-metal/libggml-metal.a)
set(WHISPER_GGML_LIB       "${DEFAULT_WHISPER_GGML_LIB}"       CACHE FILEPATH "Path to whisper ggml library")
set(WHISPER_GGML_BASE_LIB  "${DEFAULT_WHISPER_GGML_BASE_LIB}"  CACHE FILEPATH "Path to whisper ggml-base library")
set(WHISPER_GGML_CPU_LIB   "${DEFAULT_WHISPER_GGML_CPU_LIB}"   CACHE FILEPATH "Path to whisper ggml-cpu library")
set(WHISPER_GGML_BLAS_LIB  "${DEFAULT_WHISPER_GGML_BLAS_LIB}"  CACHE FILEPATH "Path to whisper ggml-blas library")
set(WHISPER_GGML_METAL_LIB "${DEFAULT_WHISPER_GGML_METAL_LIB}" CACHE FILEPATH "Path to whisper ggml-metal library")

set(DEFAULT_WHISPER_COREML_LIB ${CMAKE_SOURCE_DIR}/whisper-cpp/build/src/libwhisper.coreml.a)
set(WHISPER_COREML_LIB "${DEFAULT_WHISPER_COREML_LIB}" CACHE FILEPATH "Path to whisper CoreML library")

if(EXISTS ${WHISPER_CPP_LIB})
    add_library(thirdparty_whisper STATIC IMPORTED)
    set_target_properties(thirdparty_whisper PROPERTIES IMPORTED_LOCATION ${WHISPER_CPP_LIB})
endif()

if(EXISTS ${WHISPER_COREML_LIB})
    add_library(thirdparty_whisper_coreml STATIC IMPORTED)
    set_target_properties(thirdparty_whisper_coreml PROPERTIES IMPORTED_LOCATION ${WHISPER_COREML_LIB})
endif()

if(EXISTS ${LLAMA_CPP_LIB})
    add_library(thirdparty_llama STATIC IMPORTED)
    set_target_properties(thirdparty_llama PROPERTIES IMPORTED_LOCATION ${LLAMA_CPP_LIB})
endif()

if(EXISTS ${GGML_LIB})
    add_library(thirdparty_ggml STATIC IMPORTED)
    set_target_properties(thirdparty_ggml PROPERTIES IMPORTED_LOCATION ${GGML_LIB})
endif()

if(EXISTS ${GGML_BASE_LIB})
    add_library(thirdparty_ggml_base STATIC IMPORTED)
    set_target_properties(thirdparty_ggml_base PROPERTIES IMPORTED_LOCATION ${GGML_BASE_LIB})
endif()

if(EXISTS ${GGML_CPU_LIB})
    add_library(thirdparty_ggml_cpu STATIC IMPORTED)
    set_target_properties(thirdparty_ggml_cpu PROPERTIES IMPORTED_LOCATION ${GGML_CPU_LIB})
endif()

if(EXISTS ${GGML_BLAS_LIB})
    add_library(thirdparty_ggml_blas STATIC IMPORTED)
    set_target_properties(thirdparty_ggml_blas PROPERTIES IMPORTED_LOCATION ${GGML_BLAS_LIB})
endif()

if(EXISTS ${GGML_METAL_LIB})
    add_library(thirdparty_ggml_metal STATIC IMPORTED)
    set_target_properties(thirdparty_ggml_metal PROPERTIES IMPORTED_LOCATION ${GGML_METAL_LIB})
endif()

if(EXISTS ${WHISPER_GGML_LIB})
    add_library(thirdparty_whisper_ggml STATIC IMPORTED)
    set_target_properties(thirdparty_whisper_ggml PROPERTIES IMPORTED_LOCATION ${WHISPER_GGML_LIB})
endif()
if(EXISTS ${WHISPER_GGML_BASE_LIB})
    add_library(thirdparty_whisper_ggml_base STATIC IMPORTED)
    set_target_properties(thirdparty_whisper_ggml_base PROPERTIES IMPORTED_LOCATION ${WHISPER_GGML_BASE_LIB})
endif()
if(EXISTS ${WHISPER_GGML_CPU_LIB})
    add_library(thirdparty_whisper_ggml_cpu STATIC IMPORTED)
    set_target_properties(thirdparty_whisper_ggml_cpu PROPERTIES IMPORTED_LOCATION ${WHISPER_GGML_CPU_LIB})
endif()
if(EXISTS ${WHISPER_GGML_BLAS_LIB})
    add_library(thirdparty_whisper_ggml_blas STATIC IMPORTED)
    set_target_properties(thirdparty_whisper_ggml_blas PROPERTIES IMPORTED_LOCATION ${WHISPER_GGML_BLAS_LIB})
endif()
if(EXISTS ${WHISPER_GGML_METAL_LIB})
    add_library(thirdparty_whisper_ggml_metal STATIC IMPORTED)
    set_target_properties(thirdparty_whisper_ggml_metal PROPERTIES IMPORTED_LOCATION ${WHISPER_GGML_METAL_LIB})
endif()

# Common compile settings
add_compile_options(-O2 -Wall -Wextra)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Ensure bin/models directory exists (protected from clean builds)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/models)
set_directory_properties(PROPERTIES ADDITIONAL_CLEAN_FILES "")

set(WHISPERTALK_MODELS_DIR "${CMAKE_SOURCE_DIR}/bin/models" CACHE PATH "Path to model files")
add_compile_definitions(WHISPERTALK_MODELS_DIR="${WHISPERTALK_MODELS_DIR}")

# macOS rpath
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path;@loader_path")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif()

# macOS frameworks (shared by whisper-service and llama-service)
if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate)
    find_library(METAL_FRAMEWORK Metal)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    find_library(METALKIT_FRAMEWORK MetalKit)
endif()

# 1. SIP Client
add_executable(sip-client sip-client-main.cpp)
target_include_directories(sip-client PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(sip-client PRIVATE Threads::Threads
    ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(sip-client PRIVATE
        "-framework CoreFoundation" "-framework Security")
endif()

# 2. Inbound Audio Processor
add_executable(inbound-audio-processor inbound-audio-processor.cpp)
target_include_directories(inbound-audio-processor PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(inbound-audio-processor PRIVATE Threads::Threads
    ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(inbound-audio-processor PRIVATE
        "-framework CoreFoundation" "-framework Security")
endif()

# 2b. VAD Service
add_executable(vad-service vad-service.cpp)
target_include_directories(vad-service PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(vad-service PRIVATE Threads::Threads
    ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(vad-service PRIVATE
        "-framework CoreFoundation" "-framework Security")
endif()

# 3. Outbound Audio Processor
add_executable(outbound-audio-processor outbound-audio-processor.cpp)
target_include_directories(outbound-audio-processor PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(outbound-audio-processor PRIVATE Threads::Threads
    ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(outbound-audio-processor PRIVATE
        "-framework CoreFoundation" "-framework Security")
endif()

# 4. Whisper Service
add_executable(whisper-service whisper-service.cpp)
target_include_directories(whisper-service PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(whisper-service PRIVATE Threads::Threads
    ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
if(TARGET thirdparty_whisper)
    target_link_libraries(whisper-service PRIVATE thirdparty_whisper)
endif()
if(TARGET thirdparty_whisper_ggml_metal)
    target_link_libraries(whisper-service PRIVATE thirdparty_whisper_ggml_metal)
endif()
if(TARGET thirdparty_whisper_ggml_blas)
    target_link_libraries(whisper-service PRIVATE thirdparty_whisper_ggml_blas)
endif()
if(TARGET thirdparty_whisper_ggml_cpu)
    target_link_libraries(whisper-service PRIVATE thirdparty_whisper_ggml_cpu)
endif()
if(TARGET thirdparty_whisper_ggml_base)
    target_link_libraries(whisper-service PRIVATE thirdparty_whisper_ggml_base)
endif()
if(TARGET thirdparty_whisper_ggml)
    target_link_libraries(whisper-service PRIVATE thirdparty_whisper_ggml)
endif()
if(TARGET thirdparty_whisper_coreml)
    target_link_libraries(whisper-service PRIVATE thirdparty_whisper_coreml)
endif()
if(APPLE)
    find_library(COREML_FRAMEWORK CoreML)
    target_link_libraries(whisper-service PRIVATE ${ACCELERATE_FRAMEWORK} ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${METALKIT_FRAMEWORK} ${COREML_FRAMEWORK}
        "-framework Security")
endif()

# 5. LLaMA Service
add_executable(llama-service llama-service.cpp)
target_include_directories(llama-service PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(llama-service PRIVATE Threads::Threads
    ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(llama-service PRIVATE ${ACCELERATE_FRAMEWORK} ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${METALKIT_FRAMEWORK}
        "-framework Security")
endif()
if(TARGET thirdparty_llama)
    target_link_libraries(llama-service PRIVATE thirdparty_llama)
endif()
if(TARGET thirdparty_ggml_metal)
    target_link_libraries(llama-service PRIVATE thirdparty_ggml_metal)
endif()
if(TARGET thirdparty_ggml_blas)
    target_link_libraries(llama-service PRIVATE thirdparty_ggml_blas)
endif()
if(TARGET thirdparty_ggml_cpu)
    target_link_libraries(llama-service PRIVATE thirdparty_ggml_cpu)
endif()
if(TARGET thirdparty_ggml_base)
    target_link_libraries(llama-service PRIVATE thirdparty_ggml_base)
endif()
if(TARGET thirdparty_ggml)
    target_link_libraries(llama-service PRIVATE thirdparty_ggml)
endif()

# 6. TTS Service (generic TTS stage/dock: bridges LLaMA <-> engines <-> OAP)
add_executable(tts-service tts-service.cpp)
target_include_directories(tts-service PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(tts-service PRIVATE Threads::Threads
    ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(tts-service PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
set_property(TARGET tts-service PROPERTY CXX_STANDARD 17)

# 6b. Kokoro TTS Service (espeak-ng + CoreML, no PyTorch dependency)
option(KOKORO_COREML "Enable CoreML acceleration for Kokoro (required on macOS)" ON)
find_library(ESPEAK_NG_LIB NAMES libespeak-ng.a espeak-ng PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_library(PCAUDIO_LIB NAMES libpcaudio.a pcaudio PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_path(ESPEAK_NG_INCLUDE espeak-ng/speak_lib.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
if(APPLE AND PCAUDIO_LIB)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    find_library(AUDIOUNIT_FRAMEWORK AudioUnit)
    set(ESPEAK_NG_DEPS ${PCAUDIO_LIB})
    if(AUDIOTOOLBOX_FRAMEWORK)
        list(APPEND ESPEAK_NG_DEPS ${AUDIOTOOLBOX_FRAMEWORK})
    endif()
    if(AUDIOUNIT_FRAMEWORK)
        list(APPEND ESPEAK_NG_DEPS ${AUDIOUNIT_FRAMEWORK})
    endif()
endif()
set(ESPEAK_NG_DATA_DIR "" CACHE PATH "Path to espeak-ng-data directory")
if(NOT ESPEAK_NG_DATA_DIR)
    foreach(_edir /opt/homebrew/share/espeak-ng-data /usr/local/share/espeak-ng-data /usr/share/espeak-ng-data)
        if(EXISTS "${_edir}")
            set(ESPEAK_NG_DATA_DIR "${_edir}")
            break()
        endif()
    endforeach()
endif()
if(ESPEAK_NG_LIB AND ESPEAK_NG_INCLUDE)
    if(APPLE AND KOKORO_COREML)
        set_source_files_properties(kokoro-service.cpp PROPERTIES LANGUAGE OBJCXX)
        set_source_files_properties(neural-g2p.mm PROPERTIES LANGUAGE OBJCXX)
        add_executable(kokoro-service kokoro-service.cpp neural-g2p.mm)
        target_compile_definitions(kokoro-service PRIVATE KOKORO_COREML=1)
        find_library(COREML_FRAMEWORK_KOKORO CoreML)
        target_link_libraries(kokoro-service PRIVATE
            Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS}
            ${COREML_FRAMEWORK_KOKORO} ${FOUNDATION_FRAMEWORK} ${ACCELERATE_FRAMEWORK})
        message(STATUS "  CoreML acceleration: ENABLED (ANE)")
    else()
        add_executable(kokoro-service kokoro-service.cpp)
        target_link_libraries(kokoro-service PRIVATE Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS})
        message(STATUS "  CoreML acceleration: DISABLED (non-Apple platform)")
    endif()
    target_include_directories(kokoro-service PRIVATE ${ESPEAK_NG_INCLUDE} ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(kokoro-service PRIVATE ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
    if(APPLE)
        target_link_libraries(kokoro-service PRIVATE "-framework Security")
    endif()
    set_property(TARGET kokoro-service PROPERTY CXX_STANDARD 17)
    if(ESPEAK_NG_DATA_DIR)
        target_compile_definitions(kokoro-service PRIVATE ESPEAK_NG_DATA_DIR="${ESPEAK_NG_DATA_DIR}")
    endif()
    message(STATUS "Kokoro TTS Service: ENABLED (CoreML split decoder, no PyTorch)")
    if(ESPEAK_NG_DATA_DIR)
        message(STATUS "  espeak-ng data: ${ESPEAK_NG_DATA_DIR}")
    endif()
else()
    message(WARNING "Kokoro TTS Service: DISABLED (espeak-ng not found)")
    if(NOT ESPEAK_NG_LIB)
        message(WARNING "  espeak-ng not found. Install via: brew install espeak-ng")
    endif()
endif()

# 7. NeuTTS Service (llama.cpp backbone + NeuCodec via native CoreML + espeak-ng)
if(ESPEAK_NG_LIB AND ESPEAK_NG_INCLUDE)
    find_library(COREML_FRAMEWORK_NEUTTS CoreML)
    if(COREML_FRAMEWORK_NEUTTS)
        set_source_files_properties(neutts-service.cpp PROPERTIES LANGUAGE OBJCXX)
        add_executable(neutts-service neutts-service.cpp)
        target_include_directories(neutts-service PRIVATE
            ${CMAKE_SOURCE_DIR}/llama-cpp/include
            ${CMAKE_SOURCE_DIR}/llama-cpp/ggml/include
            ${ESPEAK_NG_INCLUDE}
            ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(neutts-service PRIVATE
            Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS}
            ${COREML_FRAMEWORK_NEUTTS} ${ACCELERATE_FRAMEWORK} ${METAL_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK} ${METALKIT_FRAMEWORK} "-lobjc"
            ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO} "-framework Security")
        set_property(TARGET neutts-service PROPERTY CXX_STANDARD 17)
        if(ESPEAK_NG_DATA_DIR)
            target_compile_definitions(neutts-service PRIVATE ESPEAK_NG_DATA_DIR="${ESPEAK_NG_DATA_DIR}")
        endif()
        if(TARGET thirdparty_llama)
            target_link_libraries(neutts-service PRIVATE thirdparty_llama)
        endif()
        if(TARGET thirdparty_ggml_metal)
            target_link_libraries(neutts-service PRIVATE thirdparty_ggml_metal)
        endif()
        if(TARGET thirdparty_ggml_blas)
            target_link_libraries(neutts-service PRIVATE thirdparty_ggml_blas)
        endif()
        if(TARGET thirdparty_ggml_cpu)
            target_link_libraries(neutts-service PRIVATE thirdparty_ggml_cpu)
        endif()
        if(TARGET thirdparty_ggml_base)
            target_link_libraries(neutts-service PRIVATE thirdparty_ggml_base)
        endif()
        if(TARGET thirdparty_ggml)
            target_link_libraries(neutts-service PRIVATE thirdparty_ggml)
        endif()
        message(STATUS "NeuTTS Service: ENABLED (llama.cpp + NeuCodec CoreML + espeak-ng)")
    else()
        message(WARNING "NeuTTS Service: DISABLED (CoreML framework not found)")
    endif()
else()
    message(WARNING "NeuTTS Service: DISABLED (espeak-ng not found)")
endif()

# 7b. VITS2 TTS Service (Piper VITS2 ONNX, libpiper, espeak-ng)
if(ESPEAK_NG_LIB AND ESPEAK_NG_INCLUDE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/libpiper/CMakeLists.txt")
        if(NOT TARGET piper)
            add_subdirectory(libpiper)
        endif()
        set(ONNXRUNTIME_VERSION "1.22.0")
        if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL arm64)
            set(ONNXRUNTIME_PREFIX "onnxruntime-osx-arm64-${ONNXRUNTIME_VERSION}")
        elseif(APPLE)
            set(ONNXRUNTIME_PREFIX "onnxruntime-osx-x86_64-${ONNXRUNTIME_VERSION}")
        else()
            if(CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64)
                set(ONNXRUNTIME_PREFIX "onnxruntime-linux-aarch64-${ONNXRUNTIME_VERSION}")
            else()
                set(ONNXRUNTIME_PREFIX "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}")
            endif()
        endif()
        set(ONNXRUNTIME_DIR "${CMAKE_SOURCE_DIR}/libpiper/lib/${ONNXRUNTIME_PREFIX}")
        set(ONNXRUNTIME_DYLIB "${ONNXRUNTIME_DIR}/lib/libonnxruntime.${ONNXRUNTIME_VERSION}.dylib")

        if(APPLE AND KOKORO_COREML)
            set_source_files_properties(vits2-service.cpp PROPERTIES LANGUAGE OBJCXX)
            find_library(COREML_FRAMEWORK_VITS2 CoreML)
            add_executable(vits2-service vits2-service.cpp neural-g2p.mm)
            target_compile_definitions(vits2-service PRIVATE KOKORO_COREML=1)
            target_link_libraries(vits2-service PRIVATE
                ${COREML_FRAMEWORK_VITS2} ${FOUNDATION_FRAMEWORK} ${ACCELERATE_FRAMEWORK})
        else()
            add_executable(vits2-service vits2-service.cpp)
        endif()
        target_include_directories(vits2-service PRIVATE
            ${ESPEAK_NG_INCLUDE}
            ${OPENSSL_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/libpiper/include)
        target_link_libraries(vits2-service PRIVATE
            Threads::Threads piper
            ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
        if(APPLE)
            target_link_libraries(vits2-service PRIVATE "-framework Security")
        endif()
        set_property(TARGET vits2-service PROPERTY CXX_STANDARD 17)
        if(ESPEAK_NG_DATA_DIR)
            target_compile_definitions(vits2-service PRIVATE ESPEAK_NG_DATA_DIR="${ESPEAK_NG_DATA_DIR}")
        endif()
        if(EXISTS "${ONNXRUNTIME_DYLIB}")
            set_target_properties(vits2-service PROPERTIES
                INSTALL_RPATH "@executable_path"
                BUILD_WITH_INSTALL_RPATH ON)
            install(FILES "${ONNXRUNTIME_DYLIB}"
                DESTINATION bin
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endif()
        message(STATUS "VITS2 Service: ENABLED (Piper, ONNX Runtime, espeak-ng)")
    else()
        message(WARNING "VITS2 Service: DISABLED (libpiper not found at libpiper/)")
    endif()
else()
    message(WARNING "VITS2 Service: DISABLED (espeak-ng not found)")
endif()

# 7c. Matcha-TTS Service (Matcha-TTS CoreML, espeak-ng, optional NeuralG2P)
if(ESPEAK_NG_LIB AND ESPEAK_NG_INCLUDE)
    find_library(COREML_FRAMEWORK_MATCHA CoreML)
    if(COREML_FRAMEWORK_MATCHA)
        set_source_files_properties(matcha-service.cpp PROPERTIES LANGUAGE OBJCXX)
        set_source_files_properties(neural-g2p.mm PROPERTIES LANGUAGE OBJCXX)
        add_executable(matcha-service matcha-service.cpp neural-g2p.mm)
        target_include_directories(matcha-service PRIVATE ${ESPEAK_NG_INCLUDE} ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(matcha-service PRIVATE
            Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS}
            ${COREML_FRAMEWORK_MATCHA} ${ACCELERATE_FRAMEWORK} ${METAL_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK} ${METALKIT_FRAMEWORK} "-lobjc"
            ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO} "-framework Security")
        set_property(TARGET matcha-service PROPERTY CXX_STANDARD 17)
        if(ESPEAK_NG_DATA_DIR)
            target_compile_definitions(matcha-service PRIVATE ESPEAK_NG_DATA_DIR="${ESPEAK_NG_DATA_DIR}")
        endif()
        message(STATUS "Matcha-TTS Service: ENABLED (CoreML, espeak-ng)")
    else()
        message(WARNING "Matcha-TTS Service: DISABLED (CoreML framework not found)")
    endif()
else()
    message(WARNING "Matcha-TTS Service: DISABLED (espeak-ng not found)")
endif()

# 7d. Moshi Service (full-duplex neural voice, bridges IAP↔moshi-backend via WebSocket+Opus)
if(OPUS_FOUND)
    add_executable(moshi-service moshi-service.cpp mongoose.c)
    target_include_directories(moshi-service PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(moshi-service PRIVATE MG_ENABLE_PACKED_FS=0)
    target_link_libraries(moshi-service PRIVATE Threads::Threads
        ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO}
        PkgConfig::OPUS)
    if(APPLE)
        target_link_libraries(moshi-service PRIVATE
            "-framework CoreFoundation" "-framework Security")
    endif()
else()
    message(WARNING "Moshi Service: DISABLED (opus not found)")
endif()

# 8. Frontend (mongoose + SQLCipher)
# Uses SQLCipher (${SQLCIPHER_DIR}/sqlite3.c) instead of the bundled sqlite3.c
# so that frontend.db and the shared tomedo-crawl.db are both opened with
# AES-256 encryption via db_key.h / prodigy_db::db_open_encrypted.
# ${SQLCIPHER_DIR} is prepended so the compiler finds SQLCipher's sqlite3.h
# (which declares sqlite3_key/sqlite3_rekey) before the project-root copy.
add_executable(frontend frontend.cpp mongoose.c ${SQLCIPHER_DIR}/sqlite3.c)
target_include_directories(frontend PRIVATE
    ${SQLCIPHER_DIR}                # SQLCipher sqlite3.h (must come first)
    ${OPENSSL_INCLUDE_DIR})
target_compile_definitions(frontend PRIVATE
    MG_ENABLE_PACKED_FS=0
    MG_TLS=MG_TLS_OPENSSL                                 # Enable mongoose TLS via OpenSSL
    SQLITE_HAS_CODEC                                       # Enable SQLCipher codec hooks
    SQLCIPHER_CRYPTO_OPENSSL                               # Use OpenSSL EVP backend (AES-256-CBC)
    SQLITE_TEMP_STORE=2                                    # Temp tables in RAM, never on disk
    SQLITE_ENABLE_COLUMN_METADATA                          # Required by SQLCipher internals
    SQLITE_EXTRA_INIT=sqlcipher_extra_init                 # Required by SQLCipher master
    SQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown)        # Required by SQLCipher master
target_link_libraries(frontend PRIVATE
    Threads::Threads
    ${OPENSSL_STATIC_SSL}
    ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(frontend PRIVATE
        "-framework CoreFoundation"
        "-framework Security")      # macOS Keychain API (db_key.h)
endif()

# 9. Tomedo Crawl (RAG sidecar)
add_executable(tomedo-crawl tomedo-crawl.cpp mongoose.c ${SQLCIPHER_DIR}/sqlite3.c)
target_include_directories(tomedo-crawl PRIVATE
    ${SQLCIPHER_DIR}                # SQLCipher sqlite3.h (must come first)
    ${CMAKE_SOURCE_DIR}/third_party/hnswlib
    ${OPENSSL_INCLUDE_DIR})
target_compile_definitions(tomedo-crawl PRIVATE
    MG_ENABLE_PACKED_FS=0
    MG_TLS=MG_TLS_OPENSSL                                 # Enable mongoose TLS via OpenSSL
    SQLITE_HAS_CODEC                                       # Enable SQLCipher codec hooks
    SQLCIPHER_CRYPTO_OPENSSL                               # Use OpenSSL EVP backend (AES-256-CBC)
    SQLITE_TEMP_STORE=2                                    # Temp tables in RAM, never on disk
    SQLITE_ENABLE_COLUMN_METADATA                          # Required by SQLCipher internals
    SQLITE_EXTRA_INIT=sqlcipher_extra_init                 # Required by SQLCipher master
    SQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown)        # Required by SQLCipher master
target_link_libraries(tomedo-crawl PRIVATE
    Threads::Threads
    ${OPENSSL_STATIC_SSL}
    ${OPENSSL_STATIC_CRYPTO})
if(APPLE)
    target_link_libraries(tomedo-crawl PRIVATE
        "-framework CoreFoundation"
        "-framework Security")      # macOS Keychain API (db_key.h)
endif()

# Install
set(INSTALL_TARGETS sip-client inbound-audio-processor vad-service outbound-audio-processor whisper-service llama-service tts-service frontend tomedo-crawl)
if(TARGET moshi-service)
    list(APPEND INSTALL_TARGETS moshi-service)
endif()
if(TARGET kokoro-service)
    list(APPEND INSTALL_TARGETS kokoro-service)
endif()
if(TARGET neutts-service)
    list(APPEND INSTALL_TARGETS neutts-service)
endif()
if(TARGET vits2-service)
    list(APPEND INSTALL_TARGETS vits2-service)
endif()
if(TARGET matcha-service)
    list(APPEND INSTALL_TARGETS matcha-service)
endif()
install(TARGETS ${INSTALL_TARGETS} RUNTIME DESTINATION bin)

# 8. Test SIP Provider (runtime tool, not a unit test)
add_executable(test_sip_provider tests/test_sip_provider.cpp mongoose.c)
target_link_libraries(test_sip_provider PRIVATE Threads::Threads)
target_compile_definitions(test_sip_provider PRIVATE MG_ENABLE_PACKED_FS=0)
set_property(TARGET test_sip_provider PROPERTY CXX_STANDARD 17)

# Tests
if(BUILD_TESTS)
    add_executable(test_sanity tests/test_sanity.cpp)
    target_link_libraries(test_sanity PRIVATE GTest::gtest_main Threads::Threads)
    
    add_executable(test_interconnect tests/test_interconnect.cpp)
    target_include_directories(test_interconnect PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(test_interconnect PRIVATE GTest::gtest_main Threads::Threads
        ${OPENSSL_STATIC_SSL} ${OPENSSL_STATIC_CRYPTO})
    if(APPLE)
        target_link_libraries(test_interconnect PRIVATE "-framework Security" "-framework CoreFoundation")
    endif()
    
    if(TARGET kokoro-service)
        if(APPLE AND KOKORO_COREML)
            set_source_files_properties(tests/test_kokoro_cpp.cpp PROPERTIES LANGUAGE OBJCXX)
            set_source_files_properties(neural-g2p.mm PROPERTIES LANGUAGE OBJCXX)
            add_executable(test_kokoro_cpp tests/test_kokoro_cpp.cpp neural-g2p.mm)
            target_compile_definitions(test_kokoro_cpp PRIVATE KOKORO_COREML=1)
            target_link_libraries(test_kokoro_cpp PRIVATE
                Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS}
                ${COREML_FRAMEWORK_KOKORO} ${FOUNDATION_FRAMEWORK} ${ACCELERATE_FRAMEWORK})
        else()
            add_executable(test_kokoro_cpp tests/test_kokoro_cpp.cpp)
            target_link_libraries(test_kokoro_cpp PRIVATE Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS})
        endif()
        target_include_directories(test_kokoro_cpp PRIVATE ${ESPEAK_NG_INCLUDE})
        if(APPLE AND ACCELERATE_FRAMEWORK AND NOT KOKORO_COREML)
            target_link_libraries(test_kokoro_cpp PRIVATE ${ACCELERATE_FRAMEWORK})
        endif()
        set_property(TARGET test_kokoro_cpp PROPERTY CXX_STANDARD 17)
        if(ESPEAK_NG_DATA_DIR)
            target_compile_definitions(test_kokoro_cpp PRIVATE ESPEAK_NG_DATA_DIR="${ESPEAK_NG_DATA_DIR}")
        endif()
    endif()

    add_executable(test_sip_provider_unit tests/test_sip_provider_unit.cpp)
    target_link_libraries(test_sip_provider_unit PRIVATE GTest::gtest_main Threads::Threads)
    set_property(TARGET test_sip_provider_unit PROPERTY CXX_STANDARD 17)

    add_executable(test_integration tests/test_integration.cpp)
    target_link_libraries(test_integration PRIVATE GTest::gtest_main Threads::Threads)
    set_property(TARGET test_integration PROPERTY CXX_STANDARD 17)

    include(GoogleTest)
    gtest_discover_tests(test_sanity)
    gtest_discover_tests(test_interconnect)
    gtest_discover_tests(test_sip_provider_unit)
    gtest_discover_tests(test_integration
        PROPERTIES ENVIRONMENT "WHISPERTALK_BIN_DIR=${CMAKE_SOURCE_DIR}/bin;WHISPERTALK_MODELS_DIR=${CMAKE_SOURCE_DIR}/bin/models"
    )
endif()
