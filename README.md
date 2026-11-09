# Fooyin Bandcamp plugin

Stream music from Bandcamp in [Fooyin](https://www.fooyin.org/).

## Features

- **Import by URL**: Add Bandcamp tracks, albums, and full artist or label
  discographies (including custom domains) to a new or existing playlist, with
  configurable discography ordering.
- **Direct streaming & link refresh**: Stream tracks directly in Fooyin.
  Expiring stream links are refreshed automatically before playback, or on
  demand from the track context menu.
- **Open on Bandcamp**: Open the release pages of selected tracks in your
  browser. Buying on Bandcamp is one of the best ways to support artists.
- **Metadata cleanup & refinement**: Automatically cleans up noisy Bandcamp
  tags—stripping redundant artist prefixes from titles, filling in missing
  artist names and release dates, and fixing broken track numbers—while
  preserving your manual tag edits across re-imports and refreshes.
- **Audio property detection**: Optionally inspects streams during import to
  populate codec, bitrate, and sample rate details in your playlist.
- **Configurable network resilience**: Adjust parallel downloads, request
  timeouts, and automatic retries to handle slow connections or rate limits.

## Installation

Download the `.fyplugin` bundle from a release, then install it in Fooyin via
**Settings → Plugins → Install…** and select the file.

> [NOTE]
> Requires a Fooyin release compatible with plugin interface
> `org.fooyin.fooyin.plugin/2`. Currently only `linux-x64` is officially
> supported; it is the only platform that is actively used and tested.

## Building from source

### Requirements

- CMake 3.21 or newer, Ninja, and a C++23 compiler.
- Qt 6 development files: Core, Network, and Widgets.
- A compatible Fooyin SDK (set `Fooyin_ROOT` if it is not installed system-wide).

### Build

To build and install the plugin into your local Fooyin setup:

```sh
cmake --preset dev
cmake --build --preset dev
cmake --install build
```

To package a distributable `.fyplugin` bundle:

```sh
cmake --preset release
cmake --build --preset release-bundle
```
