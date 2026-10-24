# Bandcamp Plugin Development Plan

## 1. Purpose and scope

Bring the existing plugin into conformance with [AGENTS.md](AGENTS.md), then
complete discography import, playlist integration, persistence, stream refresh,
and playback. The contract governs implementation decisions; this plan defines
the migration, feature decisions, dependencies, and completion criteria.

Build on the existing Qt implementation rather than replace it wholesale.
Complete and verify each increment before exposing the dependent feature.
Logging, deterministic tests, and host safety accompany every increment rather
than being deferred to a final hardening step.

### Feature decisions

- Accept album, track, artist, and label discography URLs, including custom
  domains. Expand listings completely through bounded discovery batches.
- Preserve input submission order and each album's declared track order.
  Discography order is configurable: **Default**, **Unsorted**, **Ascending**,
  or **Descending**. Default currently resolves to Unsorted, which preserves
  Bandcamp listing order across all pages. Ascending and Descending sort by
  release date; unknown dates come last in either direction, and equal dates
  retain original listing order.
- Sort each discography independently, not the entire multi-input submission.
  For an album-member selection, the containing album supplies the release
  date used for collection ordering; standalone releases use their own date.
- Reuse the same persisted track identity and preserve user edits on re-add.
  Repeated playlist entries are allowed, including overlapping submissions;
  network request sharing must not erase requested playlist occurrences.
- Append to the playlist selected in the UI, not the currently playing
  playlist. Capture its ID at submission. A "Create as new playlist" option
  overrides this; missing, temporary, locked, or automatic targets require a
  new persistent playlist.
- Create a new playlist only when a successful result is ready to commit.
  One album or album-member track uses the album title; one standalone track
  uses its title; a discography or mixed submission uses "Bandcamp".
  There is no manual naming field.
- Retain progress, per-input failure reporting, user cancellation, and bounded
  retry/backoff for transient page-request failures. Already committed results
  remain when a run is cancelled or its target becomes unavailable.
- Support manual expired-only and forced stream refresh for selected Bandcamp
  tracks, or the UI-selected playlist when no tracks are selected.
- Before playback, refresh an expired stream or one whose remaining lifetime
  is less than a configurable threshold, default **60 minutes**. This is a
  pre-playback freshness check, not a periodic refresh schedule.
- After an initial stream-open failure, permit one additional open of the
  **same URL**, within the attempt deadline. Do not perform another refresh
  as recovery. No selective HTTP-status API addition to Fooyin is required.

## 2. Current implementation assessment

The plugin currently resolves album/track inputs into a results dialog.
Discography expansion, Fooyin track construction, playlist import, and playback
are not implemented. The current plugin and unit-test targets build; the four
QtTest suites report 57 passes, no failures, and no skips. This is a baseline,
not proof of contract compliance: some tests assert superseded behaviour.

| Area | Evidence and necessary change |
| --- | --- |
| Domain and outcomes | [bandcampmodel.h](src/bandcampmodel.h), [bandcampparser.h](src/bandcampparser.h), and [bandcampservice.h](src/bandcampservice.h) expose default-filled models, error strings, warning lists, and a result containing both album and track slots. Separate incomplete candidates from admitted metadata and use tagged outcomes. A selected track must retain validated parent metadata, including album identity/title. |
| Source mapping | [bandcampparser.cpp](src/bandcampparser.cpp) selects the first track row, ignores several identity aliases and declared parent identity, treats page artist/artwork as resolved track context, truncates oversized fields, and skips malformed album rows. Implement the exact mapping/ownership rules, retain declared membership, and preserve unresolved evidence. |
| Assembly and admission | [bandcampservice.cpp](src/bandcampservice.cpp) post-processes each page before fetching the parent. Parent fetch, parse, and timeout failures return partial track success; parent children are not matched by ID. Introduce authoritative assembly and scope validation before processors. |
| Processors | [bandcamppostprocessor.cpp](src/bandcamppostprocessor.cpp) interleaves album and track steps. [bandcampprocessors.cpp](src/bandcampprocessors.cpp) lets album processors mutate children, removes streamless tracks, and reports missing titles through warnings. Reclassify rules, enforce album-first/track-second passes, and move rejection to admission. |
| Scheduling and lifecycle | [bandcampservice.cpp](src/bandcampservice.cpp) queues all inputs and retains all results until a batch ends. Timed-out parse futures can outlive active slots; destruction waits for them. Replace this with bounded operation state, separate physical-work accounting, incremental ordered delivery, and cooperative shutdown. There is no public operation cancellation or shared-request broker. |
| Fetching | [bandcampclient.cpp](src/bandcampclient.cpp) has useful byte/redirect controls, but appends reads before checking limits, lacks a final-read size check, and returns the requested rather than effective response URL. It has no retry/backoff or shared-consumer handling. Keep the transport wrapper and correct these boundaries. |
| Settings and UI | [bandcampsettings.cpp](src/bandcampsettings.cpp) already validates persisted timeout/parallelism. [bandcampplugin.cpp](src/bandcampplugin.cpp) mutates shared service settings and only opens [bandcampresultsdialog.cpp](src/bandcampresultsdialog.cpp). Snapshot per-operation options, add order/freshness settings, and introduce a separate import controller. |
| Diagnostics | [bandcamplogging.cpp](src/bandcamplogging.cpp) has the standard categories, but no native TRACE categories, source snapshots, or propagated correlation. Current logs and warning bags do not implement the agreed transition contract. |
| Tests and build | [tests/unit/CMakeLists.txt](tests/unit/CMakeLists.txt) duplicates production source lists and does not register CTest tests. It does not exercise the network client/service. [tests/integration/CMakeLists.txt](tests/integration/CMakeLists.txt) builds a standalone probe with independent source extraction and live metadata snapshots. Consolidate shared code and add deterministic orchestration tests plus default live CTest coverage. |

