# AGENTS.md

This repository implements a Bandcamp integration plugin for Fooyin. This
document defines the domain concepts, source-data interpretation, metadata
authority, and architectural boundaries that govern its implementation,
together with repository conventions for testing and logging.

## 1. Terminology

### Representations and context

| Term | Definition |
| --- | --- |
| Bandcamp source model | Bandcamp-specific page representations: release-page `data-tralbum` JSON and discography listing structure. Only the parser interprets these representations. |
| Plugin domain model | Validated album and track metadata consumed by Fooyin, represented by `BandcampAlbum` and `BandcampTrack` in [bandcampmodel.h](src/bandcampmodel.h). |
| Parse candidate | Typed source-derived values and relationship evidence awaiting assembly and required-field validation; not yet a valid domain object or a copy of the raw source tree. |
| Resolution context | Typed candidates, relationship evidence, provenance, request state, and diagnostics used to produce domain objects; separate from final metadata. |
| Resolution outcome | A tagged success, failure, or operational cancellation for the requested scope. Success contains valid domain data; failure contains a structured error, not a partial success. Cancellation follows the operation lifecycle defined under host safety. |

### Metadata operations

| Term | Definition |
| --- | --- |
| Reconciliation | Selecting authoritative facts about the same entity from multiple representations or resources. |
| Inheritance | Filling a missing domain value from its parent or other defined context. |
| Normalization | Refining or cleaning metadata according to a defined semantic rule, rather than merely copying a source field. |

## 2. Bandcamp source model

### Source characteristics

The Bandcamp source model is a web-page representation, not a guaranteed API
contract. Metadata is highly inconsistent in the wild: values can be missing,
contradictory, inherited, or embedded in poorly structured titles/credits.
Reconciliation, refinement, and cleanup are therefore core design requirements,
not optional enhancements or exceptional parser workarounds.

Root fields mix page identity, effective display values, playback data, and
related album context. The root is not universally the parent album. An album
page supplies album context, membership, and sibling streams, but is not a
guaranteed superset of its track pages' metadata.

### Release-page structure

In JSONPath notation below, `$` denotes the decoded `data-tralbum` object, not
the HTML document.

| JSONPath | Meaning |
| --- | --- |
| `$` | Release-page data with supporting context, not a uniform album tree. |
| `$.current` | The entity being viewed: an album on an album page, a track on a track page. |
| `$.current.type`, `$.current.id` | The viewed entity's kind and stable identity. |
| `$.artist`, `$.art_id` | Effective page-level values; their presence alone does not establish explicit track ownership. |
| `$.trackinfo[*]` | Track records: listed children on an album page, selected-track data on a track page. |
| `$.current.album_id`, `$.album_url` | Parent album identity and location on a track page; the location can be relative. |
| `$.album_release_date` | Album-scoped date, including on a track page; not an alias for that track's own release date. |

### Discography listings

A discography is an artist's or label's ordered music listing. It can contain
album links and track links, including standalone tracks and tracks with album
parents. A listing can be paginated or expose continuation data.

Its `data-tralbum` object can contain only a URL, without release metadata or
child records. Listing identifiers and links provide discovery candidates, not
authoritative album or track metadata.

## 3. Plugin domain model

### Vocabulary and structure

| Concept | Definition and relationships |
| --- | --- |
| Album | A release containing tracks. A one-track album is still an album. |
| Track | A recording that belongs to an album or is standalone. |
| Release | An album or a standalone track: the highest containing resource for metadata authority and stream refresh. |

### Identity and relationship invariants

- Album membership is established by explicit relationships and stable IDs,
  not matching titles, URL slugs, track counts, artwork, or track numbers.
- Unknown or malformed membership is distinct from standalone status.
  Explicit no-parent values establish standalone status; a missing or invalid
  parent location does not negate a known album ID.
- Listed membership is independent of streamability. An unavailable stream
  does not erase a declared parent-child relationship.

### Metadata invariants

- Album facts and track facts have different owners. Track artist, artwork,
  and date can differ from the corresponding album values without conflict.
  The hosting account is not necessarily the recording artist or album artist.
- `trackTotal` is the size of the declared track set, not the number selected
  or successfully resolved, or an assumed count of undisclosed tracks. Supplied
  track numbers and gaps are independent of the requested selection.
