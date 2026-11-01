#pragma once

#include "bandcamporchestrator.h"
#include "bandcampparser.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QUrl>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace Fooyin::Bandcamp {
using BandcampPageParser
    = std::function<ReleaseParseOutcome(const QByteArray&, const QUrl&, CorrelationContext, const std::atomic_bool*)>;
using BandcampListingParser = std::function<BandcampListingParseOutcome(const QByteArray&, const QUrl&,
                                                                        CorrelationContext, const std::atomic_bool*)>;

class BandcampWorkflowSlots : public QObject
{
    Q_OBJECT

public:
    explicit BandcampWorkflowSlots(QObject* parent = nullptr);

    [[nodiscard]] bool tryAcquire();
    void release();

signals:
    void capacityAvailable();

private:
    int m_active{0};
};

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

class BandcampResolver : public QObject
{
    Q_OBJECT

public:
    // Request keys must be non-empty and unique within this list; invalid lists are rejected by start().
    BandcampResolver(QNetworkAccessManager* network, BandcampOperationId operationId,
                     QVector<BandcampReleaseRequest> requests, OperationOptions options, BandcampPageParser parser,
                     std::shared_ptr<BandcampWorkflowSlots> workflowSlots, QObject* parent = nullptr);
    ~BandcampResolver() override;

    [[nodiscard]] std::optional<ResolutionError> start();
    void cancel();
    [[nodiscard]] bool isComplete() const;
    [[nodiscard]] qsizetype completedOutcomeCount() const;

signals:
    void keyedOutcomeReady(const QString& key, const Fooyin::Bandcamp::BandcampResolutionOutcome& outcome);
    void activeCountChanged(qsizetype active);
    void completed(const QHash<QString, Fooyin::Bandcamp::BandcampResolutionOutcome>& outcomes);
    void physicallyQuiescent();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Fooyin::Bandcamp
