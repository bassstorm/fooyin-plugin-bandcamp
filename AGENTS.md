# AGENTS.md

This repository implements a Bandcamp integration plugin for Fooyin. This document defines the durable engineering principles, architectural boundaries, and domain invariants that govern the codebase. Concrete schemas, field mappings, and API signatures live in the code itself.

## 1. Core Engineering Principles

### Clean Architecture & Single Responsibility

Each stage of the pipeline has one clear owner and responsibility:

| Stage | Responsibility | Does Not Own |
| --- | --- | --- |
| **HTTP Client** (`bandcampclient.*`) | Bounded single-request HTTP fetch, redirect handling, effective URL, and reply lifetime. | Retries, HTML/JSON parsing, or caching. |
| **Parser** (`bandcampparser.*`, `bandcamphtmlreader.*`) | Extracting Bandcamp `data-tralbum` JSON and discography listing structure into typed candidates with provenance. | Network requests, cross-page reconciliation, or final scope validity. |
| **Assembly & Admission** (`bandcampassembly.*`) | Reconciling track and parent-album candidates, enforcing authority rules, and validating required fields for the requested scope. | Raw HTML/JSON access or heuristic title/number cleanup. |
| **Post-Processors** (`bandcamppostprocessor.*`) | Ordered semantic refinement of admitted album metadata (first) and track metadata (second). | Rejecting resources, removing tracks, changing identities/streams, or raw source access. |
| **Resolver & Orchestrator** (`bandcampattemptsupervisor.*`, `bandcamplistingresolver.*`, `bandcampreleaseresolver.*`, `bandcamporchestrator.*`, `bandcampresolutionservice.*`) | Discography unfolding, request deduplication, parent acquisition, bounded parallelism, operation-scoped domain caching, and input-order preservation. | Source parsing or Fooyin database/playlist mutations. |
| **Fooyin Integration** (`bandcampfooyintrack.*`, `bandcamplibrarygateway.*`, `bandcampimportcontroller.*`, `bandcamprefreshcontroller.*`, `bandcampplaybackpreparation.*`, `bandcampplayback.*`, `bandcampprogressdialog.*`, `bandcampplugin.*`) | Projecting domain tracks to Fooyin tracks, one-way snapshot transport writes, playlist import, stream refresh, and playback bridging. | Metadata normalization or Bandcamp source interpretation. |

Do not split a single state machine across layers or mix UI batch workflows with single-track audio-thread preparation.

### KISS & YAGNI

- Build only what production code uses today. Do not introduce speculative abstractions, unused scope/clock wrappers, or test-only hooks in production headers.
- Avoid redundant multi-pass work (such as building duplicate JSON DOM trees) when a single pass suffices.
- Share stateless helpers and compiled regular expressions rather than duplicating them across translation units.

### Boundary Validation & Internal Trust

- **Validate untrusted input once at the entry boundary** that interprets it:
  - User URL inputs at the URL parser/dialog boundary (`bandcampurl.*`, `bandcampurldialog.*`).
  - Network responses and Bandcamp HTML/JSON at the HTTP client and parser boundaries (`bandcampclient.*`, `bandcampparser.*`).
  - Persisted Fooyin `ExtraProperties` when reading transport state back from resident tracks (`bandcampfooyintrack.*`).
  - Persisted user settings when loading configuration (`bandcampsettings.*`).
- **Internal consumers trust producer contracts.** Once a value is parsed into a validated internal type or marked `Usable`, downstream layers (`Orchestrator`, `Resolver`, `Assembly`, `Post-Processors`, `Fooyin Track Projection`) must not re-parse URLs, re-clamp options, re-verify key uniqueness, or wrap internal callbacks/signals in defensive `try/catch` blocks.
- **Stop at the deciding boundary.** When an owning stage determines a terminal failure for the requested scope, return that structured failure immediately rather than deferring or synthesizing fallback errors downstream.

