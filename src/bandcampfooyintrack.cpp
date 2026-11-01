#include "bandcampfooyintrack.h"

#include "bandcamplogging.h"
#include "bandcamptrackuri.h"

#include <QHash>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <limits>
#include <stdexcept>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
QString trackProperty(const Fooyin::Track& track, const QString& name)
{
    return track.extraProperties().value(name);
}

QString errorCodeName(FooyinTrackCommitErrorCode code)
{
    switch(code) {
        case FooyinTrackCommitErrorCode::InvalidIdentity:
            return QStringLiteral("invalid_identity");
        case FooyinTrackCommitErrorCode::Cancelled:
            return QStringLiteral("cancelled");
        case FooyinTrackCommitErrorCode::InvalidTimeout:
            return QStringLiteral("invalid_timeout");
        case FooyinTrackCommitErrorCode::LibraryUnavailable:
            return QStringLiteral("library_unavailable");
        case FooyinTrackCommitErrorCode::WrongThread:
            return QStringLiteral("wrong_thread");
        case FooyinTrackCommitErrorCode::PersistenceFailed:
            return QStringLiteral("persistence_failed");
        case FooyinTrackCommitErrorCode::HostOperationFailed:
            return QStringLiteral("host_operation_failed");
    }
    return QStringLiteral("unknown");
}

FooyinTrackCommitFailure failure(FooyinTrackCommitErrorCode code, qint64 trackId, QString reason)
{
    return {.code = code, .trackId = trackId, .reason = std::move(reason)};
}

Fooyin::Track mergeTransport(Fooyin::Track target, const Fooyin::Track& incoming)
{
    for(const QString& property : {QString::fromLatin1(StreamProviderProperty),
                                   QString::fromLatin1(BandcampSourceUrlProperty),
                                   QString::fromLatin1(BandcampStreamUrlProperty),
                                   QString::fromLatin1(BandcampExpiresAtProperty)}) {
        target.setExtraProperty(property, trackProperty(incoming, property));
    }
    return target;
}

QStringList changedTransportProperties(const Fooyin::Track& previous, const Fooyin::Track& updated)
{
    QStringList changed;
    for(const QString& property : {QString::fromLatin1(StreamProviderProperty),
                                   QString::fromLatin1(BandcampSourceUrlProperty),
                                   QString::fromLatin1(BandcampStreamUrlProperty),
                                   QString::fromLatin1(BandcampExpiresAtProperty)}) {
        if(trackProperty(previous, property) != trackProperty(updated, property)) {
            changed.append(property);
        }
    }
    return changed;
}

} // namespace

Fooyin::Track makeFooyinTrack(const BandcampResolvedTrack& resolved,
                              std::shared_ptr<Fooyin::TrackMetadataStore> metadataStore)
{
    const BandcampTrack& source = resolved.track;
    Fooyin::Track track{bandcampTrackUri(source.id), std::move(metadataStore)};
    track.setTitle(source.title);
    if(!source.artist.isEmpty()) {
        track.setArtists({source.artist});
    }
    if(resolved.album) {
        track.setAlbum(resolved.album->title);
        if(!resolved.album->artist.isEmpty()) {
            track.setAlbumArtists({resolved.album->artist});
        }
        track.setTrackTotal(QString::number(resolved.album->trackTotal));
    }
    if(source.releaseDate.isValid()) {
        track.setDate(source.releaseDate.toString(Qt::ISODate));
    }
    if(source.trackNumber != 0) {
        track.setTrackNumber(QString::number(source.trackNumber));
    }
    if(source.durationMs != 0) {
        track.setDuration(static_cast<uint64_t>(source.durationMs));
    }

    track.setMetadataWasRead(true);
    track.setExtraProperty(StreamProviderProperty, u"bandcamp"_s);
    track.setExtraProperty(BandcampSourceUrlProperty, source.sourceUrl);
    track.setExtraProperty(BandcampStreamUrlProperty, source.streamUrl);
    if(const auto expiryMs = bandcampStreamExpiryMs(source.streamUrl)) {
        track.setExtraProperty(BandcampExpiresAtProperty, QString::number(*expiryMs));
    }
    track.generateHash();
    track.clearWasModified();
    return track;
}

