#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"

if command -v cmake >/dev/null 2>&1; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$BUILD_DIR" -- -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    exit 0
fi

echo "cmake not found, using direct g++ build fallback"

if ! command -v c++ >/dev/null 2>&1; then
    echo "error: C++ compiler not found (c++)"
    exit 1
fi

print_install_hint() {
    cat <<'EOF'
Install missing host dependencies, e.g.:
  Ubuntu/Debian:
    sudo apt-get install -y build-essential pkg-config libsdl1.2-dev libfreetype-dev libpng-dev
  Fedora:
    sudo dnf install -y gcc-c++ pkgconf-pkg-config SDL-devel freetype-devel libpng-devel
  Arch:
    sudo pacman -S --needed base-devel pkgconf sdl freetype2 libpng
EOF
}

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "error: pkg-config not found"
    print_install_hint
    exit 1
fi

SDL_CFLAGS=""
SDL_LIBS=""
if pkg-config --exists sdl; then
    SDL_CFLAGS="$(pkg-config --cflags sdl)"
    SDL_LIBS="$(pkg-config --libs sdl)"
elif command -v sdl-config >/dev/null 2>&1; then
    SDL_CFLAGS="$(sdl-config --cflags)"
    SDL_LIBS="$(sdl-config --libs)"
else
    echo "error: missing dependency 'sdl' (pkg-config or sdl-config)"
    print_install_hint
    exit 1
fi

for dep in freetype2 libpng; do
    if ! pkg-config --exists "$dep"; then
        echo "error: missing dependency '$dep' (pkg-config)"
        print_install_hint
        exit 1
    fi
done

mkdir -p "$BUILD_DIR"

CXXFLAGS="$SDL_CFLAGS $(pkg-config --cflags freetype2 libpng)"
LDFLAGS="$SDL_LIBS $(pkg-config --libs freetype2 libpng) -lpthread"

c++ -std=c++17 -O2 -Wall -Wextra \
    $CXXFLAGS \
    "$ROOT_DIR/src/main.cpp" \
    "$ROOT_DIR/src/runtime_core.cpp" \
    "$ROOT_DIR/src/runtime_input.cpp" \
    "$ROOT_DIR/src/runtime_renderer.cpp" \
    "$ROOT_DIR/src/runtime_scenes.cpp" \
    $LDFLAGS \
    -o "$BUILD_DIR/inconsole-runtime-x86"