- Each concept has one resolved field. Track title and album title, or track
  artist and album artist, are separate concepts. A selected track retains the
  containing-album metadata needed by Fooyin.
- Standalone tracks have no fabricated album, album total, or track number.

### Required and optional values

The domain boundary admits only objects satisfying their required fields.
Optional does not mean unchecked: a supplied value must have a valid type and
range, or remain explicitly unknown. Missing and invalid are distinguished in
resolution context, not represented by invented metadata.

| Entity | Required before post-processing | Optional metadata |
| --- | --- | --- |
| Album metadata | Positive stable album ID, canonical album `sourceUrl`, and nonblank title. | Artist, release date, artwork. |
| Track | Positive stable track ID, canonical release `sourceUrl`, nonblank title, and valid supported HTTPS `streamUrl`. | Artist, album artist, duration, release date, artwork, track number, and total. |

Album membership adds required context: validated parent metadata and a matching
selected child. Stream presence is a domain requirement, not a post-processing
filter. A syntactically valid stream locator does not guarantee successful
network playback; playback failures remain separate operational outcomes.

Album metadata validity is independent of resolving every member. Whole-album
and selected-track requirements are defined by the resolution scope below.

### Representation constraints

- Include only metadata consumed by Fooyin plus stable identity and playback
  locators needed to use it (`id`, `sourceUrl`, stream and artwork locators).
- Exclude Bandcamp source structures, field provenance, request/effective URLs,
  scheduling, cache state, and diagnostics from final domain metadata.
- There is no discography domain entity.
- The term release does not require another entity struct. Do not duplicate
  domain fields to mirror source alternatives or build a parallel raw-model
  hierarchy for fallback.

## 4. Fooyin track identity

### Canonical URI

Every Fooyin Bandcamp track uses this virtual URI as its persistent `filepath`,
whether it belongs to an album or is standalone:

```text
bandcamp://track/<trackId>
```

| Component | Contract |
| --- | --- |
| Scheme | Literal lowercase `bandcamp`; routes playback through the plugin rather than Fooyin's generic HTTP handling. |
| Authority | Literal lowercase `track`, not an artist, album, or network hostname. |
| Path | One segment containing the positive Bandcamp track ID as decimal digits, with no sign or leading zeros; the value must fit `qint64`. |
| Other components | No credentials, port, additional path segments, trailing slash, query, or fragment. |

Reject malformed or unsupported URI shapes and missing, zero, negative, or
overflowing IDs. Do not substitute a title, track number, encoding ID, Fooyin
database ID, or hash when the Bandcamp track ID is unavailable.

### Identity invariants

- The same Bandcamp track has the same URI when imported from its album, track
  page, or a discovered release candidate. Album membership is not encoded in
  the URI and does not create a second identity.
- Titles, artists, numbering, artwork, release URLs, and signed streams are
  metadata or locators, not URI components. Their changes do not change the
  persistent identity.
- The URI serializes the plugin domain track's `id` at the Fooyin construction
  boundary. It is not another independently editable domain field.
- For a persisted Fooyin track, the URI is the authoritative serialized track
  ID. Any `_BANDCAMP_TRACK_ID` mirror must match it; disagreement is an error,
  not permission to refresh or play a different track.
- Re-adding the same URI reuses the existing identity and preserves user-edited
  metadata. Refreshing streams never changes `filepath` or user-edited tags.

### Persistent locators and playback state

Keep mutable transport state in Fooyin `ExtraProperties`, separate from the
URI and user-editable metadata:

| Property | Meaning |
| --- | --- |
| `_STREAM_PROVIDER` | Literal `bandcamp`, identifying the playback provider. |
| `_BANDCAMP_SOURCE_URL` | The domain `sourceUrl`: the canonical containing release used for refresh. |
| `_BANDCAMP_STREAM_URL` | The current signed audio locator, not track identity. |
| `_BANDCAMP_EXPIRES_AT` | The expiry associated with the current signed stream. |

The virtual URI is not a network endpoint and does not reconstruct a page URL.
Playback and refresh require the persisted source locator, resolve by the URI's
track ID, and report missing or inconsistent state as failure. Source redirects
or membership changes may update the validated refresh locator without changing
the track URI. Stream refresh updates transport state, not descriptive metadata.

## 5. Resolution and failure policy

### Selection scope and canonical source