### Reuse rather than discard

Preserve the URL classifier's custom-domain support, asynchronous Qt networking,
HTML-entity decoding, checked duration/date parsing, settings validation, small
processor classes, synthetic fixtures, and seeded mutation testing. Adapt their
contracts and tests where necessary; do not preserve incorrect behaviour simply
because a current test expects it.

### Host integration facts

The adjacent Fooyin checkout exposes the required plugin contexts, virtual URI
decoder routing, playlist insertion, database-only metadata editing, and remote
input devices. Implementation must target these actual interfaces:

- `GuiPluginContext::playlistSelection` supplies the UI-selected playlist;
  `PlaylistHandler::activePlaylist()` instead means the playback playlist.
- `PlaylistInteractor::tracksToPlaylist()` and persistent playlist creation
  supply the normal insertion path. Database identity reuse alone does not
  merge the latest user metadata or refreshed transport properties.
- `MusicLibrary` exposes resident tracks, lookup by database ID, metadata
  updates, and asynchronous write results, but no `trackForPath()` method.
  Keep URI lookup and persistence adaptation behind one library gateway.
- `AudioLoader` supports scheme-based decoders and inner decoder selection.
  `AudioSource` carries the opened device and remote-stream view; decoding
  distinguishes temporary starvation from EOF and supports cancellation.
- `RemoteIoService` supplies bounded streaming devices, but its public remote
  interfaces do not expose numeric HTTP status. Do not parse error-message
  text or use private host headers to implement playback recovery.
- Fooyin's cover repository recognizes the `COVERART` extra tag. Standard
  virtual-track metadata editing and restart persistence should be reused,
  not replaced by a plugin-specific editor or database schema.

## 3. Target design

### Components and ownership

Keep value transformations independent of Fooyin. Use QObject classes only for
asynchronous ownership or host/UI interaction. Proposed helper names describe
responsibilities, not a requirement to create a file or class for every row.

| Component | Responsibility and boundary |
| --- | --- |
| Parser | Interpret one bounded source response into typed album/track candidates or an ordered discovery batch. Own raw HTML/JSON, page-kind detection, local mappings, source diagnostics, and opaque continuation interpretation. Never fetch or iterate a whole discography. |
| Assembly/admission functions | Reconcile typed candidates by stable identity and provenance; validate parent relationships and requested scope; construct valid metadata. Pure functions, without network, UI, or source-tree access. |
| Post-processing pipelines | Separate album and track passes on admitted objects, with stable read-only parent/sibling context and semantic provenance. Each rule mutates only its own permitted entity. |
| Network client and request broker | Client owns bounded HTTP exchange and effective response information. Broker owns in-flight sharing, request lifecycle, and consumer subscriptions. Neither decides metadata authority or playlist selection. |
| Resolution operation and scheduler | One operation owns its input scopes, options, deadlines, cancellation, and outcomes. The service schedules discovery, parent acquisition, worker tasks, and bounded ordered delivery across operations. Share requests without conflating consumers' scopes or deadlines. |
| Collection ordering buffer | Preserve listing ordinals and stage chronological results using bounded temporary storage. Own ordering/backpressure, not source parsing or metadata reconciliation. |
| Fooyin track factory and library gateway | Serialize identity, project admitted metadata/artwork, reuse resident identities, and persist transport-only changes while preserving current user edits. Isolate host APIs from the resolution core. |
| Import controller | Own one UI import's target playlist, progress, cancellation, and ordered commits. The plugin class remains the composition/action-registration root; widgets collect input and display outcomes. |
| Stream resolver and decoder | Reuse release acquisition and stable-ID matching for transport refresh. Decoder prepares a sufficiently fresh stream, owns remote input/inner decoder, and performs bounded same-URL reopen recovery. Neither rewrites descriptive metadata. |