std::optional<qint64> bandcampStreamExpiryMs(const QString& streamUrl)
{
    const QUrl url{streamUrl, QUrl::StrictMode};
    static const QRegularExpression bandcampStreamHost{u"^t[0-9]+\\.bcbits\\.com$"_s};
    const QString path = url.path(QUrl::FullyDecoded);
    if(!url.host().contains(bandcampStreamHost) || !path.startsWith(u"/stream/"_s) || path.size() <= qsizetype{8}
       || path.at(8) == u'/') {
        return std::nullopt;
    }

    const QUrlQuery query{url};
    std::optional<QString> timestamp;
    for(const auto& [key, value] : query.queryItems(QUrl::FullyDecoded)) {
        if(key != u"ts"_s) {
            continue;
        }
        if(timestamp) {
            return std::nullopt;
        }
        timestamp = value;
    }
    if(!timestamp || timestamp->isEmpty()) {
        return std::nullopt;
    }
    for(const QChar ch : *timestamp) {
        if(ch < u'0' || ch > u'9') {
            return std::nullopt;
        }
    }

    bool ok = false;
    const qint64 seconds = timestamp->toLongLong(&ok);
    constexpr qint64 MaxSeconds = std::numeric_limits<qint64>::max() / 1000;
    if(!ok || seconds <= 0 || seconds > MaxSeconds) {
        return std::nullopt;
    }
    return seconds * 1000;
}

FooyinLibraryApi fooyinLibraryApi(Fooyin::MusicLibrary* library)
{
    FooyinLibraryApi api;
    api.owner = library;
    if(!library) {
        return api;
    }

    const QPointer<Fooyin::MusicLibrary> guardedLibrary{library};
    api.tracks = [guardedLibrary]() -> Fooyin::TrackList {
        if(!guardedLibrary) {
            throw std::runtime_error{"The music library was destroyed."};
        }
        return guardedLibrary->tracks();
    };
    api.metadataStore = [guardedLibrary]() -> std::shared_ptr<Fooyin::TrackMetadataStore> {
        if(!guardedLibrary) {
            throw std::runtime_error{"The music library was destroyed."};
        }
        return guardedLibrary->metadataStore();
    };
    api.writeTrackMetadata = [guardedLibrary](const Fooyin::TrackList& tracks) {
        if(!guardedLibrary) {
            throw std::runtime_error{"The music library was destroyed."};
        }
        return guardedLibrary->writeTrackMetadata(tracks);
    };
    return api;
}

BandcampLibraryGateway::BandcampLibraryGateway(FooyinLibraryApi api, QObject* parent)
    : QObject{parent}
    , m_api{std::move(api)}
{ }

BandcampLibraryGateway::~BandcampLibraryGateway()
{
    cancel();
}

void BandcampLibraryGateway::cancel()
{
    if(m_cancelled) {
        return;
    }
    m_cancelled = true;
    while(!m_pending.isEmpty()) {
        QObject::disconnect(m_pending.dequeue().receiverDestroyed);
    }
    if(!m_active) {
        return;
    }

    m_active->withdrawn = true;
    m_active->request.callbackContext.clear();
    m_active->request.completion = {};
    if(m_active->cancelWrite) {
        try {
            m_active->cancelWrite();
        }
        catch(...) {
            qCWarning(BANDCAMP_TRACKS).noquote()
                << formatTraceEvent(m_active->request.correlation, QStringLiteral("track.write.cancel_failed"),
                                    {DiagnosticField::text(QStringLiteral("reason"),
                                                           QStringLiteral("submission cancellation threw"))});
        }
    }
    if(!m_active->watcher) {
        abandonActive();
    }
}

