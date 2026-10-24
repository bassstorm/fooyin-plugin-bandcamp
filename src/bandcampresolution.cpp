#include "bandcampresolution.h"

#include <algorithm>

namespace Fooyin::Bandcamp {
std::optional<ResolvedScope> resolveScope(const RequestedScope& requested, const ParentState& parent)
{
    if(std::holds_alternative<AlbumSelection>(requested)) {
        return WholeAlbumScope{};
    }

    const qint64 trackId = std::get<TrackSelection>(requested).trackId;

    if(std::holds_alternative<UnknownParent>(parent)) {
        return std::nullopt;
    }
    if(std::holds_alternative<StandaloneParent>(parent)) {
        return StandaloneTrackScope{trackId};
    }
    return AlbumTrackScope{trackId};
}

QString resolutionErrorCodeName(ResolutionErrorCode code)
{
    switch(code) {
        case ResolutionErrorCode::FetchFailed:
            return QStringLiteral("fetch_failed");
        case ResolutionErrorCode::Timeout:
            return QStringLiteral("timeout");
        case ResolutionErrorCode::PageTooLarge:
            return QStringLiteral("page_too_large");
        case ResolutionErrorCode::ResourceLimitExceeded:
            return QStringLiteral("resource_limit_exceeded");
        case ResolutionErrorCode::UnsupportedRequest:
            return QStringLiteral("unsupported_request");
        case ResolutionErrorCode::MalformedPage:
            return QStringLiteral("malformed_page");
        case ResolutionErrorCode::RequiredFieldMissing:
            return QStringLiteral("required_field_missing");
        case ResolutionErrorCode::RelationshipUnresolved:
            return QStringLiteral("relationship_unresolved");
        case ResolutionErrorCode::ParentUnavailable:
            return QStringLiteral("parent_unavailable");
        case ResolutionErrorCode::NoResolvableTracks:
            return QStringLiteral("no_resolvable_tracks");
        case ResolutionErrorCode::InvalidField:
            return QStringLiteral("invalid_field");
    }
    return QStringLiteral("unknown");
}

QString resolutionStageName(ResolutionStage stage)
{
    switch(stage) {
        case ResolutionStage::Fetch:
            return QStringLiteral("fetch");
        case ResolutionStage::Parse:
            return QStringLiteral("parse");
        case ResolutionStage::Acquire:
            return QStringLiteral("acquire");
        case ResolutionStage::Assemble:
            return QStringLiteral("assemble");
        case ResolutionStage::Admit:
            return QStringLiteral("admit");
        case ResolutionStage::Refine:
            return QStringLiteral("refine");
        case ResolutionStage::Publish:
            return QStringLiteral("publish");
    }
    return QStringLiteral("unknown");
}

OperationOptions clampOperationOptions(OperationOptions options)
{
    options.parallelism   = std::clamp(options.parallelism, MinParallelism, MaxParallelism);
    options.itemTimeoutMs = std::clamp(options.itemTimeoutMs, MinItemTimeoutMs, MaxItemTimeoutMs);
    return options;
}
} // namespace Fooyin::Bandcamp