- `sourceUrl` means the canonical URL of the **highest containing release**:
  album URL for an album and its member tracks; own track URL for a standalone
  track. Never promote it to an artist, label, or discography URL.
- Separate the user's selection from that release source. Selecting a track
  still resolves only that track for the caller, even when its album is fetched.
  An album selection resolves its listed tracks; a collection input resolves
  the release references extracted by the parser.
- Use validated JSON page identity and effective redirected URLs to establish
  canonical locations. Resolve relative links, check resource kinds, and verify
  a fetched parent against the declared album ID when available.

### Fetch direction

Resolution goes **up** for authority and context, never down for enrichment.

| Input | Fetch policy |
| --- | --- |
| Album | Fetch the album and use its child records. Never fetch member track pages to fill gaps. |
| Track with a parent | Fetch the track page, then its album; reconcile the selected track by stable ID. |
| Standalone track | Fetch the track page; no parent request is required. |
| Discography URL | Fetch listing pages for the parser, then resolve its extracted release candidates. Collection expansion does not permit album-member page fetches. |

Direct track input can retain explicit metadata unavailable through album input.
This is an accepted consequence of not fetching downward.

### Domain admission boundary

Resolution proceeds through these distinct stages:

1. Fetch and parse pages into typed candidates. Record source extraction,
   fallbacks, conflicts, and unresolved fields without claiming domain validity.
2. Acquire required parents, verify identities, and assemble authoritative
   facts from the available candidates according to the mapping contract.
3. Validate required fields and relationships for the requested scope.
   Failure ends resolution before semantic post-processing.
4. Refine admitted domain objects: album processors first, then track
   processors with their validated parent context when present.
5. Publish the requested result or construct Fooyin tracks. Post-processors do
   not reject resources, remove invalid tracks, or decide resolution success.

An unresolved candidate field is not automatically a terminal failure: another
source may supply it during assembly. After admission, only optional gaps remain
for processors to fill; required values cannot depend on a later processor.

### Scope and atomicity

| Requested scope | Success requirement |
| --- | --- |
| Whole album | Valid album metadata and every declared child meeting the track contract. One unresolved child fails the album; never return or publish its successful subset as the album result. |
| One album track | Valid parent metadata and a valid selected child matched by stable ID. Unrelated unresolved siblings do not fail this selection. |
| Standalone track | A valid track and established no-parent status. |
| Collection or multiple inputs | An outcome for each requested release/track scope. One failed item does not stop unrelated items. |

An empty whole-album selection is reported explicitly as no resolvable tracks,
not as successful album resolution. Preserve declared membership and totals
even when some candidates cannot become valid domain tracks.

### Relationship validation

- Match track IDs, not position, title, duration, or signed stream URL.
  A missing matching child or contradictory parent identity is an unresolved
  relationship, not permission to substitute another track.
- A known parent that cannot be fetched, parsed, or reconciled means failed
  resolution. Do not return a successful partially enriched track or silently
  reclassify it as standalone. Partial data may be retained as failure context.

### Outcomes and errors

- Expected failures use explicit result values, not exception-driven control
  flow or success values with an accompanying error string.
- A structured error carries a stable code, stage, resource location, track
  identity when known, relevant field/relationship, and a concise safe reason.
  These values let callers determine the outcome independently of log settings.
- Missing optional fields, successful fallback, and resolved discrepancies do
  not accumulate in warning lists. Logs provide their diagnostic history.
- An atomic album failure identifies its failing child/cause; it does not
  require an unbounded diagnostic list. Other field details remain in logs.

### Stream refresh

- Stream URLs are signed and expire. Before playback, refresh an expired stream
  or one whose remaining lifetime is less than the configured refresh window
  (default 60 minutes). Reuse a stream outside that window. The window is a
  remaining-lifetime threshold, not a periodic refresh interval.
- Refresh by `sourceUrl`: one album fetch supplies refreshed streams for its
  member tracks; one standalone-track fetch supplies its own stream.
- Group tracks sharing a release within one refresh submission; distribute
  refreshed streams by stable track ID, not filtered list position.
  Separate submissions are independent. An album-linked track does not require
  another track-page fetch on refresh.
- Keep signed stream locators separate from canonical release identity. Never
  derive metadata or membership from them, and never strip their required query
  parameters when using them for playback.
