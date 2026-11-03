# Fooyin Bandcamp plugin

Adds Bandcamp as an audio source to [Fooyin](https://www.fooyin.org/).

## Features

- Import Bandcamp albums, tracks, and discographies into an existing or new
  Fooyin playlist as regular playlist entries with editable track properties.
- Play imported tracks from available Bandcamp MP3 streams; playback refreshes
  expiring streams using the configured refresh window.
- Manually refresh streams for selected Bandcamp tracks from the track context
  menu, using the configured stream refresh window.

Playback depends on an available Bandcamp stream and Fooyin MP3 decoder.
Sequential Bandcamp streams are reported as non-seekable. If a decoder probe
consumes a sequential stream before failing, the plugin does not reopen the
network stream to try another decoder.

## Installation

Download the `.fyplugin` bundle from a release, then install it in Fooyin via
**Settings → Plugins → Install…** and select the file.

> [NOTE]
> Currently only `linux-x64` is officially supported. It is the only platform
> that is actively used and tested.

## Requirements

### Runtime

- None beyond Fooyin requirements.

### Compile time

- CMake 3.21 or newer and a C++23 compiler.
- Qt 6 development files: Core, Concurrent, and Network.
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
`fooyin` feature. The feature keeps the Fooyin source at `/usr/local/src/fooyin`.

`SSH_AUTH_SOCK` must be set on the host before the container starts. Its socket is
bind-mounted to `/ssh-agent`, and `remoteEnv` points `SSH_AUTH_SOCK` at that path.
The variable therefore applies to terminals and tasks, not to plain `docker exec`
sessions.

## Build

| Build preset      | Configure preset | Purpose |
| ----------------- | ---------------- | ------- |
| `dev`             | `dev`            | Local development build. Install it into your Fooyin with `cmake --install build`. |
| `release`         | `release`        | Optimized module without packaging. |
| `release-bundle`  | `release`        | Packaged `.fyplugin` for sharing. |
| `snapshot-bundle` | `snapshot`       | Same bundle with `-snapshot` appended to its version, so it can't be mistaken for a release. |

To build a bundle:

```sh
cmake --preset release
cmake --build --preset release-bundle
```

## Test

| Test preset      | Configure preset | Use it for |
| ---------------- | ---------------- | ---------- |
| `tests`          | `tests`          | Full local check. Live tests need network access. |
| `tests-sanitize` | `tests-sanitize` | Same as `tests`, built with ASan and UBSan. Slower, but catches memory and undefined-behavior bugs. |
| `tests-ci`       | `tests-sanitize` | Deterministic tests only, offline. This is what CI runs. |
| `tests-live`     | `tests`          | Bandcamp compatibility only. A failure can mean Bandcamp changed or is unreachable, not that the plugin regressed. |

To run the full local check:

```sh
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```
