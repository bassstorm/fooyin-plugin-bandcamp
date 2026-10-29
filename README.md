# Fooyin Bandcamp plugin

Adds Bandcamp as an audio source to [Fooyin](https://www.fooyin.org/).

## Features

- Brings Bandcamp albums, tracks, and discographies into Fooyin.

## Installation

Download the `.fyplugin` bundle for your platform from a release, then install it
in Fooyin via **Settings → Plugins → Install…** and select the file.

Each bundle targets one operating system, one CPU architecture, and one Fooyin
version. Choose the bundle that matches your system and Fooyin.

## Requirements

### Runtime

- libxml2.

### Compile time

- CMake 3.21 or newer and a C++23 compiler.
- Qt 6 development files: Core, Concurrent, and Network.
- libxml2 development files, for example `libxml2-dev`.
- A Fooyin development install: headers, the CMake package config, and libraries.

CMake finds Fooyin through `Fooyin_ROOT`, which points to the Fooyin install
prefix. If Fooyin's development package is installed system-wide, you can skip
this step. To build Fooyin from source instead:

```sh
cmake -S ../fooyin -B ../fooyin/build \
  -DINSTALL_HEADERS=ON \
  -DBUILD_PLUGINS=OFF \
  -DBUILD_TRANSLATIONS=OFF \
  -DBUILD_ALSA=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/fooyin-dev"
cmake --build ../fooyin/build --target fooyin -j2
cmake --install ../fooyin/build
export Fooyin_ROOT="$HOME/.local/fooyin-dev"
```

### Dev container

The [.devcontainer/](.devcontainer/) config provides the compile-time requirements
above as a ready-made environment, including Fooyin built from source by the
`fooyin` feature. Its `version` option selects the release tag (default `latest`).

`SSH_AUTH_SOCK` must be set on the host before the container starts. Its socket is
bind-mounted to `/ssh-agent`, and `remoteEnv` points `SSH_AUTH_SOCK` at that path.
The variable therefore applies to terminals and tasks, not to plain `docker exec`
sessions.

## Build

Build and install the plugin against the Fooyin development install from
[Compile time](#compile-time):

```sh
cmake --preset dev
cmake --build --preset dev
cmake --install build
```

The module is installed to `lib/fooyin/plugins` under the Fooyin prefix CMake
found.

To build a bundle for sharing:

```sh
cmake --preset release
cmake --build --preset bundle
```

The bundle is written to `build-release/`, for example
`fooyin-bandcamp-<version>-<platform>.fyplugin`.

## Test

Run the unit tests, which need no network access:

```sh
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

To also check for memory and undefined-behavior errors, use the
`tests-sanitize` preset instead (AddressSanitizer and UndefinedBehaviorSanitizer).
