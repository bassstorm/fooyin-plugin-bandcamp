#pragma once

#include "bandcamporchestrator.h"
#include "bandcampparser.h"

#include <QByteArray>
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

class BandcampReleaseResolver : public QObject
{
    Q_OBJECT

public:
    // The caller supplies album or track requests with non-empty, unique keys.
    BandcampReleaseResolver(QNetworkAccessManager* network, BandcampOperationId operationId,
                            QVector<BandcampReleaseRequest> requests, OperationOptions options,
                            BandcampPageParser parser, std::shared_ptr<BandcampWorkflowSlots> workflowSlots,
                            QObject* parent = nullptr);
    ~BandcampReleaseResolver() override;

    void start();
    void cancel();
    [[nodiscard]] bool isComplete() const;
    [[nodiscard]] qsizetype completedOutcomeCount() const;

signals:
    void keyedOutcomeReady(const QString& key, const Fooyin::Bandcamp::BandcampResolutionOutcome& outcome);
    void activeCountChanged(qsizetype active);
    void completed();
    void physicallyQuiescent();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Fooyin::Bandcamp
