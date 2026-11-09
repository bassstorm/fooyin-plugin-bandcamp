#pragma once

#include "bandcamplibrarygateway.h"
#include "bandcampresolutionservice.h"

#include <QObject>
#include <QPointer>

namespace Fooyin::Bandcamp {
class BandcampPlaybackPreparation final : public QObject
{
    Q_OBJECT

public:
    BandcampPlaybackPreparation(BandcampResolutionService* service, Fooyin::Track track, OperationOptions options,
                                FooyinLibraryApi libraryApi, QObject* parent = nullptr,
                                BandcampLibraryGateway* sharedGateway = nullptr);
    ~BandcampPlaybackPreparation() override;

    void start(qint64 nowMs);
    void cancel();

signals:
    void prepared(const QString& streamUrl);
    void failed(const QString& reason);
    void finished();

private:
    void acceptScope(const BandcampScopeResult& result);
    void fail(const QString& reason, bool alreadyLogged = false);
    void cancelGatewayRequest();
    void finishIfReady();

    QPointer<BandcampResolutionService> m_service;
    Fooyin::Track m_track;
    OperationOptions m_options;
    FooyinLibraryApi m_libraryApi;
    QPointer<BandcampLibraryGateway> m_gateway;
    quint64 m_gatewayRequest{0};
    bool m_ownsGateway{false};
    BandcampOperationId m_operationId{0};
    qint64 m_trackId{0};
    bool m_started{false};
    bool m_outcomeSent{false};
    bool m_sourceFinished{false};
    bool m_writePending{false};
    bool m_withdrawalIssued{false};
    bool m_cancelled{false};
    bool m_finished{false};
};
} // namespace Fooyin::Bandcamp
