cmake_minimum_required(VERSION 3.22)
project(WhisperTalk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
set(DEFAULT_WHISPER_LIB ${CMAKE_SOURCE_DIR}/whisper-cpp/build/src/libwhisper.dylib)
set(DEFAULT_LLAMA_LIB   ${CMAKE_SOURCE_DIR}/llama-cpp/build/bin/libllama.dylib)

set(WHISPER_CPP_LIB "${DEFAULT_WHISPER_LIB}" CACHE FILEPATH "Path to whisper-cpp library")
set(LLAMA_CPP_LIB   "${DEFAULT_LLAMA_LIB}"   CACHE FILEPATH "Path to llama.cpp library")

if(EXISTS ${WHISPER_CPP_LIB})
    add_library(thirdparty_whisper SHARED IMPORTED)
    set_target_properties(thirdparty_whisper PROPERTIES IMPORTED_LOCATION ${WHISPER_CPP_LIB})
endif()

if(EXISTS ${LLAMA_CPP_LIB})
    add_library(thirdparty_llama SHARED IMPORTED)
    set_target_properties(thirdparty_llama PROPERTIES IMPORTED_LOCATION ${LLAMA_CPP_LIB})
endif()

# Common compile settings
add_compile_options(-O2 -Wall -Wextra)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# macOS rpath
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path;@loader_path")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
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

# 5. LLaMA Service
add_executable(llama-service llama-service.cpp)
target_link_libraries(llama-service PRIVATE Threads::Threads)
if(TARGET thirdparty_llama)
    target_link_libraries(llama-service PRIVATE thirdparty_llama)
endif()

# Install
install(TARGETS sip-client inbound-audio-processor outbound-audio-processor whisper-service llama-service RUNTIME DESTINATION bin)
