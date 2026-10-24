# Fooyin Bandcamp plugin

External Fooyin plugin for Bandcamp albums and tracks. It is a work in progress:
the plugin resolves Bandcamp URLs and shows what it found. Playlist creation and
playback come in later steps. See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)
for the implementation assessment and consolidated development plan.

## Build

The build can use a Fooyin development install already on your system. To keep
the Fooyin libraries, headers, and plugin build together under this project's
ignored `build/` directory, with the Fooyin source checkout at `../fooyin`, run:

```sh
cmake -S ../fooyin -B build/fooyin \
  -DINSTALL_HEADERS=ON \
  -DBUILD_PLUGINS=OFF \
  -DBUILD_TRANSLATIONS=OFF \
  -DBUILD_ALSA=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/fooyin-install"
cmake --build build/fooyin --target fooyin -j2
cmake --install build/fooyin

cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="$PWD/build/fooyin-install" \
  -DFooyin_DIR="$PWD/build/fooyin-install/lib/cmake/fooyin" \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/fooyin-install"
cmake --build build
```

This leaves the plugin at `build/fyplugin_bandcamp.so` and the local Fooyin
development install at `build/fooyin-install/`. The module is built as
`fyplugin_bandcamp.so` on Linux. To install the plugin into that local Fooyin
installation, run:

```sh
cmake --install build
```

If the Fooyin checkout is elsewhere, replace `../fooyin` with its path. If you
already have Fooyin's development package installed, omit the local Fooyin
build/install steps and set `CMAKE_PREFIX_PATH` to that package instead.

### Unit tests

The unit tests cover URL validation, typed parser candidates, metadata admission
and parent relationships, isolated and composed semantic processors,
required-parent service behavior, settings, TRACE diagnostics, and the network
client. They are off by default. To run them:

```sh
cmake -S . -B build-tests -DBANDCAMP_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH="$PWD/build/fooyin-install"
cmake --build build-tests --target bandcamp_unit_tests
ctest --test-dir build-tests --output-on-failure
```

Add `-DBANDCAMP_SANITIZE=ON` to build with AddressSanitizer and
UndefinedBehaviorSanitizer. The sanitizer flags apply to the shared core library
and to the plugin, which links it. Set `BANDCAMP_MUTATION_ITERATIONS` to change
the number of mutation iterations (default 10000).

### Live probe

The live probe is the current manual tool for fetching real Bandcamp pages and
checking the production resolution path against them. It is not wired into the plugin build or
CTest. It needs network access and saves nothing to disk. Its replacement with
production-chain integration tests is covered by
[the development plan](DEVELOPMENT_PLAN.md#increment-6---live-integration-tests-and-probe-retirement).
Until that replacement is available, run it when Bandcamp changes its pages,
or to check a new URL.

Build it against the Qt 6 install (Core, Network, and Concurrent are required):

```sh
cmake -S tests/integration -B build/probe -DCMAKE_PREFIX_PATH=/usr
cmake --build build/probe -j4
```

Run it with no arguments to check the agreed set: two discographies (reported
as SKIP until discography expansion exists), an album, a track with an album,
and a standalone track. Pass URLs to check others:

```sh
build/probe/bandcamp-probe
build/probe/bandcamp-probe https://offland.bandcamp.com/album/parallels
build/probe/bandcamp-probe -p 4 -t 3000 <url> <url>
```

- `-p`, `--parallel n`: pages fetched or parsed at once, a positive integer (default 2; the settings range is 1-8).
- `-t`, `--timeout ms`: per-item time budget in milliseconds, a positive integer (default 10000; the settings range is 1000-120000).
- `QT_LOGGING_RULES="fy.bandcamp.*.debug=true"`: adds the plugin's debug logs.

For each album and track page, the output shows:

- `fy.trace.bandcamp.parser` records on stderr: production parser source
  snapshots (compact, full decoded `data-tralbum` JSON) and field mappings.
  URL queries, fragments and credentials are redacted. The probe enables this
  category; it no longer extracts raw JSON or fetches pages twice.
- `album:` and `tracks:`, or `track:`: every resolved field, including empty
  ones shown as `""`. Durations are in milliseconds. Effective processor changes
  are logged with old/new values, provenance, and reason in correlated
  `fy.trace.bandcamp.tracks` records. The probe enables these lines itself.
- `FAIL` and `note` lines from the checks. Admission failures are structured
  outcomes; no partial album or warning list is presented as a successful result.

Expected values for the agreed set are in the `Snapshots` table in
[bandcamp_probe.cpp](tests/integration/bandcamp_probe.cpp). A mismatch can mean
Bandcamp changed its pages or the model, or source mapping, assembly, or
post-processing regressed. The probe exits with 1 when any page fails.

### Internal parser boundary

`parseReleasePage` returns a tagged `ReleaseParseOutcome` containing typed album
or track-page candidates, or a structured parse error. Optional values retain
missing/invalid/usable state and semantic provenance. Declared children retain
their order even when malformed or streamless; parent identity/location and
track-page fallback context remain separate evidence for assembly.
Identity conversion validates original decimal tokens (including exact integral
exponents) without floating-point rounding. Supported HTTPS stream strings retain
their original signed queries verbatim. Pure admission validates required
metadata and relationships before the album-first, track-second non-failing
semantic passes. Whole albums are atomic, while a selected album track retains
validated parent metadata without requiring every sibling to be valid.

Input limits are explicit in `bandcampparser.h`: 20 MiB response, 64 JSON
container levels, 16,384 UTF-16 units per source string/key, 100,000 source values
(including containers), and 10,000 declared rows. Exceeding a limit fails parsing,
never truncates metadata or returns a successful subset. Source snapshots are
bounded by response size/depth and only serialized when parser TRACE is enabled.
