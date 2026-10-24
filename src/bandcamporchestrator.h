#pragma once

#include "bandcampmodel.h"
#include "bandcampparser.h"
#include "bandcampresolution.h"
#include "bandcampurl.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace Fooyin::Bandcamp {
using BandcampOperationId       = quint64;
using BandcampResolutionOutcome = ResolutionOutcome<BandcampResolvedData>;

struct BandcampReleaseRequest
{
    QString key;
    BandcampPageUrl page;
};

struct BandcampAssociatedOutcome
{
    qsizetype ordinal{0};
    BandcampPageUrl page;
    BandcampResolutionOutcome outcome;
};

class BandcampOrchestrator
{
public:
    explicit BandcampOrchestrator(const QList<BandcampPageUrl>& pages,
                                  BandcampDiscographyOrder order = BandcampDiscographyOrder::Default);

    [[nodiscard]] const QVector<BandcampReleaseRequest>& requests() const;
    [[nodiscard]] qsizetype occurrenceCount() const;
    [[nodiscard]] qsizetype deliveredCount() const;
    [[nodiscard]] bool isDiscoveryComplete() const;
    [[nodiscard]] bool isComplete() const;

    void appendListingBatch(qsizetype inputOrdinal, const BandcampListingBatch& batch);
    QVector<BandcampAssociatedOutcome> completeDiscography(qsizetype inputOrdinal);
    QVector<BandcampAssociatedOutcome> failDiscography(qsizetype inputOrdinal,
                                                       const BandcampResolutionOutcome& outcome);
    QVector<BandcampAssociatedOutcome> associate(const QString& key, const BandcampResolutionOutcome& outcome);
    QVector<BandcampAssociatedOutcome> cancelPending();

private:
    struct Occurrence
    {
        QString key;
        BandcampPageUrl page;
        qint64 expectedId{0};
        std::optional<BandcampResolutionOutcome> immediateOutcome;
    };

    struct Group
    {
        BandcampPageUrl inputPage;
        QVector<Occurrence> occurrences;
        QVector<qsizetype> outputOrder;
        QSet<QString> pendingKeys;
        qsizetype nextOutput{0};
        bool discography{false};
        bool discoveryComplete{false};
        bool orderPrepared{false};
    };

    static QString requestKey(const BandcampPageUrl& page);
    QString addRequest(const BandcampPageUrl& page);
    void registerRequest(qsizetype groupIndex, const QString& key);
    void recordOutcome(const QString& key, const BandcampResolutionOutcome& outcome);
    const BandcampResolutionOutcome* outcomeRef(const Occurrence& occurrence) const;
    std::optional<BandcampResolutionOutcome> outcomeFor(const Occurrence& occurrence) const;
    BandcampResolutionOutcome validateListingIdentity(const Occurrence& occurrence,
                                                      const BandcampResolutionOutcome& outcome) const;
    QDate releaseDate(const BandcampResolutionOutcome& outcome) const;
    QDate releaseDate(const Occurrence& occurrence) const;
    QVector<BandcampAssociatedOutcome> drain();

    QVector<BandcampReleaseRequest> m_requests;
    QVector<Group> m_groups;
    QSet<QString> m_requestKeys;
    QHash<QString, BandcampResolutionOutcome> m_outcomes;
    QHash<QString, QVector<qsizetype>> m_requestGroups;
    BandcampDiscographyOrder m_order{BandcampDiscographyOrder::Default};
    qsizetype m_occurrenceCount{0};
    qsizetype m_pendingDiscoveries{0};
    qsizetype m_nextGroup{0};
    qsizetype m_delivered{0};
};
} // namespace Fooyin::Bandcamp
