#!/bin/bash

echo ""
echo "=== DEPRECATED ==="
echo "install.sh has been replaced by two scripts:"
echo "  1. ./runmetoinstalleverythingfirst  (install prerequisites + download models)"
echo "  2. ./runmetobuildeverything          (build all binaries)"
echo ""
echo "Please use those scripts instead."
echo "See .zencoder/rules/build.md for build configuration reference."
echo "=================="
echo ""

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
exec "$SCRIPT_DIR/runmetoinstalleverythingfirst"