### Fooyin Host Seam Trust

- **Trust Fooyin's public C++ plugin contract.** Assume Fooyin's main-thread services (`MusicLibrary`, `PlaylistHandler`, `PlaylistInteractor`, `SettingsManager`) obey their API contracts, stay on their owning thread, and do not throw C++ exceptions from normal operations.
- **Guard only real lifecycle and user-action conditions:**
  - Use `QPointer` / shutdown checks where asynchronous operations may outlive a UI widget or plugin teardown.
  - Check user-driven state (e.g., whether a target playlist still exists or is writable, whether tracks are selected).
- **Do not test Fooyin itself.** Tests verify plugin behavior within its own boundaries; never write tests that simulate impossible Fooyin API misbehavior (e.g., throwing getters or bogus write counts).

### Internal Reliability & Host Safety

- **Never crash or block Fooyin.** Fooyin process stability takes precedence over completing an operation. No blocking network wait may run on the UI thread.
- **Bound external resources.** Enforce explicit limits on response bytes, parser nesting/sizes, redirects, in-flight requests, per-attempt deadlines, and a bounded retry budget. Resource-limit violations return structured failures, never silent truncation.
- **Deterministic async lifetimes.** Every async operation must reach **exactly one** terminal state (`Success`, `Failure`, or `Cancelled`). Guard `QObject::connect` lambdas with the true receiver lifetime, synchronize cross-thread state shared with the audio thread, and cancel/clean up child operations and dialogs deterministically on `shutdown()` and destruction.
- **Explicit outcomes over exceptions.** Use typed result/outcome values (`ResolutionOutcome`) for expected failures. Contain exceptions only at external library or top-level host decoder entry points.

---

## 2. Domain & Resolution Invariants

### Source vs. Domain Separation

- Bandcamp's web representation (`data-tralbum` JSON and discography HTML) is inconsistent in the wild. Only the parser reads it, producing typed candidates and provenance (`Explicit`, `Fallback`, `Inherited`, `Derived`).
- Domain models (`BandcampAlbum`, `BandcampTrack` in [bandcampmodel.h](src/bandcampmodel.h)) admit only entities satisfying all required fields (positive stable ID, canonical release `sourceUrl`, nonblank title, and valid HTTPS `streamUrl` for tracks) and exclude parser/operational context.

### Upward-Only Fetch Direction

- Resolution fetches **upward** for authority and context, never downward into member track pages:
  - **Album input:** Fetch the album page and resolve its listed children. Never fetch individual member track pages.
  - **Track input with a parent:** Fetch the track page, then fetch its parent album page and reconcile the selected track by stable ID.
  - **Standalone track input:** Fetch the track page only (requires explicit no-parent evidence; an invalid parent link on a track with a declared `album_id` is a failure, not a standalone track).
  - **Discography input:** Unfold listing continuation pages into release references, then resolve each referenced release without fetching album-member track pages.

### Authority, Reconciliation & Normalization

- **Parent album authority:** The resolved parent album is authoritative for album metadata (`title`, `artist`, `releaseDate`, `sourceUrl`), declared membership, `trackTotal`, and album-backed track streams. An unavailable album child stream cannot be bypassed with an older track-page stream.
- **Track facts vs. album facts:** Explicit album-child values (`title`, `artist`, `trackNumber`, `duration`) precede conflicting track-page equivalents, while missing child fields may fall back to explicit track-page values. Track-owned `releaseDate` preserves explicit track values and inherits the album date only when missing.
- **Provenance precedence:** Derived, fallback, or inherited values never overwrite explicit facts.
- **Narrow normalization:** Post-processors refine optional metadata (artist inference, dirty-title prefix stripping, track-number repair) only when preconditions hold across the stable sibling set, preserving required fields and legitimate artist/title differences.
- **One-field-one-postprocessor:** Each post-processor owns and mutates a single metadata field, so its name uniquely identifies the field it changes.