### Data and lifecycle rules

- Use tagged parse and resolution results so success/failure/cancellation
  cannot coexist ambiguously. Structured errors carry the contract's stable
  code, stage, resource, identity, field/relationship, and safe reason.
- Candidate fields preserve missing/invalid/usable state and semantic
  provenance outside the final domain model. JSONPaths remain parser-owned
  diagnostics; downstream code consumes typed facts, not raw source objects.
- Represent reusable album metadata independently of whole-album success.
  A selected-track result retains its parent once without requiring every
  sibling to resolve or duplicating album-title/identity concepts.
- Snapshot options at submission. Changing settings cannot alter an existing
  operation's budget, order, or freshness policy.
- Bound submitted work, pending discoveries, live HTTP requests, physical
  parse tasks, retained bytes, reorder buffers, temporary storage, and logging.
  A logical timeout does not free a physical worker slot until work exits.
- Use a bounded admission/reorder window: a slow early item must not allow
  unlimited completed results to accumulate behind it.
- Workers own their inputs and do not depend on service lifetime. Cancel
  cooperatively and guard callbacks; do not block the GUI thread waiting for
  futures during destruction or force-terminate worker threads.
- Discovery duplicates representing the same listing entry may be reconciled
  within that collection. Explicitly repeated inputs and playlist occurrences
  remain separate consumers even when their network work is shared.

## 4. Delivery sequence

| Increment | Depends on | Deliverable |
| --- | --- | --- |
| 1 | None | Shared build/test seams, typed boundaries, diagnostic infrastructure. |
| 2 | 1 | Contract-compliant source candidates and bounded extraction. |
| 3 | 2 | Authoritative assembly, admission, and non-failing processors. |
| 4 | 3 | Bounded operation scheduler, shared acquisition, cancellation, and retries. |
| 5 | 4 | Complete discovery, chronological ordering, and settings. |
| 6 | 5 | Production-chain live tests and probe retirement. |
| 7 | 3 | Fooyin identity, track construction, and persistence gateway. |
| 8 | 5, 6, 7 | Ordered playlist import and operational UI. |
| 9 | 4, 7 | Shared stream refresh and manual actions. |
| 10 | 9 | Playback with conditional refresh and same-URL reopen. |
| 11 | 8, 10 | End-to-end acceptance and user documentation. |

### Increment 1 - Shared foundation and diagnostics

- Extract an internal, reusable Qt resolution target from the plugin source
  list. Link the plugin and tests to the same implementation; keep Fooyin
  integration in the plugin. Do not introduce a public ABI or generic framework.
- Define candidate, scope, parent-state, provenance, outcome, error, operation
  option, and correlation types. Keep unknown membership distinct from an
  established standalone track. Use ordinary value composition and variants.
- Introduce narrow test seams for HTTP responses and time/deadline control.
  Prefer a fake Qt network/reply implementation or a small injected boundary,
  not a second resolver or a real network dependency in unit tests.
- Add `fy.trace.bandcamp.<area>` categories and shared safe event formatting.
  Carry immutable job/request/release/track context through asynchronous work.
  Implement filtering, escaping, redaction, and bounded diagnostic formatting.
- Register deterministic suites with CTest when testing is enabled; retain the
  existing test option, broaden it to all tests, and share sanitizer settings.

**Complete when:** plugin and deterministic tests build against shared code;
typed boundaries and test seams are usable; native TRACE/DEBUG filtering and
correlated safe diagnostics have focused tests. Existing coverage is retained
or deliberately replaced with contract-correct expectations.

### Increment 2 - Source parsing and candidates

- Keep raw extraction private to the parser. Detect the supplied page's kind
  from its validated source representation rather than trusting the input URL
  alone. Release metadata comes only from decoded `data-tralbum`.
