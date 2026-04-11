cmake_minimum_required(VERSION 3.22)
project(Prodigy LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries by default")

# Testing support
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    include(FetchContent)
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
target_link_libraries(sip-client PRIVATE Threads::Threads)

# 2. Inbound Audio Processor
add_executable(inbound-audio-processor inbound-audio-processor.cpp)
target_link_libraries(inbound-audio-processor PRIVATE Threads::Threads)

# 2b. VAD Service
add_executable(vad-service vad-service.cpp)
target_link_libraries(vad-service PRIVATE Threads::Threads)

# 3. Outbound Audio Processor
add_executable(outbound-audio-processor outbound-audio-processor.cpp)
target_link_libraries(outbound-audio-processor PRIVATE Threads::Threads)

# 4. Whisper Service
add_executable(whisper-service whisper-service.cpp)
target_link_libraries(whisper-service PRIVATE Threads::Threads)
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
    target_link_libraries(whisper-service PRIVATE ${ACCELERATE_FRAMEWORK} ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${METALKIT_FRAMEWORK} ${COREML_FRAMEWORK})
endif()

# 5. LLaMA Service
add_executable(llama-service llama-service.cpp)
target_link_libraries(llama-service PRIVATE Threads::Threads)
if(APPLE)
    target_link_libraries(llama-service PRIVATE ${ACCELERATE_FRAMEWORK} ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${METALKIT_FRAMEWORK})
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

# 6. Kokoro TTS Service (espeak-ng + CoreML, no PyTorch dependency)
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
        add_executable(kokoro-service kokoro-service.cpp)
        target_compile_definitions(kokoro-service PRIVATE KOKORO_COREML=1)
        find_library(COREML_FRAMEWORK_KOKORO CoreML)
        target_link_libraries(kokoro-service PRIVATE
            Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS}
            ${COREML_FRAMEWORK_KOKORO} ${FOUNDATION_FRAMEWORK})
        message(STATUS "  CoreML acceleration: ENABLED (ANE)")
    else()
        add_executable(kokoro-service kokoro-service.cpp)
        target_link_libraries(kokoro-service PRIVATE Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS})
        message(STATUS "  CoreML acceleration: DISABLED (non-Apple platform)")
    endif()
    target_include_directories(kokoro-service PRIVATE ${ESPEAK_NG_INCLUDE})
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
            ${ESPEAK_NG_INCLUDE})
        target_link_libraries(neutts-service PRIVATE
            Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS}
            ${COREML_FRAMEWORK_NEUTTS} ${ACCELERATE_FRAMEWORK} ${METAL_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK} ${METALKIT_FRAMEWORK} "-lobjc")
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

# 8. Frontend (mongoose + SQLite)
add_executable(frontend frontend.cpp mongoose.c sqlite3.c)
target_link_libraries(frontend PRIVATE Threads::Threads)
target_compile_definitions(frontend PRIVATE MG_ENABLE_PACKED_FS=0)
if(APPLE)
    target_link_libraries(frontend PRIVATE "-framework CoreFoundation")
endif()

# 9. Tomedo Crawl (RAG sidecar)
# OpenSSL is STATICALLY linked (.a) so the binary has zero runtime dependencies
# beyond system libraries. Build-time only: requires openssl headers + .a files
# (e.g. `brew install openssl@3` on macOS).
find_package(OpenSSL REQUIRED)
# Force static archives — find_package prefers .dylib on macOS, but we need .a
# to produce a standalone binary with no Homebrew runtime dependency.
get_filename_component(_OPENSSL_LIB_DIR "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
if(EXISTS "${_OPENSSL_LIB_DIR}/libssl.a" AND EXISTS "${_OPENSSL_LIB_DIR}/libcrypto.a")
    set(OPENSSL_STATIC_SSL    "${_OPENSSL_LIB_DIR}/libssl.a")
    set(OPENSSL_STATIC_CRYPTO "${_OPENSSL_LIB_DIR}/libcrypto.a")
else()
    message(FATAL_ERROR "Static OpenSSL libraries not found in ${_OPENSSL_LIB_DIR}")
endif()
add_executable(tomedo-crawl tomedo-crawl.cpp mongoose.c sqlite3.c)
target_include_directories(tomedo-crawl PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/hnswlib
    ${OPENSSL_INCLUDE_DIR})
target_link_libraries(tomedo-crawl PRIVATE
    Threads::Threads
    ${OPENSSL_STATIC_SSL}
    ${OPENSSL_STATIC_CRYPTO})
target_compile_definitions(tomedo-crawl PRIVATE MG_ENABLE_PACKED_FS=0)
if(APPLE)
    target_link_libraries(tomedo-crawl PRIVATE "-framework CoreFoundation")
endif()

# Install
set(INSTALL_TARGETS sip-client inbound-audio-processor vad-service outbound-audio-processor whisper-service llama-service frontend tomedo-crawl)
if(TARGET kokoro-service)
    list(APPEND INSTALL_TARGETS kokoro-service)
endif()
if(TARGET neutts-service)
    list(APPEND INSTALL_TARGETS neutts-service)
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
    target_link_libraries(test_interconnect PRIVATE GTest::gtest_main Threads::Threads)
    
    if(TARGET kokoro-service)
        if(APPLE AND KOKORO_COREML)
            set_source_files_properties(tests/test_kokoro_cpp.cpp PROPERTIES LANGUAGE OBJCXX)
            add_executable(test_kokoro_cpp tests/test_kokoro_cpp.cpp)
            target_compile_definitions(test_kokoro_cpp PRIVATE KOKORO_COREML=1)
            target_link_libraries(test_kokoro_cpp PRIVATE
                Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS}
                ${COREML_FRAMEWORK_KOKORO} ${FOUNDATION_FRAMEWORK})
        else()
            add_executable(test_kokoro_cpp tests/test_kokoro_cpp.cpp)
            target_link_libraries(test_kokoro_cpp PRIVATE Threads::Threads ${ESPEAK_NG_LIB} ${ESPEAK_NG_DEPS})
        endif()
        target_include_directories(test_kokoro_cpp PRIVATE ${ESPEAK_NG_INCLUDE})
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