- This limits requests to Bandcamp and reduces rate-limit pressure; it does
  not guarantee that rate limiting cannot occur.
- A playback preparation attempt refreshes at most once. If opening its stream
  fails, recovery may reopen the same URL once within the attempt's deadline;
  it does not trigger another refresh.

## 6. Source-to-domain mapping and authority

### Page-local extraction contract

This table specifies parser mappings, not the entire source schema. `Album`
and `Track` denote plugin domain entities; JSONPaths refer to the relevant
page's decoded release JSON. Cross-resource policy is defined separately below.

| Output concept | Source JSONPath | Page-local mapping rule |
| --- | --- | --- |
| Viewed entity kind/ID | `$.current.type`, `$.current.id`; root aliases `$.item_type`, `$.id` | Prefer valid explicit current values; validate aliases as the same entity. Keep kind in resolution context. |
| Album title/artist | `$.current.title`, `$.current.artist`; `$.artist` | Prefer explicit album values; root artist is the page-level fallback. |
| Album date/artwork | `$.current.release_date`, `$.current.art_id`; `$.album_release_date`, `$.art_id` | Prefer explicit album values, then validated same-album root fallbacks. |
| Track ID | `$.trackinfo[*].id`, `$.trackinfo[*].track_id`; viewed track `$.current.id` | Validate aliases and match the viewed track to its record; never match by array position. |
| Track title/artist | `$.trackinfo[*].title`, `$.trackinfo[*].artist`; viewed track `$.current.title`, `$.current.artist` | Read each child from its row. For the viewed track, prefer valid explicit current values over its local row equivalents. |
| Track number | `$.trackinfo[*].track_num`; viewed track `$.current.track_number` | Read each child's number; for the viewed track, prefer a valid explicit current number over its local row equivalent. |
| Track duration | `$.trackinfo[*].duration` | Convert valid seconds to domain `durationMs`. |
| Track stream | `$.trackinfo[*].file['mp3-128']` | Map the supported signed stream locator without altering its required query parameters. |
| Track-owned date/artwork | `$.current.release_date`, `$.current.art_id` on a track page | Map explicit track values without substituting album-scoped fields. |
| Page fallback context | `$.artist`, `$.art_id` on a track page | Retain contextual values with fallback provenance, not as explicit track-owned facts. |
| Parent context | `$.current.album_id`, `$.album_url` | Extract declared album identity and location without assuming successful parent resolution. |
| Listed children | Album `$.trackinfo[*]` | Produce typed child candidates and retain declared membership/counts; never silently discard malformed or unavailable children. |

### Extraction constraints

- There is no universal current-then-root fallback. Use only known aliases for
  the same fact about the same entity. Album `$.current.release_date` and
  `$.album_release_date` share an owner; on a track page they do not.
- Do not copy album/root values into child records as if explicitly supplied
  there. Missing child values remain gaps for domain post-processing.
- Only usable values for the same fact can conflict. Equivalent representations
  or legitimate owner-specific differences are not conflicts. An unusable value
  is unresolved input, not a competing authoritative value.
- Source observations are diagnostic events, not resolution outcomes. The
  logging contract defines their levels and detail; the domain gate decides
  whether a remaining gap is fatal.

### Cross-resource reconciliation and inheritance

| Domain facts | Authority and fallback |
| --- | --- |
| Album title, album artist, membership, listed total, album date/artwork | The resolved parent album is authoritative for album context. |
| Matched track title, artist, number, duration | Valid explicit album-child values precede conflicting track-page equivalents. Missing child metadata can be filled from the selected track page. |
| Track-owned date/artwork | Preserve explicit track values; inherit album values only when those values are missing. |
| Track artist versus album artist | Preserve meaningful track artist; album artist is a separate concept, not an unconditional replacement. |
| Album-backed track stream | Use the matched album child's stream. An absent stream cannot be bypassed with an older track-page stream. |
| Track total | Derive it from declared album membership, not successful/selected children, and inherit it for album members. |

Inferred or defaulted values never displace explicit values merely because
their page has higher authority. Reconciliation precedes domain inference;
provenance must remain available wherever ordering alone cannot distinguish
explicit, fallback, and inherited values. Required-field authority is settled
during assembly before domain admission; subsequent semantic refinement stays
within the admitted object's invariants.

### Normalization constraints

