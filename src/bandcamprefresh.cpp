#include "bandcamprefresh.h"

#include "bandcamplogging.h"
#include "bandcamptrackuri.h"

#include <QDateTime>
#include <QUrl>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
bool isBandcamp(const Fooyin::Track& track)
{
    return track.filepath().startsWith(u"bandcamp:"_s, Qt::CaseInsensitive)
        || track.extraProperties().value(StreamProviderProperty) == u"bandcamp"_s;
}

struct PersistedTransport
{
    BandcampPageUrl source;
    QString streamUrl;
    BandcampStreamExpiry expiry;
};

using PersistedResult = std::variant<PersistedTransport, BandcampRefreshFailure>;

PersistedResult readPersistedTransport(const Fooyin::Track& track, qint64 id)
{
    const auto invalid = [](QString reason) -> PersistedResult {
        return BandcampRefreshFailure{BandcampRefreshError::InvalidPersistedState, std::move(reason)};
    };
    const auto properties = track.extraProperties();
    if(!id || properties.value(StreamProviderProperty) != u"bandcamp"_s
       || (track.hasExtraProperty(BandcampTrackIdProperty)
           && properties.value(BandcampTrackIdProperty) != QString::number(id))) {
        return invalid(QStringLiteral("The selected track has inconsistent Bandcamp identity or provider state."));
    }
    const QString sourceText = properties.value(BandcampSourceUrlProperty);
    const auto source = parseBandcampPageUrl(sourceText);
    if(!source.isValid() || source.kind == BandcampPageKind::Discography || source.url != sourceText) {
        return invalid(QStringLiteral("The selected track has no canonical release source."));
    }
    const QString streamText = properties.value(BandcampStreamUrlProperty);
    if(!isSupportedBandcampStreamUrl(streamText)) {
        return invalid(QStringLiteral("The selected track has an invalid HTTPS stream locator."));
    }
    BandcampStreamExpiry expiry;
    const QString expiryText = properties.value(BandcampExpiresAtProperty);
    if(!expiryText.isEmpty()) {
        bool ok = false;
        const qint64 milliseconds = expiryText.size() <= 19 ? expiryText.toLongLong(&ok) : 0;
        const auto observed = bandcampStreamExpiryMs(streamText);
        if(ok && milliseconds > 0 && QString::number(milliseconds) == expiryText
           && observed && *observed == milliseconds) {
            expiry = {BandcampExpiryState::Valid, milliseconds};
        }
        else {
            expiry.state = BandcampExpiryState::Invalid;
        }
    }
    return PersistedTransport{source, streamText, expiry};
}

CorrelationContext correlation(BandcampOperationId id)
{
    return {.jobId = static_cast<qint64>(id)};
}

void logBoundaryException(const CorrelationContext& context, const QString& event, const QString& reason)
{
    qCWarning(BANDCAMP_TRACKS).noquote()
        << formatTraceEvent(context, event, {DiagnosticField::text(QStringLiteral("reason"), reason)});
}
} // namespace

bool bandcampRefreshNeeded(BandcampStreamExpiry expiry, qint64 nowMs, qint64 windowMs)
{
    return expiry.state != BandcampExpiryState::Valid || expiry.milliseconds <= nowMs
        || expiry.milliseconds - nowMs < windowMs;
}

bool hasSelectedBandcampTracks(const Fooyin::TrackList& selection)
{
    return std::any_of(selection.cbegin(), selection.cend(), isBandcamp);
}

