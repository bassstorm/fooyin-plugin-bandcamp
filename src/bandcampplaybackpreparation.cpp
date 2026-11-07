#include "bandcampplaybackpreparation.h"

#include "bandcamplogging.h"
#include "bandcamptrackuri.h"

#include <QDateTime>

#include <utility>

namespace Fooyin::Bandcamp {
BandcampPlaybackPreparation::BandcampPlaybackPreparation(BandcampResolutionService* service, Fooyin::Track track,
                                                         OperationOptions options, FooyinLibraryApi libraryApi,
                                                         QObject* parent, BandcampLibraryGateway* sharedGateway)
    : QObject{parent}
    , m_service{service}
    , m_track{std::move(track)}
    , m_options{options}
    , m_libraryApi{std::move(libraryApi)}
    , m_gateway{sharedGateway}
    , m_ownsGateway{!sharedGateway}
{
    if(service) {
        connect(service, &BandcampResolutionService::scopeResolved, this, &BandcampPlaybackPreparation::acceptScope);
        connect(service, &BandcampResolutionService::operationFinished, this, [this](const auto& summary) {
            if(summary.operationId != m_operationId || m_finished) {
                return;
            }
            m_sourceFinished = true;
            const QPointer<BandcampPlaybackPreparation> guard{this};
            if(!m_outcomeSent) {
                fail(QStringLiteral("The release operation did not supply the selected track."));
            }
            if(guard) {
                finishIfReady();
            }
        });
        connect(service, &QObject::destroyed, this, [this]() {
            if(m_finished || !m_started) {
                return;
            }
            m_sourceFinished = true;
            const QPointer<BandcampPlaybackPreparation> guard{this};
            if(!m_outcomeSent) {
                fail(QStringLiteral("The release service was destroyed."));
            }
            if(guard) {
                finishIfReady();
            }
        });
    }
}

BandcampPlaybackPreparation::~BandcampPlaybackPreparation()
{
    m_finished = true;
    if(m_service) {
        disconnect(m_service, nullptr, this, nullptr);
        if(m_operationId && !m_sourceFinished && !m_withdrawalIssued) {
            m_service->cancel(m_operationId);
        }
    }
    cancelGatewayRequest();
}

void BandcampPlaybackPreparation::start(qint64 nowMs)
{
    if(m_started || m_finished) {
        return;
    }
    m_started            = true;
    m_trackId            = parseBandcampTrackUri(m_track.filepath()).value_or(0);
    const auto persisted = readBandcampPersistedTransport(m_track, m_trackId);
    const QPointer<BandcampPlaybackPreparation> guard{this};
    if(const auto* error = std::get_if<BandcampTransportFailure>(&persisted)) {
        m_sourceFinished = true;
        fail(error->reason);
        if(guard) {
            finishIfReady();
        }
        return;
    }

    const auto& transport = std::get<BandcampPersistedTransport>(persisted);
    if(!bandcampRefreshNeeded(transport.expiry, nowMs, m_options.prePlaybackRefreshWindowMs)) {
        m_sourceFinished = true;
        m_outcomeSent    = true;
        emit prepared(transport.streamUrl);
        if(guard) {
            finishIfReady();
        }
        return;
    }
    if(!m_service) {
        m_sourceFinished = true;
        fail(QStringLiteral("The release service is unavailable."));
        if(guard) {
            finishIfReady();
        }
        return;
    }

    const BandcampSubmission submission = m_service->submit({transport.source}, m_options);
    if(const auto* accepted = std::get_if<BandcampOperationAccepted>(&submission)) {
        m_operationId = accepted->id;
    }
    else {
        m_sourceFinished = true;
        fail(std::get<BandcampSubmissionRejected>(submission).error.reason);
        if(guard) {
            finishIfReady();
        }
    }
}

void BandcampPlaybackPreparation::acceptScope(const BandcampScopeResult& result)
{
    if(m_finished || m_outcomeSent || result.operationId != m_operationId) {
        return;
    }
    const QPointer<BandcampPlaybackPreparation> guard{this};
    if(m_cancelled || std::holds_alternative<ResolutionCancelled>(result.outcome)) {
        fail(QStringLiteral("Playback preparation was cancelled."), true);
        return;
    }
    const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&result.outcome);
    if(!success) {
        fail(QStringLiteral("Bandcamp could not prepare a playable stream."), true);
        return;
    }

    const BandcampResolvedTrackLookup tracks{success->value};
    auto selected = tracks.find(m_trackId);
    if(!selected) {
        fail(QStringLiteral("The release no longer lists the selected track."));
        return;
    }
    const auto expiry = bandcampStreamExpiryMs(selected->track.streamUrl);
    if(!expiry || *expiry <= QDateTime::currentMSecsSinceEpoch()) {
        fail(QStringLiteral("The refreshed stream has no usable future expiry."));
        return;
    }

    m_outcomeSent = true;
    emit prepared(selected->track.streamUrl);
    if(!guard || m_cancelled || m_finished) {
        return;
    }

    m_writePending = true;
    if(!m_gateway && !m_ownsGateway) {
        qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(
            CorrelationContext{.trackId = m_trackId}, QStringLiteral("playback_transport_persistence_unavailable"), {});
        m_writePending = false;
        finishIfReady();
        return;
    }
    if(!m_gateway) {
        m_gateway = new BandcampLibraryGateway{std::move(m_libraryApi), this};
    }
    const CorrelationContext context{.albumId = selected->album ? selected->album->id : 0, .trackId = m_trackId};
    m_gatewayRequest
        = m_gateway->refreshTransport(std::move(*selected), this, context, [this](FooyinTrackCommitOutcome) {
              m_gatewayRequest = 0;
              m_writePending   = false;
              finishIfReady();
          });
}

void BandcampPlaybackPreparation::cancel()
{
    if(m_finished || m_cancelled) {
        return;
    }
    m_cancelled = true;
    if(!m_started) {
        m_started = true;
    }
    if(!m_operationId) {
        m_sourceFinished = true;
    }
    const QPointer<BandcampPlaybackPreparation> guard{this};
    if(!m_outcomeSent) {
        fail(QStringLiteral("Playback preparation was cancelled."), true);
    }
    if(!guard) {
        return;
    }
    if(m_service && m_operationId && !m_sourceFinished && !m_withdrawalIssued) {
        m_withdrawalIssued = true;
        m_service->cancel(m_operationId);
    }
    if(!guard) {
        return;
    }
    cancelGatewayRequest();
    if(guard) {
        finishIfReady();
    }
}

void BandcampPlaybackPreparation::cancelGatewayRequest()
{
    const quint64 token = std::exchange(m_gatewayRequest, 0);
    if(!m_gateway) {
        return;
    }
    if(m_ownsGateway) {
        m_gateway->cancel();
    }
    else {
        m_gateway->cancelRequest(token);
    }
}

void BandcampPlaybackPreparation::fail(const QString& reason, bool alreadyLogged)
{
    if(m_outcomeSent) {
        return;
    }
    m_outcomeSent = true;
    if(!alreadyLogged) {
        const CorrelationContext context{.trackId = m_trackId};
        qCWarning(BANDCAMP_TRACKS).noquote()
            << formatTraceEvent(context, QStringLiteral("playback_preparation_failed"),
                                {DiagnosticField::text(QStringLiteral("reason"), reason)});
    }
    emit failed(reason);
}

void BandcampPlaybackPreparation::finishIfReady()
{
    if(m_finished || !m_started || !m_outcomeSent || !m_sourceFinished || m_writePending) {
        return;
    }
    m_finished = true;
    emit finished();
}
} // namespace Fooyin::Bandcamp
