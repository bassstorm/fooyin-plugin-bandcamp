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
  work deduplication must not erase requested playlist occurrences.
- Append to the playlist selected in the UI, not the currently playing
  playlist. Capture its ID at submission. A "Create as new playlist" option
  overrides this; missing, temporary, locked, or automatic targets require a
  new persistent playlist.
- Create a new playlist only when a successful result is ready to commit.
  One album or album-member track uses the album title; one standalone track
  uses its title; a discography or mixed submission uses "Bandcamp".
  There is no manual naming field.
- Retain progress, per-input failure reporting, and user cancellation.
  Page fetches are not automatically retried. Already committed results remain
  when a run is cancelled or its target becomes unavailable.
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
| Scheduling and lifecycle | [bandcampservice.cpp](src/bandcampservice.cpp) queues all inputs and retains all results until a batch ends. Timed-out parse futures can outlive active slots; destruction waits for them. Separate input association/ordering from bounded release execution and use cooperative, nonblocking shutdown. Add operation cancellation and a simple operation-local domain cache, not a shared-request broker. |
| Fetching | [bandcampclient.cpp](src/bandcampclient.cpp) has useful byte/redirect controls, but appends reads before checking limits, lacks a final-read size check, and returns the requested rather than effective response URL. Keep the transport wrapper and correct these boundaries without adding retries or cross-call sharing. |
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

### High-level flow

Use two operational roles: an **orchestrator** organizes a caller's work;
a **resolver** turns a list of release requests into plugin domain entities.
These are responsibility boundaries, not a requirement for a class or file
for every role or helper. Keep value transformations independent of Fooyin.

```text
URLs or Fooyin tracks
    |
    v
Orchestrator: unfold discographies, build requests, deduplicate work
    |                         |
    |                         +--> original occurrences, selections, order
    v
Unique release requests
    |
    v
Resolver: HTTP -> parser -> assembly/admission -> post-processors
    |
    v
Keyed per-request outcomes
    |
    v
Orchestrator: associate results, restore occurrences, apply ordering
    |
    v
Caller: playlist insertion or stream-property updates
```

### Ownership

| Owner | Responsibility and boundary |
| --- | --- |
| Orchestrator (working name) | Unfold discographies into release lists through resolver discovery calls, construct requests from URLs or stored Fooyin release URLs/track IDs, deduplicate work, retain original selections/occurrences, and associate and order outcomes. Does not fetch or interpret source data, reconcile metadata, or manage workers. |
| Resolver | Accept a complete release-request list and process it through its internal pipeline with configured parallelism. Own per-item progression, immutable options, processing deadlines, cancellation, and an operation-local domain cache. Return keyed outcomes, not caller-specific ordering or playlist changes. |
| Pipeline helpers | Encapsulate HTTP exchange, source parsing, authoritative assembly/admission, and semantic refinement separately. Each owns its mechanics and exposes typed values/outcomes; no helper reaches into another helper's mutable state. |
| Caller and host integration | Capture UI intent and destination, consume the orchestrated model, and apply playlist or transport changes through the track factory/library gateway. Preserve resident identities and edits. Widgets and playback code do not duplicate resolution logic. |

The HTTP client returns bounded response data and effective response information.
Only the parser interprets raw HTML/JSON and returns typed candidates.
Assembly/admission combines candidates and validates the requested scope before
post-processing. Processors run album-first, then track-by-track, without
rejecting resources or changing transport identity.

Discography unfolding asks the resolution layer for one bounded discovery
batch at a time: typed release references plus continuation information, not a
discography domain entity. The orchestrator follows continuations to construct
the complete release list, then submits deduplicated requests to the resolver.
Discovery acquisition and source interpretation use the same HTTP/parser
infrastructure, not a second implementation. No additional input
"normalization" stage is implied.

### State and data boundaries

- Keep the occurrence/order map separate from the unique work queue.
  Deduplicating work must not remove requested playlist occurrences.