Normalization requires a defined, narrow semantic rule. A separator in a title
alone is not proof of a different artist. Cleanup must preserve meaningful
titles, credits, and legitimate differences between album and track metadata.

## 7. Architecture and responsibilities

### Layer responsibilities

| Layer | Owns | Does not own |
| --- | --- | --- |
| Parser | Source extraction/validation, page-local candidate mapping, source diagnostic events, and typed relationship/provenance output. | Network requests, final scope validity, or domain inference. |
| Orchestration | Discography unfolding into release references, request construction/deduplication, original-input associations, and final result ordering. | Source interpretation, metadata reconciliation, or worker scheduling. |
| Release resolver | Complete release-list processing, execution parallelism, parent acquisition, authoritative assembly, scope validation, post-processing, outcomes, and operation-local domain caching. | Original-input/playlist ordering or interpretation of Bandcamp JSON or discography layout. |
| HTTP client | One bounded HTTP fetch, redirects, effective response location, and physical reply lifetime. | Automatic retries, metadata interpretation, or domain caching. |
| Album processors | Semantic refinement of admitted album-owned metadata. | Track mutation, resource rejection, raw source access, or network requests. |
| Track processors | Semantic refinement of one admitted track with validated parent context when present. | Album mutation, resource rejection, raw source access, or network requests. |
| Fooyin track construction | Serialization of domain identity into the virtual URI and projection of resolved metadata/locators into Fooyin tracks. | Inventing identity, source interpretation, or network resolution. |

### Parser boundary

See [bandcampparser.cpp](src/bandcampparser.cpp) and
[bandcampparser.h](src/bandcampparser.h).

- Only the parser interprets the Bandcamp source model. Release metadata comes
  exclusively from `data-tralbum` JSON, never visible HTML, Open Graph tags,
  JSON-LD, or discography display text.
- Discography structure remains private to the parser. Artist and label
  listings use the same extraction contract: an ordered sequence of typed
  release candidates plus continuation locators when present. No discography
  source object is passed to the resolver or post-processors.
- Discovery reads structured music-listing identifiers and links only, not
  arbitrary sidebar, recommendation, or merchandise links. Both album and track
  candidates are retained; track membership is resolved through normal release
  resolution.
- Record each decoded release JSON snapshot before field mapping, including
  snapshots later rejected for invalid shape. Field fallback/conflict events
  retain concrete JSONPaths and source values here, not in post-processors.

### Resolution boundary

See [bandcampservice.cpp](src/bandcampservice.cpp).

- The orchestrator unfolds discographies through discovery calls to the
  resolution layer without understanding the source listing. Continuations
  are exhausted before treating a collection's release list as complete.
- The resolver accepts a complete list of release requests and processes it
  with bounded internal parallelism. Callers do not manage admission batches.
  Each request includes the selection context needed for its validity scope;
  original occurrences and final ordering remain with the orchestrator.
- HTTP acquisition, parsing, assembly/admission, and post-processing have
  encapsulated responsibilities. No central controller manipulates helpers'
  internal request maps, worker state, or resource counters.
- Assembly works on typed candidates and semantic provenance; it never reads
  source JSON. It selects authoritative fields and validates the requested
  scope before constructing admitted domain objects.
- A selected track's parent context contains valid album metadata and declared
  membership/counts, not a claim that every sibling resolved. Failed siblings
  remain outside the domain track set.
- Timeout and error handling preserve the required-parent failure policy and
  distinguish failure from successful resolution.

### Resolution cache

- Cache only successfully admitted and post-processed plugin domain entities,
  above the complete resolution pipeline. Do not cache HTML, JSON, parser
  candidates, or failed outcomes.
- The cache belongs to one submitted release list and is released when that
  operation ends. There is no cross-call cache or cross-call coalescing.
- Reuse requires compatible resource identity, source authority, and requested
  scope. A selected-track result does not establish whole-album validity, and
  cached album-derived metadata must not erase explicit track-page metadata.
- Cache state remains operational context, never part of a domain entity.

### Processor contracts and order

See [bandcamppostprocessor.h](src/bandcamppostprocessor.h) and
[bandcampprocessors.cpp](src/bandcampprocessors.cpp).

- Album and track processors have separate contracts and centrally composed,
  explicitly ordered pipelines. Each processor implements one semantic rule.
