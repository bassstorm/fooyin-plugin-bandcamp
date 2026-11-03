#!/usr/bin/env bash
# Installs Fooyin at the requested ref to /usr/local, where find_package(Fooyin) finds it
# without Fooyin_ROOT. Build dependencies come from that ref's ci/ubuntu-depends.sh, so no
# package list is kept here. The source clone stays at SRC_DIR to read Fooyin code.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "The fooyin feature must run as root." >&2
    exit 1
fi

REQUESTED="${VERSION:-latest}"
REPO_URL="https://github.com/fooyin/fooyin.git"
SRC_DIR="/usr/local/src/fooyin"

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

BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

export GIT_TERMINAL_PROMPT=0
rm -rf "$SRC_DIR"
mkdir -p "$(dirname "$SRC_DIR")"
git clone --depth 1 --branch "$REF" --recurse-submodules --shallow-submodules \
    "$REPO_URL" "$SRC_DIR"
chmod -R a+rX "$SRC_DIR"

(cd "$SRC_DIR" && bash ci/ubuntu-depends.sh)

cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DINSTALL_HEADERS=ON \
    -DBUILD_PLUGINS=OFF \
    -DBUILD_TRANSLATIONS=OFF \
    -DBUILD_ALSA=OFF \
    -DBUILD_TESTING=OFF

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
cmake --install "$BUILD_DIR"
ldconfig
rm -rf /var/lib/apt/lists/*

echo "Installed Fooyin ${REF} to /usr/local (source kept in ${SRC_DIR})"
