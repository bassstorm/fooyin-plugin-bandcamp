#include "bandcampoutcome.h"

namespace Fooyin::Bandcamp {
ResolutionError makeResolutionError(ResolutionErrorCode code, ResolutionStage stage, const QUrl& url, qint64 trackId,
                                    QString field, QString reason)
{
    return {code, stage, url, trackId, std::move(field), std::move(reason)};
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

} // namespace Fooyin::Bandcamp