- Album processors run first and change only album-owned metadata. They may
  inspect validated member data required by their rule, not incomplete parse
  candidates or raw fields.
- Track processors then receive one track plus validated album context when
  present. Standalone processing has no fabricated parent.
- Parent/sibling inputs are stable for a pass. Results must not depend on
  whether an earlier sibling has already been modified. A rule requiring
  unavailable sibling data is not invoked; it does not reject the selection.
- Processors have no resolution-failure result and cannot remove tracks for
  missing streams. They preserve required-field validity and do not change
  identity or transport locators. A normalization rule whose preconditions are
  unmet leaves values unchanged.
- Required-field damage or an unexpected processor exception is an
  implementation defect handled by the host-safety boundary, not normal
  metadata reconciliation or a successful fallback.
- Every effective change follows the authority/normalization policy and emits
  the transition event defined below. A no-op emits no processor event.

### Context constraints

- Parse results expose candidates and typed context, not invalid objects
  masquerading as resolved domain models or a shadow Bandcamp source tree.
  Raw source representations remain within the parser boundary.
- Preserve semantic provenance until reconciliation is complete.
  Post-processors receive explicit/fallback/inherited distinctions, not raw
  JSON or source-field paths.
- Request state, continuation locators, caches, and diagnostics remain
  operational context rather than plugin domain metadata.

## 8. Host safety and resource constraints

Fooyin process safety takes precedence over completing a resolution. Failures
controlled by the plugin must produce bounded, isolated outcomes, not crashes,
undefined behavior, or an unresponsive host.

### External input and resource limits

- Treat HTML, JSON, URLs, redirects, stream locators, and persisted properties
  as untrusted. Validate types, array bounds, identity, URL components, and
  numeric conversions before use; reject non-finite and out-of-range numbers.
- Bound response bytes while reading, parser input/depth, field lengths,
  in-flight requests, active CPU work, redirects, and diagnostic work.
  Limits are explicit constants/settings with tests at their boundaries.
- Resource-limit violations are structured failures, not silent truncation,
  hidden item caps, skipped children, or success-shaped defaults.
- Pending release references, associations, cached domain entities, and final
  domain results are ordinary operation-owned collections, not a reason to
  impose hidden item caps or a caller-managed batching protocol.
- Bound the heavy active working set through execution parallelism and source
  limits. Release raw bodies after parsing and candidates after assembly;
  retain only the context needed to finish active work.

### Operation lifecycle

- Cancellation is a caller or owner withdrawing unfinished work, including
  owner teardown or host shutdown. It is an operational control signal, not a
  metadata defect.
- Deadline expiry and HTTP failures, including rate limiting, are failures,
  not cancellation. Page fetches are not automatically retried: one call
  produces one outcome, without retry settings or backoff.
- Each active release workflow has a finite processing deadline, including
  required parent acquisition. Waiting in the submitted list does not consume
  that processing budget. Any caller-wide deadline is a separate policy.
  Each operation reaches one terminal state, including failure and cancellation
  paths; late results cannot complete it again.
- Withdrawal stops further scheduling and releases work/resources no longer
  needed by that operation. Independent submissions do not share cancellation
  or physical request ownership.
  CPU work checks cancellation where applicable; workers are never forcibly
  terminated. No blocking network wait runs on the UI thread.

### Threads, lifetime, and exceptions

- Respect QObject thread affinity. Parsing/refinement workers use owned or
  immutable data; Fooyin UI, library, playlist, and playback objects are
  accessed only on their owning/main thread as required by their APIs.
- Callbacks are lifetime-guarded, replies are cleaned up exactly once, and
  destroyed consumers or shutdown cannot receive stale completions.
- Expected bad input, missing fields, and network errors use typed outcomes.
  Contain exceptions from exception-throwing dependencies at explicit
  plugin/task boundaries and translate them to structured failures.
- No exception escapes into a Qt event loop, host callback, worker completion,
  or decoder API. A `noexcept` annotation alone is not containment: escaping
  exceptions terminate the process.
- Never use fatal logging, assertions, or unchecked access to handle remote
  input. Boundary handling must not conceal errors or return success defaults.
  Exception handling is not a substitute for bounds/lifetime correctness and
  cannot make undefined behavior or process-wide memory exhaustion recoverable.

## 9. Testing strategy