- A release request contains its URL and the minimum selection information
  needed to distinguish a whole release from selected track IDs. Original UI
  inputs and final ordering stay outside the resolver.
- The resolver acquires any required parent as part of the same item workflow,
  then assembles, admits, and refines the result. Parent acquisition is not a
  new top-level item requiring another item-admission slot.
- Cache successful, admitted and post-processed domain entities above the full
  pipeline, for this submitted list only. Do not cache pages, JSON, parser
  candidates, or failures. Reuse must preserve source authority and selection
  scope; selected-track validity does not imply whole-album validity.
- Playlist import and stream refresh consume the same plugin model. Refresh
  selects the required tracks and updates transport fields only; it does not
  introduce a parallel parser, model-building pipeline, or warning mechanism.
- Snapshot options and preserve an outcome for every accepted request.
  Expected failures are typed values; cancellation is distinct. A failed item
  does not stop unrelated work or block final ordering.
- Pending requests, associations, cached domain entities, and ordered results
  are ordinary lightweight operation-owned collections. Bound active heavy work,
  not the submitted list: dispose of response bodies after parsing and candidates
  after assembly. The orchestrator owns associations/order; the resolver owns
  execution capacity and releases its cache when the list finishes.
- Start an item's processing deadline when its workflow starts, including any
  required parent acquisition. A long pending list must not cause untouched
  items to expire; any caller-wide deadline is a separate policy.
- Keep physical HTTP/worker capacity until actual completion, even after
  logical cancellation. Workers own their inputs; callbacks are guarded and
  teardown never waits on the GUI thread.
- One HTTP fetch produces one outcome: no automatic retry, backoff, retry
  configuration, or cross-call coalescing. The already-defined same-URL playback
  reopen is a separate behavior, not a page-fetch retry.
- Keep execution in the resolver and physical reply/worker lifetimes with their
  owners. Do not introduce a generic stage scheduler, shared-acquisition broker,
  or per-entity memory reservation framework.

### Test boundaries

Test request construction/deduplication, association, and ordering as value transformations.
Test parser, assembly/admission, and processor semantics independently. Cover
HTTP, cache scope, deadline, cancellation, and lifetime robustness at the component
that owns each behavior, using narrow dependencies where necessary. A small
number of happy-path integration tests exercise the composed flows; they are
not a sophisticated simulator for every combination of internal states.

## 4. Delivery sequence

| Increment | Depends on | Deliverable |
| --- | --- | --- |
| 1 | None | Shared build/test seams, typed boundaries, diagnostic infrastructure. |
| 2 | 1 | Contract-compliant source candidates and bounded extraction. |
| 3 | 2 | Authoritative assembly, admission, and non-failing processors. |
| 4 | 3 | List-processing release resolver, orchestration boundary, domain cache, and cancellation. |
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

### Increment 4 - Resolver pipeline, orchestration boundary, and host safety

- Implement the ownership boundary in section 3. Keep caller input organization
  and final association/order in the orchestrator; the resolver accepts complete
  release lists and returns keyed per-scope outcomes from its internal
  pipeline. Replace mutable global settings/results with immutable options,
  progress, cancellation, and an outcome for every accepted requested scope.
- Retain the network client but enforce response limits during every read,
  including completion, without first allocating an unbounded read. Validate
  redirect targets and use the effective response URL as the parsing base.
- Implement a simple operation-local cache of successful, fully processed
  domain entities. Cache lookup precedes the full pipeline; compatible success
  populates it. Do not retain raw responses/parser candidates, cache failures,
  share across calls, or confuse whole-album and selected-track validity.
- Bound active release workflows and physical HTTP/worker capacity using the
  existing source limits and configured parallelism. Pending requests remain an
  ordinary list; do not add admission windows or reservation ledgers.
  Each active item has a finite processing budget including required parent
  acquisition, starting when its workflow begins rather than at list submission.
- Return failed fetches directly, including transient transport failures, 429,
  and 5xx. Do not add automatic retries, backoff, or retry configuration.
