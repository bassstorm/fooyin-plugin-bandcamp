#!/usr/bin/env bash
set -euo pipefail

FOOYIN_VERSION="${1:-${FOOYIN_VERSION:-0.15.0}}"

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config ca-certificates curl xz-utils jq \
  clang-format-20 qt6-base-dev libqt6svg6 libqt6sql6-sqlite \
  libavcodec60 libavfilter9 libavformat60 libavutil58 libswresample4 \
  libicu74 libtag1v5 libarchive13t64 zlib1g libqcoro6core0t64 libqcoro6network0t64

sdk_url="$(curl -fsSL "https://api.github.com/repos/fooyin/fooyin/releases/tags/v${FOOYIN_VERSION}" | \
  jq -er '.assets[] | select(.name | test("^fooyin-sdk-.*-linux-x64.*\\.tar\\.xz$")) | .browser_download_url')"
curl -fsSL "$sdk_url" | sudo tar -xJ -C /usr/local --strip-components=1
echo /usr/local/lib/fooyin | sudo tee /etc/ld.so.conf.d/fooyin.conf >/dev/null
sudo ldconfig
