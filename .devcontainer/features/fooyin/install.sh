#!/usr/bin/env bash
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "The fooyin feature must run as root." >&2
    exit 1
fi

VERSION="${VERSION:-latest}"
SRC_DIR="/usr/local/src/fooyin"

apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config ca-certificates curl git xz-utils jq \
    qt6-base-dev libqt6svg6 libqt6sql6-sqlite \
    libavcodec60 libavfilter9 libavformat60 libavutil58 libswresample4 \
    libicu74 libtag1v5 libarchive13t64 zlib1g libqcoro6core0t64 libqcoro6network0t64

if [ "$VERSION" = "latest" ]; then
    RELEASE_DATA="$(curl -fsSL https://api.github.com/repos/fooyin/fooyin/releases/latest)"
    TAG="$(printf '%s' "$RELEASE_DATA" | jq -er .tag_name)"
    VER="${TAG#v}"
else
    TAG="v${VERSION#v}"
    VER="${VERSION#v}"
    RELEASE_DATA="$(curl -fsSL "https://api.github.com/repos/fooyin/fooyin/releases/tags/${TAG}")"
fi

SDK_URL="$(printf '%s' "$RELEASE_DATA" | jq -er '.assets[] | select(.name | test("^fooyin-sdk-.*-linux-x64.*\\.tar\\.xz$")) | .browser_download_url')"
SDK_ARCHIVE="/tmp/fooyin-sdk-${VER}.tar.xz"
trap 'rm -f "$SDK_ARCHIVE"; rm -rf /var/lib/apt/lists/*' EXIT

export GIT_TERMINAL_PROMPT=0
rm -rf "$SRC_DIR"
mkdir -p "$(dirname "$SRC_DIR")"
git clone --depth 1 --branch "$TAG" "https://github.com/fooyin/fooyin.git" "$SRC_DIR"
chmod -R a+rX "$SRC_DIR"

curl -fsSL "$SDK_URL" -o "$SDK_ARCHIVE"
tar -xJf "$SDK_ARCHIVE" -C /usr/local --strip-components=1

printf '%s\n' /usr/local/lib/fooyin > /etc/ld.so.conf.d/fooyin.conf
ldconfig
rm -f "$SDK_ARCHIVE"
rm -rf /var/lib/apt/lists/*

echo "Installed Fooyin ${TAG} SDK to /usr/local (source kept in ${SRC_DIR})"
