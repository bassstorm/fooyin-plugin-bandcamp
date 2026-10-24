#include "bandcamporchestrator.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
qint64 resolvedIdentity(const BandcampResolvedData& data)
{
    return std::visit(
        [](const auto& value) -> qint64 {
            using T = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T, BandcampAlbum>) {
                return value.metadata.id;
            }
            else {
                return value.track.id;
            }
        },
        data);
}
} // namespace

BandcampOrchestrator::BandcampOrchestrator(const QList<BandcampPageUrl>& pages, BandcampDiscographyOrder order)
    : m_order{order}
    , m_occurrenceCount{pages.size()}
{
    m_groups.reserve(pages.size());
    m_requests.reserve(pages.size());
    m_requestKeys.reserve(pages.size());

    for(const BandcampPageUrl& page : pages) {
        Group group;
        group.inputPage         = page;
        group.discography       = page.kind == BandcampPageKind::Discography;
        group.discoveryComplete = !group.discography;
        if(group.discography) {
            ++m_pendingDiscoveries;
        }
        QString key;
        if(!group.discography) {
            key = addRequest(page);
            group.occurrences.append({key, page, 0, std::nullopt});
        }
        m_groups.append(std::move(group));
        if(!key.isEmpty()) {
            registerRequest(m_groups.size() - 1, key);
        }
    }
}

const QVector<BandcampReleaseRequest>& BandcampOrchestrator::requests() const
{
    return m_requests;
}

qsizetype BandcampOrchestrator::occurrenceCount() const
{
    return m_occurrenceCount;
}

qsizetype BandcampOrchestrator::deliveredCount() const
{
    return m_delivered;
}

bool BandcampOrchestrator::isDiscoveryComplete() const
{
    return m_pendingDiscoveries == 0;
}

bool BandcampOrchestrator::isComplete() const
{
    return isDiscoveryComplete() && m_nextGroup == m_groups.size() && m_delivered == occurrenceCount();
}

void BandcampOrchestrator::appendListingBatch(qsizetype inputOrdinal, const BandcampListingBatch& batch)
{
    if(inputOrdinal < 0 || inputOrdinal >= m_groups.size()) {
        return;
    }
    Group& group = m_groups[inputOrdinal];
    if(!group.discography || group.discoveryComplete) {
        return;
    }
    const qsizetype previousCount = std::max<qsizetype>(1, group.occurrences.size());
    for(const BandcampListingReference& reference : batch.references) {
        const QString key = addRequest(reference.page);
        group.occurrences.append({key, reference.page, reference.id, std::nullopt});
        registerRequest(inputOrdinal, key);
    }
    m_occurrenceCount += std::max<qsizetype>(1, group.occurrences.size()) - previousCount;
}

QVector<BandcampAssociatedOutcome> BandcampOrchestrator::completeDiscography(qsizetype inputOrdinal)
{
    if(inputOrdinal < 0 || inputOrdinal >= m_groups.size()) {
        return {};
    }
    Group& group = m_groups[inputOrdinal];
    if(!group.discography || group.discoveryComplete) {
        return {};
    }
    if(group.occurrences.isEmpty()) {
        ResolutionError error{ResolutionErrorCode::NoResolvableTracks,
                              ResolutionStage::Publish,
                              QUrl(group.inputPage.url),
                              0,
                              QStringLiteral("listing"),
                              QStringLiteral("discography contained no release references")};
        group.occurrences.append(
            {{}, group.inputPage, 0, BandcampResolutionOutcome{ResolutionFailure{std::move(error)}}});
    }
    group.discoveryComplete = true;
    --m_pendingDiscoveries;
    return drain();
}

