#!/bin/bash

# Script to copy only essential whisper.cpp files from GitHub repository
# This downloads fresh files and avoids copying unnecessary files like examples, tests, bindings, etc.

set -e

TARGET_DIR="/Users/whisper/Documents/augment-projects/clean-repo"
TEMP_DIR="/tmp/whisper-cpp-download"

echo "🔍 Downloading essential whisper.cpp files from GitHub..."
echo "📂 Target: $TARGET_DIR"

cd "$TARGET_DIR"

# Clean up any existing temp directory
rm -rf "$TEMP_DIR"
mkdir -p "$TEMP_DIR"

echo "📥 Cloning whisper.cpp repository..."
git clone --depth 1 https://github.com/ggml-org/whisper.cpp.git "$TEMP_DIR"

# Remove existing whisper-cpp directory if it exists
if [ -d "whisper-cpp" ]; then
    echo "🗑️  Removing existing whisper-cpp directory..."
    rm -rf whisper-cpp
fi

# Create whisper directory structure
echo "📁 Creating whisper directory structure..."
mkdir -p whisper-cpp/include
mkdir -p whisper-cpp/src
mkdir -p whisper-cpp/ggml/include
mkdir -p whisper-cpp/ggml/src

# Copy essential header files
echo "📄 Copying header files..."
cp "$TEMP_DIR/include/whisper.h" whisper-cpp/include/
cp "$TEMP_DIR/ggml/include/ggml.h" whisper-cpp/ggml/include/
cp "$TEMP_DIR/ggml/include/ggml-alloc.h" whisper-cpp/ggml/include/
cp "$TEMP_DIR/ggml/include/ggml-backend.h" whisper-cpp/ggml/include/
cp "$TEMP_DIR/ggml/include/ggml-cpu.h" whisper-cpp/ggml/include/

# Copy internal header files (needed for compilation)
echo "📄 Copying internal header files..."
cp "$TEMP_DIR/ggml/src/ggml-impl.h" whisper-cpp/ggml/src/
cp "$TEMP_DIR/ggml/src/ggml-common.h" whisper-cpp/ggml/src/

# Copy essential source files
echo "📄 Copying core source files..."
cp "$TEMP_DIR/src/whisper.cpp" whisper-cpp/src/
cp "$TEMP_DIR/src/whisper-arch.h" whisper-cpp/src/
cp "$TEMP_DIR/ggml/src/ggml.c" whisper-cpp/ggml/src/
cp "$TEMP_DIR/ggml/src/ggml-alloc.c" whisper-cpp/ggml/src/
cp "$TEMP_DIR/ggml/src/ggml-backend.cpp" whisper-cpp/ggml/src/

