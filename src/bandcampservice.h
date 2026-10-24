#pragma once

#include "bandcamporchestrator.h"
#include "bandcampresolver.h"

#include <QList>
#include <QObject>

#include <memory>
#include <variant>

class QNetworkAccessManager;

namespace Fooyin::Bandcamp {
struct BandcampOperationAccepted
{
    BandcampOperationId id{0};
};

struct BandcampSubmissionRejected
{
    ResolutionError error;
};

using BandcampSubmission = std::variant<BandcampOperationAccepted, BandcampSubmissionRejected>;

struct BandcampScopeResult
{
    BandcampOperationId operationId{0};
    qsizetype ordinal{0};
    BandcampPageUrl page;
    BandcampResolutionOutcome outcome;
};

struct BandcampOperationProgress
{
    BandcampOperationId operationId{0};
    qsizetype requested{0};
    qsizetype delivered{0};
    qsizetype active{0};
};

struct BandcampOperationSummary
{
    BandcampOperationId operationId{0};
    qsizetype requested{0};
    qsizetype succeeded{0};
    qsizetype failed{0};
    qsizetype cancelled{0};
};

struct BandcampServiceDependencies
{
    BandcampPageParser parseReleasePage;
    BandcampListingParser parseDiscographyPage;
};

class BandcampService : public QObject
{
    Q_OBJECT

public:
    explicit BandcampService(QNetworkAccessManager* network, QObject* parent = nullptr,
                             BandcampServiceDependencies dependencies = {});
    ~BandcampService() override;

    BandcampSubmission submit(const QList<BandcampPageUrl>& pages, OperationOptions options = {});
    void cancel(BandcampOperationId id);

signals:
    void scopeResolved(const Fooyin::Bandcamp::BandcampScopeResult& result);
    void progressChanged(const Fooyin::Bandcamp::BandcampOperationProgress& progress);
    void operationFinished(const Fooyin::Bandcamp::BandcampOperationSummary& summary);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Fooyin::Bandcamp