- Implement section 6 of [AGENTS.md](AGENTS.md): known same-owner aliases,
  identity agreement, stable-ID row matching, explicit track-owned date/artwork,
  fallback context, and declared parent identity/location.
- Preserve every declared album member, including malformed and streamless
  candidates, with the original membership/count and ordering evidence.
- Preserve positive `qint64` identities without lossy floating-point conversion.
  Reject unsupported types, non-finite/fractional numbers, and overflow.
- Retain useful decoding helpers, but replace field truncation with explicit
  bounded failures. Define and test input/depth/field/record limits.
- Emit each decoded release JSON snapshot before mapping and source transition,
  conflict, fallback, and unresolved-field events at their agreed levels.

**Complete when:** deterministic extraction tests cover the mapping and ownership
rules, wrong shapes and numeric/resource boundaries yield explicit outcomes,
and neither raw source objects nor warning bags leave the parser.

### Increment 3 - Assembly, admission, and semantic pipelines

- Add pure assembly/admission logic and make album/track resolution use it.
  Acquire a declared parent and match the selected child by stable ID before
  applying cross-resource authority. Parent failure is terminal for that scope.
- Enforce required metadata before processors. Whole albums are all-or-nothing;
  a selected track requires valid parent metadata and its matched child, not
  successful unrelated siblings. Retain the containing album in track results.
- Replace the mixed pipeline with album-first and track-second passes.
  Convert child-mutating album rules into track rules using stable sibling
  context; disable sibling-dependent rules when the required valid set is absent.
- Retain narrowly justified artist/title cleanup, optional inheritance, shared
  artist inference, and missing-number repair. Preserve meaningful explicit
  values and gaps. Number repair must reserve valid supplied numbers before
  assigning inferred ones, so it cannot force a later explicit number to change.
- Derive declared totals during assembly. Remove stream filtering and
  missing-title warning processors; admission owns those validity decisions.
  Remove the separate post-parent patch pipeline and all warning-list plumbing.
- Keep processor mutations traceable with old/new values and reasons. A no-op
  emits no processor event; owner-specific date/artwork differences are not
  themselves processor conflicts.

**Complete when:** the production album/track path enforces admission before
refinement; isolated rules and composed passes are deterministic and preserve
invariants. Tests expecting partial-parent success, discarded children, or
successful missing required titles have been replaced, not merely disabled.

### Increment 4 - Operations, shared acquisition, and host safety

- Replace mutable global batch settings/results with identified operations,
  immutable options, per-scope outcomes, progress, cancellation, and incremental
  ordered delivery. Preserve an outcome for every accepted requested scope.
- Retain the network client but enforce response limits during every read,
  including completion, without first allocating an unbounded read. Validate
  redirect targets and use the effective response URL as the parsing base.
- Implement request sharing for release/parent acquisition, including concurrent
  operations. Keep parsed source facts immutable and scope-independent: a
  whole-album failure must not poison a valid selected-track consumer.
- Account separately for network slots, worker slots, queued work, retained
  bytes, and ordered results. Apply backpressure before scheduling more work.
  Give every consumer a finite budget, including queue wait and required
  parent acquisition; an entire discography is not one page-sized item timeout.
- Implement bounded backoff for transient network failures, HTTP 429, and 5xx,
  respecting usable `Retry-After` information and remaining deadlines. Shared
  requests share retries; cancellation/deadlines never create retry loops.
- Make consumer withdrawal independent of other subscribers. Abort physical
  requests only when unused, discard late results, and complete each operation
  once. Contain worker/dependency exceptions at explicit boundaries.

**Complete when:** fake-network/time tests demonstrate ordered bounded delivery,
parent sharing, independent scopes/deadlines, retry exhaustion, cancellation,
late-result handling, and teardown without blocking or stale host access.

### Increment 5 - Discography discovery, ordering, and settings

- Parser listing support returns one bounded ordered batch of typed album/track
  references plus opaque continuation information. Interpret structured listing
  identifiers/links only, including artist and label variants.
- Resolver submits these references to ordinary release resolution and requests
  continuation batches only when capacity permits. Detect repeated continuations
  and incomplete discovery explicitly; never silently stop at the first page,
  skip malformed entries, or impose a hidden successful item cap.
- Preserve listing ordinals across pages. Unsorted mode releases complete scopes
  incrementally in that order; completion timing cannot reorder them.
- Chronological modes must discover and resolve the full collection before its
  ordered publication. Use a bounded disk-backed staging/order buffer, such as
  a temporary SQLite store using existing Qt SQL support, not an in-memory list
  of every resolved release. Store typed results/keys, not raw HTML/JSON.