Tests establish semantic correctness, host safety, and live compatibility.
Classify them by dependency into **deterministic** and **live** execution groups.
Semantic correctness and robustness are coverage goals, not separate execution
groups. Unit versus integration describes test scope, not whether a test needs
Bandcamp. Sanitizers are a build mode applied to the same test cases.
Coverage follows the owning component's responsibilities rather than a duplicate
catalogue of individual test cases in this document.

### Deterministic tests

Tests in [tests/deterministic](tests/deterministic/) use synthetic inputs and
controlled dependencies, with no live Bandcamp or other external-service
dependency. This group includes isolated unit tests, deterministic
component-composition tests, and fault/mutation tests. The
`bandcamp_deterministic_tests` target is registered with CTest as
`bandcamp.deterministic`. CI runs the entire group, not just semantic happy
paths.

An integration-scope test using synthetic inputs remains deterministic.

#### Semantic correctness

- Cover each post-processor's business rules in isolation and the composed
  pipelines' ordering, context, and invariant preservation.
- Cover parser mappings, authoritative assembly, relationship validation,
  domain admission, and selection/atomicity policies with deterministic inputs.
  A successful resolution alone does not prove correct metadata.
- Cover URL/identity handling and settings behaviour at their own boundaries.
- Cover the logging contract's filtering, semantic event context, correlation,
  effective changes versus no-ops, and redaction, not incidental message
  formatting.
- Add focused deterministic regression coverage alongside defect fixes.

#### Robustness and host safety

- Exercise malformed, truncated, and oversized external data, invalid types,
  numeric overflow, and resource-limit boundaries.
- Exercise operational failures and asynchronous lifecycle safety, including
  deadlines, cancellation, shared work, consumer destruction, and exception
  containment. Verify bounded, explicit outcomes rather than merely no crash.
- Use deterministic fault/mutation tests for recoverable failures. Keep these
  cases in the normal deterministic suite rather than an optional hardening
  suite.

### Live compatibility tests

- Tests in [tests/live](tests/live/) invoke the production
  resolution entry point with real Bandcamp URLs, exercising fetching,
  parsing, assembly, validation, and post-processing as one chain.
- These are happy-path compatibility smoke tests, not exhaustive catalogue or
  edge-case tests. Reuse the existing production infrastructure; deterministic
  tests own detailed mapping, reconciliation, and failure-policy coverage.
- Cover artist and label discographies, albums, album-member tracks, and
  standalone tracks. Assert successful completion within finite deadlines
  and meaningful, nonempty results for the requested scope.
- Keep assertions limited to the public outcome contract. Do not duplicate
  source extraction, metadata reconciliation, or validation logic, or freeze
  mutable live titles, durations, and track counts as expected snapshots.
- Network errors, rate limits, challenge pages, and unavailable resources are
  visible test failures, not passing skips. They mean live compatibility was
  not demonstrated, not necessarily that the code regressed.
- Bound request volume and concurrency and give the suite a finite watchdog.
- The `bandcamp_live_tests` target is registered with CTest as `bandcamp.live`
  and labeled `live`. Excluding that label selects all deterministic tests,
  including deterministic integration and robustness coverage.

### Build instrumentation and execution presets

- Compile deterministic and live test targets when testing is enabled.
  Compilation itself performs no live requests.
- The `tests` configure/build setup is the normal build. The `tests-sanitize`
  setup instruments production code and test binaries with ASan/UBSan on
  supported compilers. It runs the same assertions with additional checks for
  memory errors and undefined behaviour; it is not another test suite.
- Sanitizers complement semantic and robustness assertions and detect defects
  only on exercised paths. Sanitizer diagnostics must fail the run, not merely
  appear in successful test logs.

| Test preset | Selected tests | Configure/build preset | Intended use |
| --- | --- | --- | --- |
| `tests` | All deterministic and live tests | `tests` | Default manual local run; first in README |
| `tests-sanitize` | All deterministic and live tests | `tests-sanitize` | Manual full run with ASan/UBSan |
| `tests-ci` | All deterministic tests; exclude label `live` | `tests-sanitize` | CI with ASan/UBSan; also usable locally offline |
| `tests-live` | Only tests labelled `live` | `tests` | Manual Bandcamp compatibility check |