QVector<BandcampAssociatedOutcome> BandcampOrchestrator::failDiscography(qsizetype inputOrdinal,
                                                                         const BandcampResolutionOutcome& outcome)
{
    if(inputOrdinal < 0 || inputOrdinal >= m_groups.size()) {
        return {};
    }
    Group& group = m_groups[inputOrdinal];
    if(!group.discography || group.discoveryComplete) {
        return {};
    }
    m_occurrenceCount -= std::max<qsizetype>(1, group.occurrences.size()) - 1;
    group.occurrences.clear();
    group.outputOrder.clear();
    group.pendingKeys.clear();
    group.nextOutput = 0;
    group.occurrences.append({{}, group.inputPage, 0, outcome});
    group.discoveryComplete = true;
    --m_pendingDiscoveries;
    return drain();
}

QVector<BandcampAssociatedOutcome> BandcampOrchestrator::associate(const QString& key,
                                                                   const BandcampResolutionOutcome& outcome)
{
    recordOutcome(key, outcome);
    return drain();
}

QVector<BandcampAssociatedOutcome> BandcampOrchestrator::cancelPending()
{
    for(Group& group : m_groups) {
        if(group.discography && !group.discoveryComplete) {
            m_occurrenceCount -= std::max<qsizetype>(1, group.occurrences.size()) - 1;
            group.occurrences.clear();
            group.occurrences.append({{}, group.inputPage, 0, BandcampResolutionOutcome{ResolutionCancelled{}}});
            group.discoveryComplete = true;
            --m_pendingDiscoveries;
        }
    }
    for(const BandcampReleaseRequest& request : std::as_const(m_requests)) {
        recordOutcome(request.key, ResolutionCancelled{});
    }
    return drain();
}

QString BandcampOrchestrator::requestKey(const BandcampPageUrl& page)
{
    return QString::number(static_cast<unsigned int>(page.kind)) + QChar(u':') + page.url;
}

QString BandcampOrchestrator::addRequest(const BandcampPageUrl& page)
{
    const QString key = requestKey(page);
    if(!m_requestKeys.contains(key)) {
        m_requestKeys.insert(key);
        m_requests.append({key, page});
    }
    return key;
}

void BandcampOrchestrator::registerRequest(qsizetype groupIndex, const QString& key)
{
    if(groupIndex < 0 || groupIndex >= m_groups.size() || key.isEmpty() || m_outcomes.contains(key)) {
        return;
    }
    Group& group = m_groups[groupIndex];
    if(group.pendingKeys.contains(key)) {
        return;
    }
    group.pendingKeys.insert(key);
    m_requestGroups[key].append(groupIndex);
}

void BandcampOrchestrator::recordOutcome(const QString& key, const BandcampResolutionOutcome& outcome)
{
    if(!m_requestKeys.contains(key) || m_outcomes.contains(key)) {
        return;
    }
    m_outcomes.insert(key, outcome);
    const auto groups = m_requestGroups.constFind(key);
    if(groups == m_requestGroups.cend()) {
        return;
    }
    for(const qsizetype groupIndex : groups.value()) {
        if(groupIndex < 0 || groupIndex >= m_groups.size()) {
            continue;
        }
        Group& group = m_groups[groupIndex];
        group.pendingKeys.remove(key);
    }
}

const BandcampResolutionOutcome* BandcampOrchestrator::outcomeRef(const Occurrence& occurrence) const
{
    if(occurrence.immediateOutcome) {
        return &*occurrence.immediateOutcome;
    }
    const auto it = m_outcomes.constFind(occurrence.key);
    return it == m_outcomes.cend() ? nullptr : &it.value();
}

std::optional<BandcampResolutionOutcome> BandcampOrchestrator::outcomeFor(const Occurrence& occurrence) const
{
    const BandcampResolutionOutcome* outcome = outcomeRef(occurrence);
    if(!outcome) {
        return std::nullopt;
    }
    return validateListingIdentity(occurrence, *outcome);
}