- Apply date direction only to known dates; unknown dates always sort last,
  with listing ordinal as the stable tie-breaker. Successful releases can still
  publish when unrelated release resolutions fail. Discovery or staging failure
  is explicit and cannot claim complete ordered collection success.
- Bound disk usage and database/cache memory, handle storage errors, and clean
  temporary state on success, failure, cancellation, and shutdown. Waiting for
  complete chronological order is intentional, not stalled progress.
- Add persisted **Default / Unsorted / Ascending / Descending** options to the
  settings dialog, preserving Default as a setting distinct from its current
  Unsorted interpretation. Add the pre-playback refresh-window setting with a
  60-minute default, finite nonnegative bounds, and checked unit conversions.
  Zero means expired-only refresh. Preserve timeout/parallelism settings.

**Complete when:** paginated synthetic listings resolve without missing entries
or order instability; both date directions, unknowns, ties, backpressure, and
temporary-storage failures are tested. Settings survive restart and affect only
new operations. Both artist and label discovery work through the production path.

### Increment 6 - Live integration tests and probe retirement

- Replace the standalone probe with ordinary QtTest integration tests linked
  to the production resolution target. Carry forward representative URLs for
  artist/label listings, album, member track, and standalone track.
- Assert timely successful completion and meaningful nonempty results of the
  requested scope. Do not duplicate parsing/validation or snapshot mutable
  metadata. Use ordinary QtTest/CTest assertions and failure reporting.
- Register live tests in the default CTest run, including CI, with a separate
  live label for explicit offline runs. Bound request concurrency and give the
  suite a finite watchdog. External failures fail visibly rather than skip.
- Add a CI build/test job that builds the plugin and test targets and runs the
  default CTest suite without excluding live coverage.
- Remove the probe implementation, standalone build, snapshot checks, command
  line options, and README instructions only after the replacement and production
  diagnostics work. Keep deterministic mutation testing and sanitizer support.

**Complete when:** normal test execution includes all supported live resource
types, offline selection remains available, and there is one production source
interpretation path with no test-side extraction or metadata snapshots.

### Increment 7 - Fooyin identity, metadata, and persistence

- Add one strict URI serializer/parser for `bandcamp://track/<trackId>` and
  reuse it at construction, lookup, refresh, and decoder boundaries.
- Project admitted metadata into Fooyin tracks: title/artist, parent album
  title/artist when present, release year, supplied/reconciled number and total,
  duration, and artwork. Leave unknown optional values absent; do not fabricate
  album metadata for standalone tracks.
- Store provider, canonical source, signed stream, and expiry in the agreed
  `ExtraProperties`. Parse expiry safely from transport data, outside descriptive
  metadata. Any retained ID mirror must agree with the URI.
- Use the host cover repository's `COVERART` path for artwork locators. Mark
  metadata read and initialize host identity/hash through supported APIs.
- Put URI-based resident lookup, reuse, and persistence in a library gateway.
  Resolve a bounded incoming batch against host tracks rather than scanning the
  entire library separately for each track. Do not use private database helpers
  or invent a nonexistent path-lookup API.
- Reuse the latest resident track at commit time, preserving edited tags and
  database ID; merge only validated transport changes. Handle overlapping imports
  and delayed writes without overwriting newer edits with stale snapshots.
  Observe asynchronous host write outcomes and report persistence failures.

**Complete when:** album and track inputs produce identical stable identities;
tag/artwork mapping and URI rejection have deterministic tests; re-add preserves
edits and database identity while allowing repeated playlist occurrences.

### Increment 8 - Playlist import controller and operational UI

- Keep action registration in the plugin and move per-import orchestration into
  a controller. Widgets consume progress and structured outcomes; they do not
  schedule requests, reconcile data, or inspect diagnostic warning bags.
- Add "Create as new playlist" and capture the UI-selected destination and
  options at submission. Validate the destination again before each commit.
- Lazily create a persistent destination with the agreed automatic name.
  If no results succeed, create no playlist and report the outcome.
- Commit successful scopes on the main thread in submission/collection order,
  keeping each admitted whole album as one ordered block. Failed scopes advance
  ordering without blocking unrelated results.
- Preserve user-requested repeated entries. Shared acquisition and stable
  database identity are optimizations, not playlist deduplication policies.