Only `tests-ci` excludes live tests. "All tests" means all registered test cases,
not every possible compiler or build-mode combination. `tests-ci` and
`tests-live` are test-selection presets reusing existing build setups; do not
add duplicate build directories or a live-test build flag.

CI builds the ordinary plugin and the sanitized test targets, then runs the
entire deterministic suite:

```sh
cmake --preset tests-sanitize
cmake --build --preset tests-sanitize
ctest --preset tests-ci
```

Do not additionally run `tests`, `tests-sanitize`, or `tests-live` test presets in
CI for Increment 6: each includes live execution. A live CI step may be added
later through an explicit policy change. CI need not repeat the deterministic
suite without sanitizers for this increment.

## 10. Logging and diagnostic events

### Responsibility and transport

Resolution outcomes report whether work succeeded. Logging explains source
observations, decisions, effective transitions, and operational failures; log
levels never change domain validity.

Use the existing Fooyin/Qt logging infrastructure. Declare categories in
[bandcamplogging.h](src/bandcamplogging.h) and
[bandcamplogging.cpp](src/bandcamplogging.cpp):

- Standard events use `fy.bandcamp.<area>` with the appropriate Qt severity.
- TRACE uses `qCDebug` with `fy.trace.bandcamp.<area>`. Fooyin recognizes that
  prefix as Trace and filters it separately from ordinary Debug; a text label
  on a normal debug category is not equivalent.
- Areas match the emitting stage: `ui`, `fetch`, `parser`, `resolve`, `tracks`,
  or `settings`. Do not change global logging rules to attach local context.

### Event contract

| Event | Level | Required detail |
| --- | --- | --- |
| Decoded Bandcamp release JSON | TRACE | A redacted full source snapshot for every fetched page, once per request, before field mapping. |
| Source-to-candidate mapping or effective field change | TRACE | Field, old/new values, source/processor, and reason; source JSONPaths where the parser owns them. |
| Field fallback or inheritance | TRACE | Old/new values and the selected source/context; explicit markers distinguish unset or invalid input. |
| Conflicting usable source fields | DEBUG | Both concrete JSONPaths and values, the chosen source/value, and the precedence reason. |
| Unresolved field after page-local sources are exhausted | DEBUG | Field/paths and missing/invalid reason. This observation does not itself fail resolution. |
| Resolution/request lifecycle transition | TRACE | Stage/state change and associated scope; include acquisition, assembly, admission, and completion links. |
| Page/job completion summary | INFO | Outcome and relevant success/failure/cancellation counts, not warning totals. |
| Terminal resolution failure | WARN | Structured error code, stage, resource URL, known identity, and concise reason, once for that requested outcome. |

Initial mapping is a transition from unset to a candidate value. Processors
log only effective mutations, using domain field names and semantic provenance,
never raw JSON access. A no-op processor emits no start/change/finish event.
Propagating an existing error does not repeatedly log the same failure.

Invalid JSON has no decoded snapshot: report extraction/decoding failure with
safe location/size information, not an unredacted HTML/body dump.

### Correlation

- Every event carries the available operation/job and request context. Each
  track-scoped event is prefixed by its canonical `bandcamp://track/<trackId>`
  as soon as a trustworthy ID exists.
- Before identity is known, use job/request and resource context, not a
  fabricated track URI. Album/shared-page events use release/request context.
- Track events refer to the request that supplied their data, including shared
  album requests. Link consumers explicitly; do not duplicate the source JSON
  or album events for every track.
- Propagate immutable correlation context through asynchronous work. Never
  rely on a global mutable current track, or thread-local state surviving a
  task switch without explicit propagation.
- Escape untrusted field text. A multiline logical event retains its context
  on every rendered line, or is emitted as one safely encoded record.

### Filtering, redaction, and cost

- Source snapshots and transitions always have TRACE instrumentation, but
  emission respects the configured level. Disabled TRACE must not serialize
  JSON or build expensive diagnostic strings.
- Never emit signed stream URLs, credentials, or authorization/signature
  material at any level. Redact sensitive URL components before formatting
  snapshots, transitions, errors, and summaries; retain field structure and
  nonsecret metadata for troubleshooting.
- Diagnostic formatting, buffering, and retention are bounded. Logging must
  not introduce an unbounded queue, retain page bodies unnecessarily, recurse
  through a failing logger, or jeopardize the host.
