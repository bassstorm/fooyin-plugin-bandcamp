#pragma once

#include "bandcamporchestrator.h"
#include "bandcampparser.h"

#include <QObject>

#include <atomic>
#include <functional>
#include <memory>

class QNetworkAccessManager;

namespace Fooyin::Bandcamp {
using BandcampListingParser = std::function<BandcampListingParseOutcome(const QByteArray&, const QUrl&,
                                                                        CorrelationContext, const std::atomic_bool*)>;

class BandcampListingResolver : public QObject
{
    Q_OBJECT

public:
    using OutcomeCallback = std::function<void(BandcampListingParseOutcome)>;

    BandcampListingResolver(QNetworkAccessManager* network, BandcampOperationId operationId, QUrl pageUrl,
                            int timeoutMs, BandcampListingParser parser,
                            std::shared_ptr<BandcampWorkflowSlots> workflowSlots, OutcomeCallback callback,
                            QObject* parent = nullptr);
    ~BandcampListingResolver() override;

    void start();
    void cancel();

signals:
    void physicallyQuiescent();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Fooyin::Bandcamp