- Replace the diagnostic results-only flow with bounded progress and per-input
  failure presentation. Allow cancellation; retain committed tracks and withdraw
  unused work. If the fixed destination is removed or becomes unwritable, stop
  further commits, report the reason, and release the run's remaining work.

**Complete when:** selected/current versus playback playlist, new/temporary/
locked/automatic/deleted targets, empty outcomes, parallel ordering, repeated
entries, cancellation, and restart persistence are verified through controller
tests and focused Fooyin smoke checks.

### Increment 9 - Shared stream refresh and manual actions

- Build the stream resolver on the existing broker/parser and stable-ID
  selection logic. Refresh canonical album sources for members and track sources
  for standalone tracks; do not add a second raw parser or per-member fetch path.
- One album request distributes available refreshed transport state by track ID
  to its consumers. Missing selected children fail their own refresh scope;
  unrelated bad siblings do not discard usable selected streams.
- Validate source/URI/ID consistency and signed HTTPS locators. Preserve query
  parameters for transport and parse expiry without overflow. Unknown/untrusted
  expiry requires establishing fresh usable state, not assuming infinite validity.
- Implement manual expired-only and forced modes over selected Bandcamp tracks,
  falling back to the UI-selected playlist when selection is empty. Ignore
  non-Bandcamp tracks as outside the requested action's scope.
- Persist only provider/source/stream/expiry transport properties on the latest
  resident identities. Report refresh and write outcomes separately; never
  overwrite user tags, artwork edits, statistics, or filepath.
- Share concurrent refresh requests and retain only bounded freshness state;
  any cached state must be associated with its validated release and track IDs.

**Complete when:** one album acquisition refreshes several selected members,
standalone refresh works, malformed or mismatched state fails explicitly, and
manual refresh persists transport changes without changing edited metadata.

### Increment 10 - Playback and bounded reopen recovery

- Register a Bandcamp-scheme decoder using supported Fooyin interfaces.
  Inject a lifetime-safe refresh bridge and inner-decoder access; avoid global
  remote-provider replacement, raw plugin captures, and loader ownership cycles.
- Check freshness once when preparing a playback attempt: refresh if expired,
  or if remaining lifetime is strictly less than the configured window.
  Exactly the threshold is not expiring. Reuse sufficiently valid transport.
  There is no periodic timer and no unconditional refresh on every playback.
- If expiry is unknown/untrusted, obtain usable fresh state or fail explicitly.
  A refresh returning a still-valid stream inside the window does not trigger
  a loop in the same attempt; an already expired refreshed stream cannot play.
- Open a bounded remote device and delegate MP3 decoding to the existing host
  decoder. Preserve device/inner-decoder ownership, timeout and cancellation
  propagation, playback hints, starvation versus EOF, and truthful seekability.
  Do not download entire audio files or implement a codec.
- On the first stream-open failure, allow one bounded delayed reopen of the
  same URL. Do not refresh again, parse HTTP codes from error strings, retry
  malformed identity/backend errors, or retry after cancellation/deadline expiry.
- Keep network/refresh waits off the GUI thread and avoid a worker-to-GUI wait
  cycle during stop/shutdown. Queue transport persistence to the host thread.
  Keep signed URLs out of plugin and delegated diagnostic paths.

**Complete when:** deterministic tests verify freshness boundaries, zero/default
window, unknown expiry, one refresh at most, one same-URL reopen at most,
cancellation, decoder starvation, and terminal failures. Fooyin plays an imported
track to completion, including after restart with an expired stored stream.

### Increment 11 - End-to-end acceptance and documentation

- Exercise discography/album/track import into persistent playlists, artwork,
  normal properties editing, restart, re-add, manual refresh, and playback.
  Verify user-edited metadata survives each path.
- Run deterministic semantic/robustness suites, default live CTest coverage,
  and targeted ASan/UBSan runs, including increased seeded mutation iterations.
  Verify bounded memory, queues, reorder windows, and disk staging on a large
  synthetic collection rather than relying only on a small live discography.
- Verify diagnostic correlation across source snapshots, reconciliation,
  processor transitions, shared requests, and failures, including redaction.
- Update [README.md](README.md) with actual features, all settings, playlist
  rules, conditional refresh/reopen behaviour, playback/seek limitations, native
  TRACE capture, and build/test/offline/sanitizer commands. Remove temporary
  adapters and debug actions; do not leave parallel legacy paths.

**Complete when:** the requested feature chain works through the plugin, the
contract's three test families are exercised, documentation matches behaviour,
and no temporary migration code or obsolete diagnostic tool remains.