BandcampRefreshOperation::BandcampRefreshOperation(BandcampService* service, Fooyin::TrackList selected,
                                                   OperationOptions options, FooyinLibraryApi libraryApi,
                                                   QObject* parent)
    : QObject{parent}
    , m_service{service}
    , m_selected{std::move(selected)}
    , m_options{clampOperationOptions(options)}
    , m_gateway{new BandcampLibraryGateway{std::move(libraryApi), this}}
{
    if(service) {
        connect(service, &BandcampService::scopeResolved, this, &BandcampRefreshOperation::acceptScope);
        connect(service, &BandcampService::progressChanged, this, [this](const auto& progress) {
            if(!m_stopping && !m_finished && progress.operationId == m_summary.operationId) {
                const auto context = correlation(m_summary.operationId);
                try {
                    emit progressChanged(progress);
                }
                catch(...) {
                    logBoundaryException(context, QStringLiteral("refresh.observer.failed"),
                                         QStringLiteral("progress observer threw"));
                }
            }
        });
        connect(service, &BandcampService::operationFinished, this, [this](const auto& summary) {
            if(summary.operationId != m_summary.operationId || m_finished) {
                return;
            }
            m_sourceFinished = true;
            failPending({BandcampRefreshError::OperationFailed,
                         QStringLiteral("The release operation did not supply every requested result.")});
        });
        connect(service, &QObject::destroyed, this, [this]() {
            if(m_finished) {
                return;
            }
            m_sourceFinished = true;
            failPending({BandcampRefreshError::SourceUnavailable,
                         QStringLiteral("The release service was destroyed.")});
        });
    }
}

BandcampRefreshOperation::~BandcampRefreshOperation()
{
    m_finished = true;
    (void)withdrawSource();
    m_gateway->cancel();
}

void BandcampRefreshOperation::start(qint64 nowMs)
{
    if(m_started || m_finished) {
        return;
    }
    m_started = true;
    const QPointer<BandcampRefreshOperation> guard{this};
    QList<BandcampPageUrl> pages;
    QList<QPair<qint64, BandcampRefreshOutcome>> immediate;
    struct PreparedTransport
    {
        qint64 id;
        QString sourceUrl;
        QString streamUrl;
        qint64 expiresAtMs;
    };
    QList<PreparedTransport> reusableTransports;
    try {
        QSet<QString> identities;
        auto selection = std::move(m_selected);
        QList<QPair<qint64, Fooyin::Track>> accepted;
        for(const Fooyin::Track& track : selection) {
            if(!isBandcamp(track) || identities.contains(track.filepath())) {
                continue;
            }
            identities.insert(track.filepath());
            const auto id = parseBandcampTrackUri(track.filepath()).value_or(0);
            if(id) {
                m_unsettled.insert(id);
            }
            else {
                ++m_anonymousUnsettled;
            }
            accepted.append({id, track});
        }
        for(const auto& [id, track] : accepted) {
            const auto input = readPersistedTransport(track, id);
            if(const auto* error = std::get_if<BandcampRefreshFailure>(&input)) {
                immediate.append({id, *error});
            }
            else {
                const auto& state = std::get<PersistedTransport>(input);
                if(!bandcampRefreshNeeded(state.expiry, nowMs, m_options.prePlaybackRefreshWindowMs)) {
                    reusableTransports.append(
                        {id, state.source.url, state.streamUrl, state.expiry.milliseconds});
                    immediate.append({id, BandcampRefreshSuccess::Unchanged});
                }
                else {
                    if(!m_consumers.contains(state.source.url)) {
                        pages.append(state.source);
                    }
                    m_consumers[state.source.url].append(id);
                }
            }
        }
    }
    catch(...) {
        if(guard) {
            stopWithFailure({BandcampRefreshError::OperationFailed,
                             QStringLiteral("Preparing selected refresh consumers failed.")});
        }
        return;
    }
    for(const auto& transport : reusableTransports) {
        try {
            emit transportPrepared(transport.id, transport.sourceUrl, transport.streamUrl,
                                   transport.expiresAtMs, false);
        }
        catch(...) {
            if(guard) {
                stopWithFailure({BandcampRefreshError::OperationFailed,
                                 QStringLiteral("Publishing reusable transport failed.")});
            }
            return;
        }
        if(!guard) {
            return;
        }
        if(m_stopping || m_finished) {
            break;
        }
    }
    for(const auto& [id, outcome] : immediate) {
        settle(id, m_stopping ? BandcampRefreshOutcome{BandcampRefreshCancelled{}} : outcome);
        if(!guard) {
            return;
        }
    }
    if(m_stopping || pages.isEmpty()) {
        m_sourceFinished = true;
        failPending({BandcampRefreshError::OperationFailed,
                     QStringLiteral("The refresh operation stopped before submission.")});
        return;
    }
    if(!m_service) {
        stopWithFailure({BandcampRefreshError::SourceUnavailable,
                         QStringLiteral("The release service is unavailable.")});
        return;
    }
    BandcampSubmission submission;
    try {
        submission = m_service->submit(pages, m_options);
    }
    catch(...) {
        if(guard) {
            stopWithFailure({BandcampRefreshError::OperationFailed,
                             QStringLiteral("Submitting the release operation failed.")});
        }
        return;
    }
    if(!guard) {
        return;
    }
    if(const auto* accepted = std::get_if<BandcampOperationAccepted>(&submission)) {
        m_summary.operationId = accepted->id;
        if(m_stopping) {
            m_sourceFinished = false;
            if(!withdrawSource() && guard) {
                stopWithFailure({BandcampRefreshError::OperationFailed,
                                 QStringLiteral("Withdrawing the release operation failed.")});
            }
            return;
        }
        qCInfo(BANDCAMP_TRACKS).noquote()
            << formatTraceEvent(correlation(accepted->id), QStringLiteral("refresh.started"),
                                {DiagnosticField::number(QStringLiteral("consumers"), m_unsettled.size()),
                                 DiagnosticField::number(QStringLiteral("window_ms"),
                                                         m_options.prePlaybackRefreshWindowMs)});
    }
    else {
        const auto& error = std::get<BandcampSubmissionRejected>(submission).error;
        stopWithFailure({BandcampRefreshError::SubmissionRejected, error.reason, error});
    }
}