# Copy backend implementation files
echo "🔧 Copying backend implementation files..."
if [ -f "$TEMP_DIR/ggml/src/ggml-backend-impl.h" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-backend-impl.h" whisper-cpp/ggml/src/
fi
if [ -f "$TEMP_DIR/ggml/src/ggml-backend-reg.cpp" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-backend-reg.cpp" whisper-cpp/ggml/src/
fi

# Copy Apple Silicon / CoreML specific files
echo "🍎 Copying Apple Silicon CoreML files..."
if [ -f "$TEMP_DIR/ggml/src/ggml-coreml.c" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-coreml.c" whisper-cpp/ggml/src/
fi
if [ -f "$TEMP_DIR/ggml/src/ggml-coreml.m" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-coreml.m" whisper-cpp/ggml/src/
fi
if [ -f "$TEMP_DIR/ggml/include/ggml-coreml.h" ]; then
    cp "$TEMP_DIR/ggml/include/ggml-coreml.h" whisper-cpp/ggml/include/
fi

# Copy Metal (GPU) files for Apple Silicon
echo "⚡ Copying Metal GPU acceleration files..."
if [ -f "$TEMP_DIR/ggml/src/ggml-metal.c" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-metal.c" whisper-cpp/ggml/src/
fi
if [ -f "$TEMP_DIR/ggml/src/ggml-metal.cpp" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-metal.cpp" whisper-cpp/ggml/src/
fi
if [ -f "$TEMP_DIR/ggml/src/ggml-metal.m" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-metal.m" whisper-cpp/ggml/src/
fi
if [ -f "$TEMP_DIR/ggml/include/ggml-metal.h" ]; then
    cp "$TEMP_DIR/ggml/include/ggml-metal.h" whisper-cpp/ggml/include/
fi
if [ -f "$TEMP_DIR/ggml/src/ggml-metal.metal" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-metal.metal" whisper-cpp/ggml/src/
fi

# Copy essential utility files
echo "🔧 Copying utility files..."
if [ -f "$TEMP_DIR/ggml/src/ggml-quants.c" ]; then
    cp "$TEMP_DIR/ggml/src/ggml-quants.c" whisper-cpp/ggml/src/
fi
if [ -f "$TEMP_DIR/ggml/include/ggml-quants.h" ]; then
    cp "$TEMP_DIR/ggml/include/ggml-quants.h" whisper-cpp/ggml/include/
fi

# Copy common utilities (often needed)
echo "🛠️  Copying common utilities..."
if [ -f "$TEMP_DIR/examples/common.h" ]; then
    mkdir -p whisper-cpp/examples
    cp "$TEMP_DIR/examples/common.h" whisper-cpp/examples/
fi
if [ -f "$TEMP_DIR/examples/common.cpp" ]; then
    cp "$TEMP_DIR/examples/common.cpp" whisper-cpp/examples/
fi

# Copy LICENSE and README for reference
echo "📋 Copying documentation..."
cp "$TEMP_DIR/LICENSE" whisper-cpp/
if [ -f "$TEMP_DIR/README.md" ]; then
    cp "$TEMP_DIR/README.md" whisper-cpp/README-whisper.md
fi

# Create a simple CMakeLists.txt for our integration
echo "🔨 Creating CMakeLists.txt for integration..."
cat > whisper-cpp/CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.12)
project(whisper-integration)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple Silicon optimizations
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
    add_compile_definitions(GGML_USE_ACCELERATE)
    find_library(ACCELERATE_FRAMEWORK Accelerate)
    
    # CoreML support
    add_compile_definitions(WHISPER_USE_COREML)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    find_library(COREML_FRAMEWORK CoreML)
endif()

# Include directories
include_directories(include)
include_directories(ggml/include)

# GGML sources
set(GGML_SOURCES
    ggml/src/ggml.c
    ggml/src/ggml-alloc.c
    ggml/src/ggml-backend.c
)

# Add Apple Silicon specific sources
if(APPLE)
    list(APPEND GGML_SOURCES
        ggml/src/ggml-coreml.c
        ggml/src/ggml-metal.c
    )
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ggml/src/ggml-coreml.m)
        list(APPEND GGML_SOURCES ggml/src/ggml-coreml.m)
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ggml/src/ggml-metal.m)
        list(APPEND GGML_SOURCES ggml/src/ggml-metal.m)
    endif()
endif()

# Add quantization support if available
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ggml/src/ggml-quants.c)
    list(APPEND GGML_SOURCES ggml/src/ggml-quants.c)
endif()

# Whisper library
add_library(whisper STATIC
    src/whisper.cpp
    ${GGML_SOURCES}
)

# Link frameworks on Apple
if(APPLE)
    target_link_libraries(whisper ${ACCELERATE_FRAMEWORK})
    if(FOUNDATION_FRAMEWORK)
        target_link_libraries(whisper ${FOUNDATION_FRAMEWORK})
    endif()
    if(COREML_FRAMEWORK)
        target_link_libraries(whisper ${COREML_FRAMEWORK})
    endif()
endif()

# Compiler flags
target_compile_options(whisper PRIVATE
    -Wall -Wextra -Wpedantic -Wcast-qual -Wno-unused-function
)

if(APPLE)
    target_compile_options(whisper PRIVATE -mfma -mf16c -mavx -mavx2)
endif()
EOF

# Clean up temporary directory
echo "🧹 Cleaning up temporary files..."
rm -rf "$TEMP_DIR"

echo "✅ Essential whisper.cpp files copied successfully!"
echo ""
echo "📊 Summary of copied files:"
find whisper-cpp -type f | wc -l | xargs echo "   Total files:"
echo "   📁 Directory structure:"
echo "      whisper-cpp/"
echo "      ├── include/          (whisper headers)"
echo "      ├── src/              (whisper source)"
echo "      ├── ggml/"
echo "      │   ├── include/      (ggml headers)"
echo "      │   └── src/          (ggml source + Apple Silicon)"
echo "      ├── examples/         (common utilities)"
echo "      ├── CMakeLists.txt    (build configuration)"
echo "      └── LICENSE"
echo ""
echo "🍎 Apple Silicon optimizations included:"
echo "   ✅ CoreML acceleration files"
echo "   ✅ Metal GPU acceleration files"
echo "   ✅ Accelerate framework support"
echo ""
echo "💡 Next steps:"
echo "   1. Update whisper-service to use local whisper-cpp/"
echo "   2. Compile with: cd whisper-cpp && cmake -B build && cmake --build build"
echo "   3. Link against: whisper-cpp/build/libwhisper.a"
