cmake_minimum_required(VERSION 3.22)
project(WhisperTalk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Testing support
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
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

# 6. Kokoro TTS Service (libtorch + espeak-ng)
set(TORCH_CMAKE_PREFIX "" CACHE PATH "Path to libtorch cmake config")
if(NOT TORCH_CMAKE_PREFIX)
    execute_process(
        COMMAND python3 -c "import torch; print(torch.utils.cmake_prefix_path)"
        OUTPUT_VARIABLE TORCH_CMAKE_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(TORCH_CMAKE_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${TORCH_CMAKE_PREFIX}")
endif()
find_package(Torch QUIET)
find_library(ESPEAK_NG_LIB espeak-ng PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_path(ESPEAK_NG_INCLUDE espeak-ng/speak_lib.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
set(ESPEAK_NG_DATA_DIR "" CACHE PATH "Path to espeak-ng-data directory")
if(NOT ESPEAK_NG_DATA_DIR)
    foreach(_edir /opt/homebrew/share/espeak-ng-data /usr/local/share/espeak-ng-data /usr/share/espeak-ng-data)
        if(EXISTS "${_edir}")
            set(ESPEAK_NG_DATA_DIR "${_edir}")
            break()
        endif()
    endforeach()
endif()
if(Torch_FOUND AND ESPEAK_NG_LIB AND ESPEAK_NG_INCLUDE)
    add_executable(kokoro-service kokoro-service.cpp)
    target_link_libraries(kokoro-service PRIVATE Threads::Threads ${TORCH_LIBRARIES} ${ESPEAK_NG_LIB})
    target_include_directories(kokoro-service PRIVATE ${ESPEAK_NG_INCLUDE})
    set_property(TARGET kokoro-service PROPERTY CXX_STANDARD 17)
    if(ESPEAK_NG_DATA_DIR)
        target_compile_definitions(kokoro-service PRIVATE ESPEAK_NG_DATA_DIR="${ESPEAK_NG_DATA_DIR}")
    endif()
    if(APPLE)
        set_target_properties(kokoro-service PROPERTIES
            BUILD_RPATH "${TORCH_CMAKE_PREFIX}/../../lib"
            INSTALL_RPATH "@executable_path;@executable_path/../lib")
    endif()
    message(STATUS "Kokoro TTS Service: ENABLED (libtorch + espeak-ng)")
    if(ESPEAK_NG_DATA_DIR)
        message(STATUS "  espeak-ng data: ${ESPEAK_NG_DATA_DIR}")
    endif()
else()
    message(WARNING "Kokoro TTS Service: DISABLED (libtorch or espeak-ng not found)")
    if(NOT Torch_FOUND)
        message(WARNING "  libtorch not found. Set TORCH_CMAKE_PREFIX or install PyTorch.")
    endif()
    if(NOT ESPEAK_NG_LIB)
        message(WARNING "  espeak-ng not found. Install via: brew install espeak-ng")
    endif()
endif()

# Install
set(INSTALL_TARGETS sip-client inbound-audio-processor outbound-audio-processor whisper-service llama-service)
if(TARGET kokoro-service)
    list(APPEND INSTALL_TARGETS kokoro-service)
endif()
install(TARGETS ${INSTALL_TARGETS} RUNTIME DESTINATION bin)

# Tests
if(BUILD_TESTS)
    add_executable(test_sanity tests/test_sanity.cpp)
    target_link_libraries(test_sanity PRIVATE GTest::gtest_main Threads::Threads)
    
    add_executable(test_interconnect tests/test_interconnect.cpp)
    target_link_libraries(test_interconnect PRIVATE GTest::gtest_main Threads::Threads)
    
    if(TARGET kokoro-service)
        add_executable(test_kokoro_cpp tests/test_kokoro_cpp.cpp)
        target_link_libraries(test_kokoro_cpp PRIVATE Threads::Threads ${TORCH_LIBRARIES} ${ESPEAK_NG_LIB})
        target_include_directories(test_kokoro_cpp PRIVATE ${ESPEAK_NG_INCLUDE})
        set_property(TARGET test_kokoro_cpp PROPERTY CXX_STANDARD 17)
        if(ESPEAK_NG_DATA_DIR)
            target_compile_definitions(test_kokoro_cpp PRIVATE ESPEAK_NG_DATA_DIR="${ESPEAK_NG_DATA_DIR}")
        endif()
        if(APPLE)
            set_target_properties(test_kokoro_cpp PROPERTIES
                BUILD_RPATH "${TORCH_CMAKE_PREFIX}/../../lib")
        endif()
    endif()

    include(GoogleTest)
    gtest_discover_tests(test_sanity)
    gtest_discover_tests(test_interconnect)
endif()