void BandcampRefreshOperation::acceptScope(const BandcampScopeResult& result)
{
    if(m_finished || result.operationId != m_summary.operationId) {
        return;
    }
    const QPointer<BandcampRefreshOperation> guard{this};
    const auto ids = m_consumers.take(result.page.url);
    try {
        const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&result.outcome);
        QHash<qint64, const BandcampTrack*> tracksById;
        const BandcampAlbumMetadata* parent{nullptr};
        if(success) {
            if(const auto* album = std::get_if<BandcampAlbum>(&success->value)) {
                parent = &album->metadata;
                tracksById.reserve(album->tracks.size());
                for(const BandcampTrack& track : album->tracks) {
                    tracksById.insert(track.id, &track);
                }
            }
            else {
                const auto& track = std::get<BandcampResolvedTrack>(success->value);
                tracksById.insert(track.track.id, &track.track);
                parent = track.album ? &*track.album : nullptr;
            }
        }
        for(qint64 id : ids) {
            if(m_stopping || std::holds_alternative<ResolutionCancelled>(result.outcome)) {
                settle(id, BandcampRefreshCancelled{});
            }
            else if(!success) {
                const auto& error = std::get<ResolutionFailure>(result.outcome).error;
                settle(id, BandcampRefreshFailure{BandcampRefreshError::ResolutionFailed, error.reason, error}, true);
            }
            else {
                const auto matched = tracksById.constFind(id);
                if(matched == tracksById.cend()) {
                    settle(id, BandcampRefreshFailure{BandcampRefreshError::MissingSelectedTrack,
                                                      QStringLiteral("The release no longer lists the selected track.")});
                }
                else {
                    const auto expiry = bandcampStreamExpiryMs((*matched)->streamUrl);
                    if(!expiry || *expiry <= QDateTime::currentMSecsSinceEpoch()) {
                        settle(id, BandcampRefreshFailure{BandcampRefreshError::UnusableRefreshedExpiry,
                                                          QStringLiteral("The refreshed stream has no usable future expiry.")});
                    }
                    else {
                        emit transportPrepared(id, (*matched)->sourceUrl, (*matched)->streamUrl, *expiry, true);
                        if(!guard) {
                            return;
                        }
                        if(m_stopping || m_finished) {
                            settle(id, BandcampRefreshCancelled{});
                            if(!guard) {
                                return;
                            }
                            continue;
                        }
                        m_pendingWrites.enqueue({**matched, parent ? std::optional{*parent} : std::nullopt});
                    }
                }
            }
            if(!guard) {
                return;
            }
        }
        pump();
    }
    catch(...) {
        if(guard) {
            stopWithFailure({BandcampRefreshError::OperationFailed,
                             QStringLiteral("Associating refreshed release tracks failed.")});
        }
    }
}