void BandcampLibraryGateway::commit(QList<BandcampResolvedTrack> incoming, int activeTimeoutMs,
                                    QObject* callbackContext,
                                    CorrelationContext correlation, Completion completion)
{
    if(!callbackContext || !completion) {
        return;
    }
    if(m_cancelled) {
        const auto error = failure(FooyinTrackCommitErrorCode::Cancelled, 0,
                                   QStringLiteral("The library submission was withdrawn."));
        logFailure(correlation, error);
        deliver(callbackContext, std::move(completion), error, correlation);
        return;
    }

    if(!m_api.owner) {
        const auto error = failure(FooyinTrackCommitErrorCode::LibraryUnavailable, 0,
                                   QStringLiteral("The music library is unavailable."));
        logFailure(correlation, error);
        deliver(callbackContext, std::move(completion), error, correlation);
        return;
    }

    if(QThread::currentThread() != thread() || callbackContext->thread() != thread()
       || m_api.owner->thread() != thread()) {
        const auto error = failure(FooyinTrackCommitErrorCode::WrongThread, 0,
                                   QStringLiteral("The library submission must run on its owning thread."));
        logFailure(correlation, error);
        deliver(callbackContext, std::move(completion), error, correlation);
        return;
    }
    if(activeTimeoutMs <= 0) {
        const auto error = failure(FooyinTrackCommitErrorCode::InvalidTimeout, 0,
                                   QStringLiteral("The active library-write timeout must be positive."));
        logFailure(correlation, error);
        deliver(callbackContext, std::move(completion), error, correlation);
        return;
    }

    CommitRequest request{m_nextToken++, std::move(incoming), activeTimeoutMs,
                          callbackContext, std::move(correlation), std::move(completion), {}};
    const quint64 token = request.token;
    request.receiverDestroyed = QObject::connect(callbackContext, &QObject::destroyed, this,
                                                 [this, token]() { withdraw(token); });
    if(m_active || !m_pending.isEmpty() || m_nextScheduled) {
        if(BANDCAMP_TRACE_TRACKS().isDebugEnabled()) {
            qCDebug(BANDCAMP_TRACE_TRACKS).noquote()
                << formatTraceEvent(request.correlation, QStringLiteral("track.commit.queued"),
                                    {DiagnosticField::number(QStringLiteral("occurrences"),
                                                             static_cast<qint64>(request.incoming.size()))});
        }
    }
    m_pending.enqueue(std::move(request));
    scheduleNext();
}

void BandcampLibraryGateway::scheduleNext()
{
    if(m_nextScheduled || m_active || m_pending.isEmpty()) {
        return;
    }
    m_nextScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_nextScheduled = false;
        startNext();
    });
}

void BandcampLibraryGateway::startNext()
{
    if(m_active || m_pending.isEmpty()) {
        return;
    }

    CommitRequest request = m_pending.dequeue();
    if(!request.callbackContext) {
        QObject::disconnect(request.receiverDestroyed);
        scheduleNext();
        return;
    }
    m_active.emplace();
    m_active->request = std::move(request);
    start(m_active->request);
}