- Cancel unfinished work within its owning operation, discard late results,
  and complete each operation once. Keep physical capacity occupied until work
  actually exits. Contain worker/dependency exceptions at explicit boundaries.

**Complete when:** the resolver pipeline and orchestration boundary have focused
component tests for their own semantics and robustness, including operation-local
domain reuse, independent scopes/deadlines, bounded active work, long input lists,
one-outcome fetches, cancellation, late completion, and safe teardown. Association/ordering tests do not require
HTTP or worker simulation; happy-path composition uses the production pipeline.

### Increment 5 - Discography discovery, ordering, and settings

- Parser listing support returns one bounded ordered batch of typed album/track
  references plus opaque continuation information. Interpret structured listing
  identifiers/links only, including artist and label variants.
- Include structured entries embedded in the listing, such as the music grid's
  `data-client-items` collection, not just rendered links. Combine rendered and
  embedded entries in source-defined order and reconcile repeated representations
  of the same listing entry without losing explicit input occurrences. Read only
  discovery identity, kind, and location; embedded display metadata is not
  authoritative release metadata.
- Treat an encoded listing collection as a source container, not one metadata
  string subject to a leaf-field length limit. Apply response, JSON depth/record,
  and individual field limits at their respective boundaries. Per-page source
  limits do not impose an aggregate release-count cap across the discography.
- The orchestrator unfolds these discovery batches through resolver calls and
  submits the complete deduplicated release list to resolution. Detect
  repeated continuations and incomplete discovery explicitly; never silently
  stop at the first page, skip malformed entries, or impose a hidden successful
  item cap.
- Preserve listing ordinals across pages. Unsorted mode releases complete scopes
  incrementally in that order; completion timing cannot reorder them.
- Chronological modes must discover and resolve the full collection before its
  ordered publication. Retain domain results and ordering keys in the
  orchestrator, not raw HTML/JSON; do not add a disk-backed staging subsystem.
- Apply date direction only to known dates; unknown dates always sort last,
  with listing ordinal as the stable tie-breaker. Successful releases can still
  publish when unrelated release resolutions fail. Discovery failure
  is explicit and cannot claim complete ordered collection success.
- Release operation-owned collections on completion/cancellation. Waiting for
  complete chronological order is intentional, not stalled progress.
- Add persisted **Default / Unsorted / Ascending / Descending** options to the
  settings dialog, preserving Default as a setting distinct from its current
  Unsorted interpretation. Add the pre-playback refresh-window setting with a
  60-minute default, finite nonnegative bounds, and checked unit conversions.
  Zero means expired-only refresh. Preserve timeout/parallelism settings.

**Complete when:** paginated synthetic listings resolve without missing entries
or order instability; both date directions, unknowns, ties, discovery failures,
and operation cleanup are tested. Settings survive restart and affect only
new operations. Both artist and label discovery work through the production path.
Large embedded listings are fully extracted even when the encoded collection
exceeds an individual field's length limit, and large-list ordering avoids
repeated full-list scans. Use deterministic catalogues for volume coverage rather
than issuing hundreds of live release requests during routine validation.

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

- Build the stream resolver on the existing release pipeline and stable-ID
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
- Group same-release tracks within one refresh submission and associate cached
  domain entities with validated release/track identities and selection scope.
  Separate submissions have independent acquisition, caches, and cancellation.

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
  Verify bounded heavy working data and full processing of a large lightweight
  input list rather than relying only on a small live discography.
- Verify diagnostic correlation across source snapshots, reconciliation,
  processor transitions, domain-cache reuse, and failures, including redaction.
- Update [README.md](README.md) with actual features, all settings, playlist
  rules, conditional refresh/reopen behaviour, playback/seek limitations, native
  TRACE capture, and build/test/offline/sanitizer commands. Remove temporary
  adapters and debug actions; do not leave parallel legacy paths.

**Complete when:** the requested feature chain works through the plugin, the
contract's three test families are exercised, documentation matches behaviour,
and no temporary migration code or obsolete diagnostic tool remains.
