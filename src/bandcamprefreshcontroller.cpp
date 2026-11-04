#include "bandcamprefreshcontroller.h"

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

CorrelationContext correlation(BandcampOperationId id)
{
    return {.jobId = static_cast<qint64>(id)};
}
} // namespace

bool hasSelectedBandcampTracks(const Fooyin::TrackList& selection)
{
    return std::any_of(selection.cbegin(), selection.cend(), isBandcamp);
}

BandcampRefreshController::BandcampRefreshController(BandcampResolutionService* service, Fooyin::TrackList selected,
                                                     OperationOptions options, FooyinLibraryApi libraryApi,
                                                     QObject* parent)
    : QObject{parent}
    , m_service{service}
    , m_selected{std::move(selected)}
    , m_options{options}
    , m_libraryApi{std::move(libraryApi)}
{
    if(service) {
        connect(service, &BandcampResolutionService::scopeResolved, this, &BandcampRefreshController::acceptScope);
        connect(service, &BandcampResolutionService::progressChanged, this, [this](const auto& progress) {
            if(!m_stopping && !m_finished && progress.operationId == m_summary.operationId) {
                emit progressChanged(progress);
            }
        });
        connect(service, &BandcampResolutionService::operationFinished, this, [this](const auto& summary) {
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

BandcampRefreshController::~BandcampRefreshController()
{
    m_finished = true;
    withdrawSource();
    if(m_gateway) {
        m_gateway->cancel();
    }
}

void BandcampRefreshController::start(qint64 nowMs)
{
    if(m_started || m_finished) {
        return;
    }
    m_started = true;
    const QPointer<BandcampRefreshController> guard{this};
    QList<BandcampPageUrl> pages;
    QList<QPair<qint64, BandcampRefreshOutcome>> immediate;
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
        const auto input = readBandcampPersistedTransport(track, id);
        if(const auto* error = std::get_if<BandcampTransportFailure>(&input)) {
            immediate.append({id, BandcampRefreshFailure{BandcampRefreshError::InvalidPersistedState,
                                                         error->reason}});
        }
        else {
            const auto& state = std::get<BandcampPersistedTransport>(input);
            if(!bandcampRefreshNeeded(state.expiry, nowMs, m_options.prePlaybackRefreshWindowMs)) {
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
    const BandcampSubmission submission = m_service->submit(pages, m_options);
    if(!guard) {
        return;
    }
    if(const auto* accepted = std::get_if<BandcampOperationAccepted>(&submission)) {
        m_summary.operationId = accepted->id;
        if(m_stopping) {
            m_sourceFinished = false;
            withdrawSource();
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

void BandcampRefreshController::acceptScope(const BandcampScopeResult& result)
{
    if(m_finished || result.operationId != m_summary.operationId) {
        return;
    }
    const QPointer<BandcampRefreshController> guard{this};
    const auto ids = m_consumers.take(result.page.url);
    const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&result.outcome);
    std::optional<BandcampResolvedTrackLookup> tracks;
    if(success) {
        tracks.emplace(success->value);
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
            auto matched = tracks->find(id);
            if(!matched) {
                settle(id, BandcampRefreshFailure{BandcampRefreshError::MissingSelectedTrack,
                                                  QStringLiteral("The release no longer lists the selected track.")});
            }
            else {
                const auto expiry = bandcampStreamExpiryMs(matched->track.streamUrl);
                if(!expiry || *expiry <= QDateTime::currentMSecsSinceEpoch()) {
                    settle(id, BandcampRefreshFailure{BandcampRefreshError::UnusableRefreshedExpiry,
                                                      QStringLiteral("The refreshed stream has no usable future expiry.")});
                }
                else {
                    m_pendingWrites.enqueue(std::move(*matched));
                }
            }
        }
        if(!guard) {
            return;
        }
    }
    pump();
}

void BandcampRefreshController::pump()
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
    const QPointer<BandcampRefreshController> guard{this};
    if(!m_gateway) {
        m_gateway = new BandcampLibraryGateway{std::move(m_libraryApi), this};
    }
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

void BandcampRefreshController::settle(qint64 id, BandcampRefreshOutcome outcome, bool alreadyLogged)
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
    emit consumerFinished(id, outcome);
}

void BandcampRefreshController::withdrawSource()
{
    if(m_withdrawalIssued || m_sourceFinished || !m_summary.operationId) {
        return;
    }
    m_withdrawalIssued = true;
    const auto service = m_service;
    const auto id = m_summary.operationId;
    if(service) {
        service->cancel(id);
    }
}

void BandcampRefreshController::cancel()
{
    if(m_finished || m_stopping) {
        return;
    }
    m_stopping = true;
    const QPointer<BandcampRefreshController> guard{this};
    withdrawSource();
    if(!guard) {
        return;
    }
    if(m_gateway) {
        m_gateway->cancel();
    }
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

void BandcampRefreshController::failPending(BandcampRefreshFailure error)
{
    const auto consumers = std::exchange(m_consumers, {});
    const QPointer<BandcampRefreshController> guard{this};
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

void BandcampRefreshController::stopWithFailure(BandcampRefreshFailure error)
{
    qCWarning(BANDCAMP_TRACKS).noquote()
        << formatTraceEvent(correlation(m_summary.operationId), QStringLiteral("refresh.operation.failed"),
                            {DiagnosticField::number(QStringLiteral("code"), static_cast<qint64>(error.code)),
                             DiagnosticField::text(QStringLiteral("reason"), error.reason)});
    m_stopping = true;
    m_consumers.clear();
    m_pendingWrites.clear();
    const QPointer<BandcampRefreshController> guard{this};
    withdrawSource();
    if(!guard) {
        return;
    }
    if(m_gateway) {
        m_gateway->cancel();
    }
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

void BandcampRefreshController::finishIfReady()
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
    emit finished(summary);
}
} // namespace Fooyin::Bandcamp