void BandcampLibraryGateway::start(const CommitRequest& request)
{
    const quint64 token = request.token;
    if(!request.callbackContext) {
        abandonActive();
        return;
    }
    m_active->deadline.start();
    QTimer::singleShot(request.activeTimeoutMs, this, [this, token]() { onTimeout(token); });
    if(!m_api.owner || !m_api.tracks || !m_api.metadataStore || !m_api.writeTrackMetadata) {
        failActive(failure(FooyinTrackCommitErrorCode::LibraryUnavailable, 0,
                           QStringLiteral("The public library persistence interface is unavailable.")));
        return;
    }

    QSet<qint64> requestedIds;
    for(const BandcampResolvedTrack& incoming : request.incoming) {
        requestedIds.insert(incoming.track.id);
    }

    Fooyin::TrackList residentSnapshot;
    std::shared_ptr<Fooyin::TrackMetadataStore> metadataStore;
    try {
        residentSnapshot = m_api.tracks();
        metadataStore    = m_api.metadataStore();
    }
    catch(...) {
        failActive(failure(FooyinTrackCommitErrorCode::HostOperationFailed, 0,
                           QStringLiteral("The library snapshot could not be read.")));
        return;
    }

    QHash<qint64, Fooyin::Track> residentsByTrackId;
    for(const Fooyin::Track& resident : residentSnapshot) {
        const auto id = parseBandcampTrackUri(resident.filepath());
        if(!id || !requestedIds.contains(*id)) {
            continue;
        }

        if(resident.hasExtraProperty(BandcampTrackIdProperty)) {
            if(trackProperty(resident, BandcampTrackIdProperty) != QString::number(*id)) {
                failActive(failure(FooyinTrackCommitErrorCode::InvalidIdentity, *id,
                                   QStringLiteral("A resident track ID mirror conflicts with its URI.")));
                return;
            }
        }
        residentsByTrackId.insert(*id, resident);
    }

    QSet<int> writeTargetIds;
    m_active->output.reserve(request.incoming.size());
    m_active->writeTargets.reserve(request.incoming.size());
    QSet<qint64> newIdentityIds;
    QSet<qint64> loggedTrackIds;

    for(const BandcampResolvedTrack& resolved : request.incoming) {
        const qint64 id = resolved.track.id;
        const Fooyin::Track incoming = makeFooyinTrack(resolved, metadataStore);

        const auto resident = residentsByTrackId.constFind(id);
        const Fooyin::Track previous = resident == residentsByTrackId.cend() ? incoming : resident.value();
        Fooyin::Track updated = mergeTransport(previous, incoming);
        if(previous.id() <= 0) {
            if(updated.hash().isEmpty()) {
                updated.generateHash();
            }
            newIdentityIds.insert(id);
        }

        if(resident != residentsByTrackId.cend() && previous.id() > 0
           && !changedTransportProperties(previous, updated).isEmpty()
           && !writeTargetIds.contains(previous.id())) {
            writeTargetIds.insert(previous.id());
            m_active->writeTargets.push_back(updated);
        }

        if(resident != residentsByTrackId.cend() && !loggedTrackIds.contains(id)) {
            loggedTrackIds.insert(id);
            CorrelationContext trackCorrelation = request.correlation.withTrack(id);
            trackCorrelation.resourceUrl = QUrl{trackProperty(updated, BandcampSourceUrlProperty)};
            traceTransportChange(trackCorrelation, previous, updated);
        }
        m_active->output.push_back(std::move(updated));
    }
    m_active->newIdentityCount = static_cast<qsizetype>(newIdentityIds.size());

    if(m_active->deadline.elapsed() >= request.activeTimeoutMs) {
        onTimeout(token);
        return;
    }

    if(BANDCAMP_TRACE_TRACKS().isDebugEnabled()) {
        qCDebug(BANDCAMP_TRACE_TRACKS).noquote()
            << formatTraceEvent(request.correlation, QStringLiteral("track.commit.started"),
                                {DiagnosticField::number(QStringLiteral("occurrences"),
                                                         static_cast<qint64>(request.incoming.size())),
                                 DiagnosticField::number(QStringLiteral("write_targets"),
                                                         static_cast<qint64>(m_active->writeTargets.size()))});
    }

    if(m_active->writeTargets.empty()) {
        FooyinTrackCommitSuccess result{std::move(m_active->output), 0, m_active->newIdentityCount};
        qCInfo(BANDCAMP_TRACKS).noquote()
            << formatTraceEvent(request.correlation, QStringLiteral("track.commit.completed"),
                                {DiagnosticField::number(QStringLiteral("occurrences"),
                                                         static_cast<qint64>(result.tracks.size())),
                                 DiagnosticField::number(QStringLiteral("updated_residents"), 0),
                                 DiagnosticField::number(QStringLiteral("new_idless"),
                                                         static_cast<qint64>(result.newIdentityCount))});
        finishActive(std::move(result));
        return;
    }

    Fooyin::WriteRequest writeRequest;
    try {
        writeRequest = m_api.writeTrackMetadata(m_active->writeTargets);
    }
    catch(...) {
        failActive(failure(FooyinTrackCommitErrorCode::HostOperationFailed, 0,
                           QStringLiteral("The library rejected the track write.")));
        return;
    }
    if(!writeRequest.finished.isValid()) {
        failActive(failure(FooyinTrackCommitErrorCode::PersistenceFailed, 0,
                           QStringLiteral("The library returned an invalid write request.")));
        return;
    }

    m_active->cancelWrite = std::move(writeRequest.cancel);
    auto* watcher = new QFutureWatcher<Fooyin::WriteResult>(this);
    m_active->watcher = watcher;
    QObject::connect(watcher, &QFutureWatcher<Fooyin::WriteResult>::finished, this,
                     [this, token]() { onWriteFinished(token); });
    watcher->setFuture(writeRequest.finished);
    if(BANDCAMP_TRACE_TRACKS().isDebugEnabled()) {
        qCDebug(BANDCAMP_TRACE_TRACKS).noquote()
            << formatTraceEvent(request.correlation, QStringLiteral("track.write.started"),
                                {DiagnosticField::number(QStringLiteral("targets"),
                                                         static_cast<qint64>(m_active->writeTargets.size()))});
    }
    if(m_active->deadline.elapsed() >= request.activeTimeoutMs) {
        onTimeout(token);
    }
}

