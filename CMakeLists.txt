cmake_minimum_required(VERSION 3.22)
project(WhisperTalk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build options
option(BUILD_HTTP_SERVER "Build the HTTP server" ON)
option(BUILD_WHISPER_SERVICE "Build the Whisper service" ON)
option(BUILD_LLAMA_SERVICE "Build the LLaMA service" ON)
option(BUILD_PIPER_SERVICE "Build the Piper service" ON)
option(BUILD_SIP_CLIENT "Build the SIP client" ON)

# Dependencies
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

# Headers
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libpiper/include
    ${CMAKE_SOURCE_DIR}/llama-cpp/include
    ${CMAKE_SOURCE_DIR}/llama-cpp/ggml/include
    ${CMAKE_SOURCE_DIR}/whisper-cpp/include
    ${CMAKE_SOURCE_DIR}/whisper-cpp/ggml/include
)

# External libraries (imported) - set these to your local builds if not auto-detected
# Note: actual library build outputs (based on current subprojects):
#  - whisper.cpp: ${CMAKE_SOURCE_DIR}/whisper-cpp/build/src/libwhisper.dylib
#  - llama.cpp:   ${CMAKE_SOURCE_DIR}/llama-cpp/build/bin/libllama.dylib
#  - libpiper:    ${CMAKE_SOURCE_DIR}/libpiper/build/libpiper.dylib (if built)
set(DEFAULT_WHISPER_LIB ${CMAKE_SOURCE_DIR}/whisper-cpp/build/src/libwhisper.dylib)
set(DEFAULT_LLAMA_LIB   ${CMAKE_SOURCE_DIR}/llama-cpp/build/bin/libllama.dylib)
set(DEFAULT_PIPER_LIB   ${CMAKE_SOURCE_DIR}/libpiper/build/libpiper.dylib)

set(WHISPER_CPP_LIB "${DEFAULT_WHISPER_LIB}" CACHE FILEPATH "Path to whisper-cpp library")
set(LLAMA_CPP_LIB   "${DEFAULT_LLAMA_LIB}"   CACHE FILEPATH "Path to llama.cpp library")
set(PIPER_LIB       "${DEFAULT_PIPER_LIB}"    CACHE FILEPATH "Path to Piper library")

if(EXISTS ${WHISPER_CPP_LIB})
    add_library(thirdparty_whisper SHARED IMPORTED)
    set_target_properties(thirdparty_whisper PROPERTIES IMPORTED_LOCATION ${WHISPER_CPP_LIB})
else()
    message(WARNING "whisper-cpp library not found at ${WHISPER_CPP_LIB}. Set WHISPER_CPP_LIB to a valid path.")
endif()

if(EXISTS ${LLAMA_CPP_LIB})
    add_library(thirdparty_llama SHARED IMPORTED)
    set_target_properties(thirdparty_llama PROPERTIES IMPORTED_LOCATION ${LLAMA_CPP_LIB})
else()
    message(WARNING "llama.cpp library not found at ${LLAMA_CPP_LIB}. Set LLAMA_CPP_LIB to a valid path.")
endif()

if(EXISTS ${PIPER_LIB})
    add_library(thirdparty_piper SHARED IMPORTED)
    set_target_properties(thirdparty_piper PROPERTIES IMPORTED_LOCATION ${PIPER_LIB})
else()
    message(WARNING "Piper library not found at ${PIPER_LIB}. Set PIPER_LIB to a valid path.")
endif()

# Common compile settings
add_compile_options(-O2 -Wall -Wextra)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# macOS rpath for local dylibs
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path;@loader_path")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif()

# Source groups
set(SHARED_SOURCES
    database.cpp
    service-advertisement.cpp
    simple-http-api.cpp
)

if(BUILD_HTTP_SERVER)
    add_executable(http-server
        http-server.cpp
        ${SHARED_SOURCES}
    )
    target_link_libraries(http-server PRIVATE Threads::Threads SQLite::SQLite3)
endif()

if(BUILD_WHISPER_SERVICE)
    add_executable(whisper-service
        whisper-service-main.cpp
        whisper-service.cpp
        ${SHARED_SOURCES}
    )
    target_link_libraries(whisper-service PRIVATE Threads::Threads SQLite::SQLite3)
    if(TARGET thirdparty_whisper)
        target_link_libraries(whisper-service PRIVATE thirdparty_whisper)
    endif()
endif()

if(BUILD_LLAMA_SERVICE)
    add_executable(llama-service
        llama-service-main.cpp
        llama-service.cpp
        ${SHARED_SOURCES}
    )
    target_link_libraries(llama-service PRIVATE Threads::Threads SQLite::SQLite3)
    if(TARGET thirdparty_llama)
        target_link_libraries(llama-service PRIVATE thirdparty_llama)
    endif()
endif()

if(BUILD_PIPER_SERVICE)
    add_executable(piper-service
        piper-service-main.cpp
        piper-service.cpp
        ${SHARED_SOURCES}
    )
    target_link_libraries(piper-service PRIVATE Threads::Threads SQLite::SQLite3)
    if(TARGET thirdparty_piper)
        target_link_libraries(piper-service PRIVATE thirdparty_piper)
    endif()
endif()

if(BUILD_SIP_CLIENT)
    add_executable(sip-client
        sip-client-main.cpp
        audio-processor-service.cpp
        simple-audio-processor.cpp
        ${SHARED_SOURCES}
    )
    target_link_libraries(sip-client PRIVATE Threads::Threads SQLite::SQLite3 OpenSSL::SSL OpenSSL::Crypto)
endif()



# Install
if(BUILD_HTTP_SERVER)
    install(TARGETS http-server RUNTIME DESTINATION bin)
endif()
if(BUILD_WHISPER_SERVICE)
    install(TARGETS whisper-service RUNTIME DESTINATION bin)
endif()
if(BUILD_LLAMA_SERVICE)
    install(TARGETS llama-service RUNTIME DESTINATION bin)
endif()
if(BUILD_PIPER_SERVICE)
    install(TARGETS piper-service RUNTIME DESTINATION bin)
endif()
if(BUILD_SIP_CLIENT)
    install(TARGETS sip-client RUNTIME DESTINATION bin)
endif()

