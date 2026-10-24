#pragma once

#include "bandcampdiagnostics.h"
#include "bandcampresolution.h"

#include <QByteArray>
#include <QDate>
#include <QList>

namespace Fooyin::Bandcamp {
constexpr qsizetype MaxPageBytes        = 20 * 1024 * 1024;
constexpr int MaxJsonDepth              = 64;
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
    CandidateField<qint64> artId;
};

struct AlbumCandidate
{
    CandidateField<qint64> id;
    CandidateField<QUrl> sourceUrl;
    CandidateField<QString> title;
    CandidateField<QString> artist;
    CandidateField<QDate> releaseDate;
    CandidateField<qint64> artId;
    QList<TrackCandidate> tracks;
};

enum class ParentDeclaration : quint8 { Unknown, Invalid, NoParent, AlbumBacked };

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
    CandidateField<qint64> fallbackArtId;
};

using ReleaseCandidate    = std::variant<AlbumCandidate, TrackPageCandidate>;
using ReleaseParseOutcome = ResolutionOutcome<ReleaseCandidate>;

// Kind is determined by decoded source identity, never by a URL or the caller's
// selection. Raw JSON, concrete source paths and diagnostic history stay inside
// the parser.
ReleaseParseOutcome parseReleasePage(const QByteArray& body, const QUrl& pageUrl, CorrelationContext context = {});
} // namespace Fooyin::Bandcamp
