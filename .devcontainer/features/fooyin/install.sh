#!/usr/bin/env bash
# Clones Fooyin at the requested ref, installs the build dependencies that the same
# ref's CI script declares, then builds with the README options and installs to
# /usr/local, where CMake's find_package(Fooyin) finds it without Fooyin_ROOT.
# Dependencies follow the ref, so no package list is maintained here.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "The fooyin feature must run as root." >&2
    exit 1
fi

REQUESTED="${VERSION:-latest}"
REPO_URL="https://github.com/fooyin/fooyin.git"

if [ "$REQUESTED" = "latest" ]; then
    REF="$(curl -fsSL https://api.github.com/repos/fooyin/fooyin/releases/latest \
        | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')"
    if [ -z "$REF" ]; then
        echo "Could not resolve the latest Fooyin release." >&2
        exit 1
    fi
else
    REF="$REQUESTED"
fi

echo "Building Fooyin ${REF}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

export GIT_TERMINAL_PROMPT=0
git clone --depth 1 --branch "$REF" --recurse-submodules --shallow-submodules \
    "$REPO_URL" "$WORK_DIR/fooyin"

(cd "$WORK_DIR/fooyin" && bash ci/ubuntu-depends.sh)

cmake -S "$WORK_DIR/fooyin" -B "$WORK_DIR/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DINSTALL_HEADERS=ON \
    -DBUILD_PLUGINS=OFF \
    -DBUILD_TRANSLATIONS=OFF \
    -DBUILD_ALSA=OFF \
    -DBUILD_TESTING=OFF

cmake --build "$WORK_DIR/build" --parallel "$(nproc)"
cmake --install "$WORK_DIR/build"
ldconfig
rm -rf /var/lib/apt/lists/*

echo "Installed Fooyin ${REF} to /usr/local"