BandcampResolutionOutcome BandcampOrchestrator::validateListingIdentity(const Occurrence& occurrence,
                                                                        const BandcampResolutionOutcome& outcome) const
{
    if(occurrence.expectedId <= 0 || !std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(outcome)) {
        return outcome;
    }
    const auto& data = std::get<ResolutionSuccess<BandcampResolvedData>>(outcome).value;
    if(resolvedIdentity(data) == occurrence.expectedId) {
        return outcome;
    }
    return ResolutionFailure{ResolutionError{
        ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Publish, QUrl(occurrence.page.url),
        occurrence.page.kind == BandcampPageKind::Track ? occurrence.expectedId : 0, QStringLiteral("listing.itemId"),
        QStringLiteral("resolved release identity disagrees with its listing ID")}};
}

QDate BandcampOrchestrator::releaseDate(const BandcampResolutionOutcome& outcome) const
{
    const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&outcome);
    if(!success) {
        return {};
    }
    return std::visit(
        [](const auto& value) -> QDate {
            using T = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T, BandcampAlbum>) {
                return value.metadata.releaseDate;
            }
            else {
                return value.album ? value.album->releaseDate : value.track.releaseDate;
            }
        },
        success->value);
}

QDate BandcampOrchestrator::releaseDate(const Occurrence& occurrence) const
{
    const BandcampResolutionOutcome* outcome = outcomeRef(occurrence);
    if(!outcome
       || (occurrence.expectedId > 0 && std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(*outcome)
           && resolvedIdentity(std::get<ResolutionSuccess<BandcampResolvedData>>(*outcome).value)
                  != occurrence.expectedId)) {
        return {};
    }
    return releaseDate(*outcome);
}

QVector<BandcampAssociatedOutcome> BandcampOrchestrator::drain()
{
    QVector<BandcampAssociatedOutcome> ready;
    while(m_nextGroup < m_groups.size()) {
        Group& group = m_groups[m_nextGroup];
        if(!group.discoveryComplete) {
            break;
        }
        if(group.occurrences.isEmpty()) {
            ++m_nextGroup;
            continue;
        }

        const bool chronological
            = group.discography
           && (m_order == BandcampDiscographyOrder::Ascending || m_order == BandcampDiscographyOrder::Descending);
        if(chronological && !group.orderPrepared) {
            if(!group.pendingKeys.isEmpty()) {
                break;
            }
            group.outputOrder.reserve(group.occurrences.size());
            for(qsizetype i = 0; i < group.occurrences.size(); ++i) {
                group.outputOrder.append(i);
            }
            QVector<QDate> dates;
            dates.reserve(group.occurrences.size());
            for(const Occurrence& occurrence : std::as_const(group.occurrences)) {
                dates.append(releaseDate(occurrence));
            }
            std::stable_sort(group.outputOrder.begin(), group.outputOrder.end(),
                             [this, &dates](qsizetype left, qsizetype right) {
                                 const QDate leftDate  = dates.at(left);
                                 const QDate rightDate = dates.at(right);
                                 if(leftDate.isValid() != rightDate.isValid()) {
                                     return leftDate.isValid();
                                 }
                                 if(leftDate.isValid() && leftDate != rightDate) {
                                     return m_order == BandcampDiscographyOrder::Ascending ? leftDate < rightDate
                                                                                           : leftDate > rightDate;
                                 }
                                 return left < right;
                             });
            group.orderPrepared = true;
        }
        else if(!group.orderPrepared) {
            group.outputOrder.reserve(group.occurrences.size());
            for(qsizetype i = 0; i < group.occurrences.size(); ++i) {
                group.outputOrder.append(i);
            }
            group.orderPrepared = true;
        }

        while(group.nextOutput < group.outputOrder.size()) {
            const qsizetype entryIndex   = group.outputOrder.at(group.nextOutput);
            const Occurrence& occurrence = group.occurrences.at(entryIndex);
            const auto outcome           = outcomeFor(occurrence);
            if(!outcome) {
                break;
            }
            ready.append({m_delivered++, occurrence.page, *outcome});
            ++group.nextOutput;
        }
        if(group.nextOutput != group.outputOrder.size()) {
            break;
        }
        ++m_nextGroup;
    }
    return ready;
}
} // namespace Fooyin::Bandcamp