void BandcampRefreshOperation::pump()
{
    if(m_writingTrackId || m_finished) {
        return;
    }
    if(m_stopping || m_pendingWrites.isEmpty()) {
        finishIfReady();
        return;
    }
    auto track = m_pendingWrites.dequeue();
    const auto id = track.track.id;
    const auto context = correlation(m_summary.operationId).withTrack(id);
    m_writingTrackId = id;
    const QPointer<BandcampRefreshOperation> guard{this};
    try {
        m_gateway->refreshTransport(std::move(track), m_options.itemTimeoutMs, this, context,
                                     [guard, id](FooyinTrackCommitOutcome outcome) {
            if(!guard) {
                return;
            }
            if(const auto* success = std::get_if<FooyinTrackCommitSuccess>(&outcome)) {
                guard->settle(id, success->updatedResidentCount > 0 ? BandcampRefreshSuccess::Updated
                                                                   : BandcampRefreshSuccess::Unchanged);
            }
            else if(std::get<FooyinTrackCommitFailure>(outcome).code == FooyinTrackCommitErrorCode::Cancelled) {
                guard->settle(id, BandcampRefreshCancelled{});
            }
            else {
                guard->settle(id, BandcampRefreshFailure{BandcampRefreshError::WriteFailed,
                                                        std::get<FooyinTrackCommitFailure>(outcome).reason}, true);
            }
            if(guard) {
                guard->m_writingTrackId = 0;
                guard->pump();
            }
        });
    }
    catch(...) {
        if(guard) {
            settle(id, BandcampRefreshFailure{BandcampRefreshError::OperationFailed,
                                              QStringLiteral("The transport write could not start.")});
            if(guard) {
                m_writingTrackId = 0;
                pump();
            }
        }
    }
}

void BandcampRefreshOperation::settle(qint64 id, BandcampRefreshOutcome outcome, bool alreadyLogged)
{
    if(id) {
        if(!m_unsettled.remove(id)) {
            return;
        }
    }
    else {
        if(!m_anonymousUnsettled) {
            return;
        }
        --m_anonymousUnsettled;
    }
    if(const auto* success = std::get_if<BandcampRefreshSuccess>(&outcome)) {
        *success == BandcampRefreshSuccess::Updated ? ++m_summary.updated : ++m_summary.unchanged;
    }
    else if(std::holds_alternative<BandcampRefreshCancelled>(outcome)) {
        ++m_summary.cancelled;
    }
    else {
        ++m_summary.failed;
        const auto& error = std::get<BandcampRefreshFailure>(outcome);
        if(!alreadyLogged) {
            qCWarning(BANDCAMP_TRACKS).noquote()
                << formatTraceEvent(correlation(m_summary.operationId).withTrack(id),
                                    QStringLiteral("refresh.consumer.failed"),
                                    {DiagnosticField::number(QStringLiteral("code"), static_cast<qint64>(error.code)),
                                     DiagnosticField::text(QStringLiteral("reason"), error.reason)});
        }
    }
    const auto context = correlation(m_summary.operationId).withTrack(id);
    try {
        emit consumerFinished(id, outcome);
    }
    catch(...) {
        logBoundaryException(context, QStringLiteral("refresh.observer.failed"),
                             QStringLiteral("consumer observer threw"));
    }
}

bool BandcampRefreshOperation::withdrawSource()
{
    if(m_withdrawalIssued || m_sourceFinished || !m_summary.operationId) {
        return true;
    }
    m_withdrawalIssued = true;
    const auto service = m_service;
    const auto id = m_summary.operationId;
    const QPointer<BandcampRefreshOperation> guard{this};
    try {
        if(service) {
            service->cancel(id);
        }
    }
    catch(...) {
        logBoundaryException(correlation(id), QStringLiteral("refresh.cancel.failed"),
                             QStringLiteral("release cancellation threw"));
        if(guard) {
            // Physical work remains service-owned; detach late callbacks after failed withdrawal.
            disconnect(service, nullptr, this, nullptr);
            m_sourceFinished = true;
        }
        return false;
    }
    return true;
}