### Scope & Atomicity

- **Whole album:** Atomic. Requires valid album metadata and every declared child to be admitted with a valid stream. One unresolved child fails the album; never publish a partial track subset as an album result.
- **Single album track:** Requires valid parent album metadata and the selected child matched by stable track ID. Unrelated broken siblings do not fail a single-track selection, and `trackTotal` still reflects the full declared album count.
- **Collection / multi-URL:** Each requested item resolves to its own outcome; one failed item does not abort unrelated items.

### Track Identity, Persistence & Stream Refresh

- **Canonical identity:** Every Fooyin Bandcamp track uses `bandcamp://track/<trackId>` as its immutable `filepath` whether imported from an album, track page, or discography.
- **Separated transport state:** Mutable playback locators (`_STREAM_PROVIDER`, `_BANDCAMP_SOURCE_URL`, `_BANDCAMP_STREAM_URL`, `_BANDCAMP_EXPIRES_AT`) live in Fooyin `ExtraProperties`. `sourceUrl` is always the highest containing release (album URL for album members, track URL for standalone tracks).
- **One-way snapshot persistence:** Re-importing or refreshing existing tracks snapshots resident records at commit start (preferring library residents, then the first matching playlist occurrence), preserves user-edited metadata and extra tags, and merges only transport properties.
- **Refresh by release `sourceUrl`:** Manual refresh and playback preparation refresh streams that are expired, untrusted, or within the configured remaining-lifetime window by resolving deduplicated canonical `sourceUrl` releases and matching tracks by stable ID.

---

## 3. Diagnostics, Testing & Verification

### Logging & Redaction

- **Diagnostic sufficiency & signal over noise:** Logs (`fy.bandcamp.<area>` and `fy.trace.bandcamp.<area>` in [bandcamplogging.h](src/bandcamplogging.h)) reconstruct why an operation reached its outcome and why admitted metadata has its values (anomalies, conflicts, effective mutations, resilience actions, terminal failures). Direct primary-source mappings, implicit album-to-track inheritance, and no-ops are silent.
- **Severity by signal role:** `Warning` for degraded execution (retries, clamps) and terminal failures; `Info` for coarse lifecycle milestones and batch summaries; `Debug` for admitted entity summaries and metadata deltas; `Trace` for raw source snapshots, wire-level fetch metrics, and per-item enumerations. Keep disabled `Trace` instrumentation near-zero cost.
- **Entity correlation without repetition:** Correlate records by stable domain entity ID (`album`, `track`), falling back to the full canonical URL only before an entity ID is established. Never invent synthetic job/request counters or repeat the URL once an entity ID is known.
- **Single-record failures and retries:** Log each retry and terminal failure once at `Warning` at its deciding boundary; upstream propagation and user cancellation are silent.
- **Lightweight URL cleanup:** Log full canonical URLs (preserving custom domains) with query, fragment, and userinfo stripped at `QUrl` formatting boundaries. Never regex-scan free text or raw JSON; signed stream locators may appear only inside raw `Trace` source snapshots.

### Testing & Build Verification

- **Deterministic suite (`tests/deterministic/`):** Covers parser extraction, assembly/authority, post-processors, orchestration/resolution, identity/transport persistence, playback bridging, logging/redaction, and resource/lifecycle bounds using synthetic inputs and contract-valid internal inputs.
- **Live suite (`tests/live/`):** Happy-path smoke tests against real Bandcamp URLs (`ctest --preset tests-live`), excluded from CI (`tests-ci`).
- **Formatting:** Format C++ with `cmake --preset dev && cmake --build --preset format` (clang-format 22 from the dev container, using the repository `.clang-format`). CI fails on violations via `format-check`.
- Validate code changes with the sanitized deterministic suite:

```sh
cmake --preset tests-sanitize
cmake --build --preset tests-sanitize
ctest --preset tests-ci --output-on-failure
```
