#!/usr/bin/env bash
# Debian ships only the versioned binary, so CMake looks it up as clang-format-<major>.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "The clang-format feature must run as root." >&2
    exit 1
fi

CLANG_FORMAT_MAJOR=22

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends "clang-format-${CLANG_FORMAT_MAJOR}"
rm -rf /var/lib/apt/lists/*

"clang-format-${CLANG_FORMAT_MAJOR}" --version