void BandcampLibraryGateway::onWriteFinished(quint64 token)
{
    if(!m_active || m_active->request.token != token || !m_active->watcher) {
        return;
    }
    if(!m_active->timedOut && m_active->deadline.elapsed() >= m_active->request.activeTimeoutMs) {
        onTimeout(token);
    }
    if(!m_active || m_active->request.token != token) {
        return;
    }
    if(m_active->timedOut || m_active->withdrawn || !m_active->request.callbackContext) {
        abandonActive();
        return;
    }
    if(!m_api.owner) {
        failActive(failure(FooyinTrackCommitErrorCode::LibraryUnavailable, 0,
                           QStringLiteral("The music library was destroyed during persistence.")));
        return;
    }

    try {
        const QFuture<Fooyin::WriteResult> future = m_active->watcher->future();
        if(future.isCanceled() || future.resultCount() != 1) {
            failActive(failure(FooyinTrackCommitErrorCode::PersistenceFailed, 0,
                               QStringLiteral("The library write was cancelled or returned no single result.")));
            return;
        }
        const Fooyin::WriteResult result = future.result();
        const qsizetype expected         = m_active->writeTargets.size();
        if(result.state != Fooyin::WriteState::Completed || result.failed != 0 || result.succeeded != expected) {
            failActive(failure(FooyinTrackCommitErrorCode::PersistenceFailed, 0,
                               QStringLiteral("The library did not complete every requested track write.")));
            return;
        }

        FooyinTrackCommitSuccess success{std::move(m_active->output), expected, m_active->newIdentityCount};
        if(BANDCAMP_TRACE_TRACKS().isDebugEnabled()) {
            qCDebug(BANDCAMP_TRACE_TRACKS).noquote()
                << formatTraceEvent(m_active->request.correlation, QStringLiteral("track.write.completed"),
                                    {DiagnosticField::number(QStringLiteral("targets"), expected)});
        }
        qCInfo(BANDCAMP_TRACKS).noquote()
            << formatTraceEvent(m_active->request.correlation, QStringLiteral("track.commit.completed"),
                                {DiagnosticField::number(QStringLiteral("occurrences"),
                                                         static_cast<qint64>(success.tracks.size())),
                                 DiagnosticField::number(QStringLiteral("updated_residents"), expected),
                                 DiagnosticField::number(QStringLiteral("new_idless"),
                                                         static_cast<qint64>(success.newIdentityCount))});
        finishActive(std::move(success));
    }
    catch(...) {
        failActive(failure(FooyinTrackCommitErrorCode::PersistenceFailed, 0,
                           QStringLiteral("The library write result could not be read.")));
    }
}

void BandcampLibraryGateway::onTimeout(quint64 token)
{
    if(!m_active || m_active->request.token != token || m_active->timedOut) {
        return;
    }

    m_active->timedOut = true;
    if(m_active->cancelWrite) {
        try {
            m_active->cancelWrite();
        }
        catch(...) {
            qCWarning(BANDCAMP_TRACKS).noquote()
                << formatTraceEvent(m_active->request.correlation, QStringLiteral("track.write.cancel_failed"),
                                    {DiagnosticField::text(QStringLiteral("reason"),
                                                           QStringLiteral("timeout cancellation threw"))});
        }
    }
    FooyinTrackCommitFailure error = failure(FooyinTrackCommitErrorCode::PersistenceFailed, 0,
                                             QStringLiteral("The active library write timed out."));
    if(!m_active->watcher) {
        failActive(std::move(error));
        return;
    }
    logFailure(m_active->request.correlation, error);
    if(!m_active->callbackDelivered && m_active->request.callbackContext) {
        m_active->callbackDelivered = true;
        deliver(m_active->request.callbackContext, std::move(m_active->request.completion), error,
                m_active->request.correlation);
    }
}

void BandcampLibraryGateway::withdraw(quint64 token)
{
    if(m_active && m_active->request.token == token) {
        m_active->withdrawn = true;
        m_active->request.callbackContext.clear();
        m_active->request.completion = {};
        if(m_active->cancelWrite) {
            try {
                m_active->cancelWrite();
            }
            catch(...) {
                qCWarning(BANDCAMP_TRACKS).noquote()
                    << formatTraceEvent(m_active->request.correlation, QStringLiteral("track.write.cancel_failed"),
                                        {DiagnosticField::text(QStringLiteral("reason"),
                                                               QStringLiteral("withdrawal cancellation threw"))});
            }
        }
        if(!m_active->watcher) {
            abandonActive();
        }
        return;
    }

    QQueue<CommitRequest> retained;
    while(!m_pending.isEmpty()) {
        CommitRequest request = m_pending.dequeue();
        if(request.token == token) {
            QObject::disconnect(request.receiverDestroyed);
        }
        else {
            retained.enqueue(std::move(request));
        }
    }
    m_pending = std::move(retained);
}

