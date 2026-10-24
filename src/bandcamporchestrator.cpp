#include "bandcamporchestrator.h"

namespace Fooyin::Bandcamp {
BandcampOrchestrator::BandcampOrchestrator(const QList<BandcampPageUrl>& pages)
{
    m_occurrences.reserve(pages.size());
    m_requests.reserve(pages.size());

    m_requestKeys.reserve(pages.size());
    for(const BandcampPageUrl& page : pages) {
        const QString key = requestKey(page);
        if(!m_requestKeys.contains(key)) {
            m_requestKeys.insert(key);
            m_requests.append({key, page});
        }
        m_occurrences.append({key, page});
    }
}

const QVector<BandcampReleaseRequest>& BandcampOrchestrator::requests() const
{
    return m_requests;
}

qsizetype BandcampOrchestrator::occurrenceCount() const
{
    return m_occurrences.size();
}

qsizetype BandcampOrchestrator::deliveredCount() const
{
    return m_nextOccurrence;
}

bool BandcampOrchestrator::isComplete() const
{
    return m_outcomes.size() == m_requests.size() && m_nextOccurrence == m_occurrences.size();
}

QVector<BandcampAssociatedOutcome> BandcampOrchestrator::associate(const QString& key,
                                                                   const BandcampResolutionOutcome& outcome)
{
    if(m_requestKeys.contains(key) && !m_outcomes.contains(key)) {
        m_outcomes.insert(key, outcome);
    }

    QVector<BandcampAssociatedOutcome> ready;
    while(m_nextOccurrence < m_occurrences.size()) {
        const Occurrence& occurrence = m_occurrences.at(m_nextOccurrence);
        const auto it                = m_outcomes.constFind(occurrence.key);
        if(it == m_outcomes.cend()) {
            break;
        }
        ready.append({m_nextOccurrence, occurrence.page, it.value()});
        ++m_nextOccurrence;
    }
    return ready;
}

QString BandcampOrchestrator::requestKey(const BandcampPageUrl& page)
{
    return QString::number(static_cast<unsigned int>(page.kind)) + QChar(u':') + page.url;
}
} // namespace Fooyin::Bandcamp
