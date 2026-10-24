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
