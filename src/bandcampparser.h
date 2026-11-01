#pragma once

#include "bandcampdiagnostics.h"
#include "bandcampresolution.h"
#include "bandcampurl.h"

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QUrl>

#include <atomic>
#include <optional>

namespace Fooyin::Bandcamp {
constexpr qsizetype MaxPageBytes        = 20 * 1024 * 1024;
constexpr int MaxJsonDepth              = 64;
constexpr qsizetype MaxHtmlDepth        = 256;
constexpr qsizetype MaxSourceFieldChars = 16384;
constexpr qsizetype MaxSourceRecords    = 100000;
constexpr qsizetype MaxDeclaredTracks   = 10000;
constexpr qint64 MaxDurationMs          = 604800000;
constexpr qint64 MaxTrackNumber         = 100000;

struct TrackCandidate
{
    // List order is preserved, even when the declared record is malformed.
    qsizetype declaredIndex{-1};
    FieldState recordState{FieldState::Missing};
    CandidateField<qint64> id;
    CandidateField<QString> title;
    CandidateField<QString> artist;
    CandidateField<qint64> trackNumber;
    CandidateField<qint64> durationMs;
    // Validated HTTPS locator kept verbatim: QUrl serialization can change signed queries.
    CandidateField<QString> streamUrl;
    CandidateField<QDate> releaseDate;
};

struct AlbumCandidate
{
    CandidateField<qint64> id;
    CandidateField<QUrl> sourceUrl;
    CandidateField<QString> title;
    CandidateField<QString> artist;
    CandidateField<QDate> releaseDate;
    QList<TrackCandidate> tracks;
};

enum class ParentDeclaration : quint8
{
    Unknown,
    Invalid,
    NoParent,
    AlbumBacked
};

struct ParentEvidence
{
    ParentDeclaration declaration{ParentDeclaration::Unknown};
    CandidateField<qint64> albumId;
    CandidateField<QUrl> albumUrl;
};

struct TrackPageCandidate
{
    CandidateField<QUrl> sourceUrl;
    TrackCandidate track;
    // Missing means no matching ID; Invalid means ambiguous duplicate matches.
    CandidateField<qsizetype> matchingRecord;
    ParentEvidence parent;
    // Context, not explicit facts about the track.
    CandidateField<QString> fallbackArtist;
};

using ReleaseCandidate    = std::variant<AlbumCandidate, TrackPageCandidate>;
using ReleaseParseOutcome = ResolutionOutcome<ReleaseCandidate>;

struct BandcampListingReference
{
    qsizetype ordinal{0};
    qint64 id{0};
    BandcampPageUrl page;
};

struct BandcampListingContinuation
{
    QUrl locator;
};

struct BandcampListingBatch
{
    QUrl effectiveUrl;
    QList<BandcampListingReference> references;
    std::optional<BandcampListingContinuation> continuation;
};

using BandcampListingParseOutcome = ResolutionOutcome<BandcampListingBatch>;

// Kind is determined by decoded source identity, never by a URL or the caller's
// selection. Raw JSON, concrete source paths and diagnostic history stay inside
// the parser.
ReleaseParseOutcome parseReleasePage(const QByteArray& body, const QUrl& pageUrl, CorrelationContext context = {},
                                     const std::atomic_bool* stop = nullptr);
BandcampListingParseOutcome parseDiscographyPage(const QByteArray& body, const QUrl& pageUrl,
                                                 CorrelationContext context   = {},
                                                 const std::atomic_bool* stop = nullptr);
} // namespace Fooyin::Bandcamp
