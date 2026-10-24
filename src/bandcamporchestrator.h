#pragma once

#include "bandcampmodel.h"
#include "bandcampresolution.h"
#include "bandcampurl.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>
#include <QtGlobal>

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
    explicit BandcampOrchestrator(const QList<BandcampPageUrl>& pages);

    [[nodiscard]] const QVector<BandcampReleaseRequest>& requests() const;
    [[nodiscard]] qsizetype occurrenceCount() const;
    [[nodiscard]] qsizetype deliveredCount() const;
    [[nodiscard]] bool isComplete() const;

    QVector<BandcampAssociatedOutcome> associate(const QString& key, const BandcampResolutionOutcome& outcome);

private:
    struct Occurrence
    {
        QString key;
        BandcampPageUrl page;
    };

    static QString requestKey(const BandcampPageUrl& page);

    QVector<BandcampReleaseRequest> m_requests;
    QVector<Occurrence> m_occurrences;
    QSet<QString> m_requestKeys;
    QHash<QString, BandcampResolutionOutcome> m_outcomes;
    qsizetype m_nextOccurrence{0};
};
} // namespace Fooyin::Bandcamp