void BandcampLibraryGateway::finishActive(FooyinTrackCommitOutcome outcome)
{
    if(!m_active) {
        return;
    }

    ActiveCommit active = std::move(*m_active);
    m_active.reset();
    QObject::disconnect(active.request.receiverDestroyed);
    if(active.watcher) {
        active.watcher->disconnect(this);
        active.watcher->deleteLater();
    }
    if(const auto* error = std::get_if<FooyinTrackCommitFailure>(&outcome)) {
        logFailure(active.request.correlation, *error);
    }
    if(!active.callbackDelivered && !active.withdrawn && active.request.callbackContext) {
        deliver(active.request.callbackContext, std::move(active.request.completion), std::move(outcome),
                active.request.correlation);
    }
    scheduleNext();
}

void BandcampLibraryGateway::failActive(FooyinTrackCommitFailure error)
{
    if(!m_active) {
        return;
    }
    finishActive(std::move(error));
}

void BandcampLibraryGateway::abandonActive()
{
    if(!m_active) {
        return;
    }
    ActiveCommit active = std::move(*m_active);
    m_active.reset();
    QObject::disconnect(active.request.receiverDestroyed);
    if(active.watcher) {
        active.watcher->disconnect(this);
        active.watcher->deleteLater();
    }
    scheduleNext();
}

void BandcampLibraryGateway::deliver(QObject* context, Completion completion, FooyinTrackCommitOutcome outcome,
                                     const CorrelationContext& correlation) const
{
    QPointer<QObject> receiver{context};
    if(!receiver || !completion) {
        return;
    }

    const bool queued = QMetaObject::invokeMethod(
        receiver, [receiver, completion = std::move(completion), outcome = std::move(outcome), correlation]() mutable {
            if(!receiver) {
                return;
            }
            try {
                completion(std::move(outcome));
            }
            catch(...) {
                qCWarning(BANDCAMP_TRACKS).noquote()
                    << formatTraceEvent(correlation, QStringLiteral("track.completion.exception"),
                                        {DiagnosticField::text(QStringLiteral("reason"),
                                                               QStringLiteral("completion callback threw"))});
            }
        },
        Qt::QueuedConnection);
    if(!queued) {
        qCWarning(BANDCAMP_TRACKS).noquote()
            << formatTraceEvent(correlation, QStringLiteral("track.completion.dispatch_failed"), {});
    }
}

void BandcampLibraryGateway::traceTransportChange(const CorrelationContext& context, const Fooyin::Track& previous,
                                                  const Fooyin::Track& updated) const
{
    if(!BANDCAMP_TRACE_TRACKS().isDebugEnabled()) {
        return;
    }
    const QStringList changed = changedTransportProperties(previous, updated);
    if(changed.isEmpty()) {
        return;
    }

    qCDebug(BANDCAMP_TRACE_TRACKS).noquote()
        << formatTraceEvent(context, QStringLiteral("transport.updated"),
                            {DiagnosticField::text(QStringLiteral("properties"), changed.join(u","_s)),
                             DiagnosticField::text(QStringLiteral("reason"),
                                                   QStringLiteral("transport-only resident snapshot merge")),
                             DiagnosticField::url(QStringLiteral("old_source_url"),
                                                  QUrl{trackProperty(previous, BandcampSourceUrlProperty)}),
                             DiagnosticField::url(QStringLiteral("new_source_url"),
                                                  QUrl{trackProperty(updated, BandcampSourceUrlProperty)}),
                             DiagnosticField::url(QStringLiteral("old_stream_locator"),
                                                  QUrl{trackProperty(previous, BandcampStreamUrlProperty)}),
                             DiagnosticField::url(QStringLiteral("new_stream_locator"),
                                                  QUrl{trackProperty(updated, BandcampStreamUrlProperty)}),
                             DiagnosticField::text(QStringLiteral("old_expiry_ms"),
                                                   trackProperty(previous, BandcampExpiresAtProperty)),
                             DiagnosticField::text(QStringLiteral("new_expiry_ms"),
                                                   trackProperty(updated, BandcampExpiresAtProperty))});
}

void BandcampLibraryGateway::logFailure(const CorrelationContext& context,
                                        const FooyinTrackCommitFailure& error) const
{
    const CorrelationContext failureContext = error.trackId > 0 ? context.withTrack(error.trackId) : context;
    qCWarning(BANDCAMP_TRACKS).noquote()
        << formatTraceEvent(failureContext, QStringLiteral("track.commit.failed"),
                            {DiagnosticField::text(QStringLiteral("code"), errorCodeName(error.code)),
                             DiagnosticField::text(QStringLiteral("reason"), error.reason)});
}
} // namespace Fooyin::Bandcamp