void BandcampRefreshOperation::cancel()
{
    if(m_finished || m_stopping) {
        return;
    }
    m_stopping = true;
    const QPointer<BandcampRefreshOperation> guard{this};
    const bool withdrawn = withdrawSource();
    if(!guard) {
        return;
    }
    if(!withdrawn) {
        stopWithFailure({BandcampRefreshError::OperationFailed,
                         QStringLiteral("Withdrawing the release operation failed.")});
        return;
    }
    m_gateway->cancel();
    if(!guard) {
        return;
    }
    const auto pending = std::exchange(m_pendingWrites, {});
    for(const auto& track : pending) {
        settle(track.track.id, BandcampRefreshCancelled{});
        if(!guard) {
            return;
        }
    }
    if(!m_summary.operationId) {
        m_sourceFinished = true;
        failPending({BandcampRefreshError::OperationFailed,
                     QStringLiteral("The refresh operation was withdrawn before submission.")});
    }
    else {
        finishIfReady();
    }
}

void BandcampRefreshOperation::failPending(BandcampRefreshFailure error)
{
    const auto consumers = std::exchange(m_consumers, {});
    const QPointer<BandcampRefreshOperation> guard{this};
    for(const auto& ids : consumers) {
        for(qint64 id : ids) {
            settle(id, m_stopping ? BandcampRefreshOutcome{BandcampRefreshCancelled{}}
                                 : BandcampRefreshOutcome{error});
            if(!guard) {
                return;
            }
        }
    }
    pump();
}

void BandcampRefreshOperation::stopWithFailure(BandcampRefreshFailure error)
{
    qCWarning(BANDCAMP_TRACKS).noquote()
        << formatTraceEvent(correlation(m_summary.operationId), QStringLiteral("refresh.operation.failed"),
                            {DiagnosticField::number(QStringLiteral("code"), static_cast<qint64>(error.code)),
                             DiagnosticField::text(QStringLiteral("reason"), error.reason)});
    m_stopping = true;
    m_consumers.clear();
    m_pendingWrites.clear();
    const QPointer<BandcampRefreshOperation> guard{this};
    (void)withdrawSource();
    if(!guard) {
        return;
    }
    m_gateway->cancel();
    if(!guard) {
        return;
    }
    if(!m_summary.operationId) {
        m_sourceFinished = true;
    }
    const auto unsettled = m_unsettled;
    for(qint64 id : unsettled) {
        if(id != m_writingTrackId) {
            settle(id, error, true);
            if(!guard) {
                return;
            }
        }
    }
    while(m_anonymousUnsettled) {
        settle(0, error, true);
        if(!guard) {
            return;
        }
    }
    finishIfReady();
}

void BandcampRefreshOperation::finishIfReady()
{
    if(m_finished || !m_started || !m_sourceFinished || m_writingTrackId || !m_unsettled.isEmpty()
       || m_anonymousUnsettled
       || !m_pendingWrites.isEmpty()) {
        return;
    }
    m_finished = true;
    const auto summary = m_summary;
    qCInfo(BANDCAMP_TRACKS).noquote()
        << formatTraceEvent(correlation(summary.operationId), QStringLiteral("refresh.completed"),
                            {DiagnosticField::number(QStringLiteral("updated"), summary.updated),
                             DiagnosticField::number(QStringLiteral("unchanged"), summary.unchanged),
                             DiagnosticField::number(QStringLiteral("failed"), summary.failed),
                             DiagnosticField::number(QStringLiteral("cancelled"), summary.cancelled)});
    try {
        emit finished(summary);
    }
    catch(...) {
        logBoundaryException(correlation(summary.operationId), QStringLiteral("refresh.observer.failed"),
                             QStringLiteral("completion observer threw"));
    }
}
} // namespace Fooyin::Bandcamp
