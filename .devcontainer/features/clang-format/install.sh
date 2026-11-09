#!/usr/bin/env bash
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "The clang-format feature must run as root." >&2
    exit 1
fi

CLANG_FORMAT_MAJOR=20

apt-get update
apt-get install -y --no-install-recommends "clang-format-${CLANG_FORMAT_MAJOR}"
update-alternatives --install /usr/bin/clang-format clang-format "/usr/bin/clang-format-${CLANG_FORMAT_MAJOR}" 100
rm -rf /var/lib/apt/lists/*

clang-format --version
