#include "bandcampimportcontroller.h"

#include "bandcamplogging.h"

#include <QThread>

#include <utility>

namespace Fooyin::Bandcamp {
namespace {
CorrelationContext importCorrelation(BandcampOperationId operationId)
{
    return {.jobId = static_cast<qint64>(operationId)};
}

void logCancelFailure(BandcampOperationId operationId, QString reason)
{
    qCWarning(BANDCAMP_UI).noquote()
        << formatTraceEvent(importCorrelation(operationId), QStringLiteral("import.cancel_failed"),
                            {DiagnosticField::text(QStringLiteral("reason"), std::move(reason))});
}
} // namespace

BandcampImportController::BandcampImportController(BandcampOperationId operationId,
                                                   QList<BandcampPageUrl> submittedPages,
                                                   bool forceNewPlaylist, Fooyin::UId selectedPlaylistId,
                                                   int activeTimeoutMs, FooyinLibraryApi libraryApi,
                                                   BandcampPlaylistApi playlistApi, CancelOperation cancelOperation,
                                                   QObject* parent)
    : QObject{parent}
    , m_operationId{operationId}
    , m_submittedPages{std::move(submittedPages)}
    , m_destinationId{std::move(selectedPlaylistId)}
    , m_activeTimeoutMs{activeTimeoutMs}
    , m_libraryOwner{libraryApi.owner}
    , m_playlistApi{std::move(playlistApi)}
    , m_cancelOperation{std::move(cancelOperation)}
    , m_gateway{new BandcampLibraryGateway{std::move(libraryApi), this}}
{
    const bool ownersAvailable = playlistOwnersAvailable();
    if(m_initialFailure || !ownersAvailable || !m_playlistApi.inspect || !m_playlistApi.createNew
       || !m_playlistApi.insert) {
        m_initialFailure = true;
        m_stopReason = tr("The playlist service is unavailable.");
    }
    else if(forceNewPlaylist || !m_destinationId.isValid()) {
        m_pendingNewPlaylist = true;
    }
    else if(m_playlistApi.owner->thread() != thread()) {
        m_initialFailure = true;
        m_stopReason = tr("The playlist service is unavailable.");
    }
    else {
        try {
            switch(m_playlistApi.inspect(m_destinationId)) {
                case BandcampPlaylistAvailability::Writable:
                    break;
                case BandcampPlaylistAvailability::OwnerUnavailable:
                    m_initialFailure = true;
                    m_stopReason = tr("The playlist service is unavailable.");
                    break;
                case BandcampPlaylistAvailability::Missing:
                case BandcampPlaylistAvailability::Temporary:
                case BandcampPlaylistAvailability::Automatic:
                case BandcampPlaylistAvailability::Locked:
                    m_pendingNewPlaylist = true;
                    break;
            }
        }
        catch(...) {
            m_initialFailure = true;
            m_stopReason = tr("The playlist service is unavailable.");
        }
    }
}

BandcampImportController::~BandcampImportController()
{
    const bool withdrawResolution = !m_resolutionFinished && !m_resolutionCancellationIssued;
    m_finished = true;
    m_stopping = true;
    m_pendingScopes.clear();
    m_handoffPhase = HandoffPhase::None;
    if(m_gateway) {
        m_gateway->cancel();
    }
    if(withdrawResolution) {
        requestResolutionCancellation();
    }
}

void BandcampImportController::start()
{
    if(m_started || m_finished) {
        return;
    }
    m_started = true;
    if(m_initialFailure) {
        failRun(m_stopReason, false);
    }
}

void BandcampImportController::cancel()
{
    if(m_finished || m_stopping) {
        return;
    }

    const QPointer<BandcampImportController> guard{this};
    m_stopping = true;
    clearPendingAsCancelled();
    switch(m_handoffPhase) {
        case HandoffPhase::Gateway:
            ++m_cancelled;
            m_handoffPhase = HandoffPhase::None;
            break;
        case HandoffPhase::PreparingDestination:
            ++m_cancelled;
            break;
        case HandoffPhase::None:
        case HandoffPhase::Inserting:
            break;
    }
    m_gateway->cancel();
    if(!guard) {
        return;
    }
    guard->requestResolutionCancellation();
    if(guard) {
        guard->finishIfReady();
    }
}

void BandcampImportController::acceptScopeResult(const BandcampScopeResult& result)
{
    if(result.operationId != m_operationId || m_finished) {
        return;
    }
    if(m_stopping) {
        ++m_cancelled;
        return;
    }

    m_pendingScopes.enqueue(result);
    pump();
}

void BandcampImportController::acceptProgress(const BandcampOperationProgress& progress)
{
    if(progress.operationId == m_operationId && !m_finished && !m_stopping) {
        emit progressChanged(progress);
    }
}

void BandcampImportController::finishResolution(const BandcampOperationSummary& summary)
{
    if(summary.operationId != m_operationId || m_resolutionFinished || m_finished) {
        return;
    }
    m_resolutionFinished = true;
    if((m_handoffPhase != HandoffPhase::None || !m_pendingScopes.isEmpty()) && !m_importingEmitted) {
        m_importingEmitted = true;
        const QPointer<BandcampImportController> guard{this};
        emit importing();
        if(!guard) {
            return;
        }
    }
    finishIfReady();
}

void BandcampImportController::pump()
{
    if(!m_started || m_finished || m_stopping || m_handoffPhase != HandoffPhase::None) {
        return;
    }

    while(!m_pendingScopes.isEmpty()) {
        const BandcampScopeResult scope = m_pendingScopes.dequeue();
        if(std::holds_alternative<ResolutionFailure>(scope.outcome)) {
            ++m_failed;
            continue;
        }
        if(std::holds_alternative<ResolutionCancelled>(scope.outcome)) {
            ++m_cancelled;
            continue;
        }

        const QPointer<BandcampImportController> guard{this};
        const bool available = playlistOwnersAvailable();
        if(!guard) {
            return;
        }
        if(QThread::currentThread() != thread() || !available) {
            guard->failRun(tr("The playlist service is unavailable."), true);
            return;
        }
        if(!guard->m_pendingNewPlaylist) {
            const bool writable = guard->destinationWritable(guard->m_destinationId);
            if(!guard) {
                return;
            }
            if(!writable) {
                guard->failRun(tr("The selected playlist is no longer available or writable."), true);
                return;
            }
        }

        const BandcampResolvedData& data
            = std::get<ResolutionSuccess<BandcampResolvedData>>(scope.outcome).value;
        QList<BandcampResolvedTrack> tracks;
        if(const auto* album = std::get_if<BandcampAlbum>(&data)) {
            tracks.reserve(album->tracks.size());
            for(const BandcampTrack& track : album->tracks) {
                tracks.append({track, album->metadata});
            }
        }
        else {
            tracks.append(std::get<BandcampResolvedTrack>(data));
        }

        guard->m_handoffPhase = HandoffPhase::Gateway;
        const CorrelationContext correlation{.jobId = static_cast<qint64>(m_operationId),
                                             .resourceUrl = QUrl(scope.page.url)};
        try {
            guard->m_gateway->commit(std::move(tracks), guard->m_activeTimeoutMs, guard, correlation,
                              [guard, scope](FooyinTrackCommitOutcome outcome) mutable {
                                  if(guard) {
                                      guard->onCommit(scope, std::move(outcome));
                                  }
                              });
        }
        catch(...) {
            if(guard) {
                guard->m_handoffPhase = HandoffPhase::None;
                guard->failRun(tr("Preparing tracks for the playlist failed."), true);
            }
        }
        return;
    }

    finishIfReady();
}

void BandcampImportController::onCommit(const BandcampScopeResult& scope, FooyinTrackCommitOutcome outcome)
{
    if(m_finished || m_handoffPhase != HandoffPhase::Gateway) {
        return;
    }
    m_handoffPhase = HandoffPhase::PreparingDestination;
    if(m_stopping) {
        m_handoffPhase = HandoffPhase::None;
        finishIfReady();
        return;
    }

    if(const auto* failure = std::get_if<FooyinTrackCommitFailure>(&outcome)) {
        m_handoffPhase = HandoffPhase::None;
        if(isFatalGatewayFailure(*failure)) {
            failRun(tr("The library service is unavailable."), true, false);
            return;
        }
        ++m_failed;
        pump();
        return;
    }

    const FooyinTrackCommitSuccess& prepared = std::get<FooyinTrackCommitSuccess>(outcome);
    Fooyin::UId destination;
    const QPointer<BandcampImportController> guard{this};
    const bool destinationPrepared = guard->prepareDestination(scope, destination);
    if(!guard) {
        return;
    }
    if(guard->m_stopping || guard->m_finished) {
        guard->m_handoffPhase = HandoffPhase::None;
        guard->finishIfReady();
        return;
    }
    if(!destinationPrepared) {
        guard->m_handoffPhase = HandoffPhase::None;
        guard->failRun(tr("A writable playlist could not be created."), true);
        return;
    }

    const auto insert = guard->m_playlistApi.insert;
    guard->m_handoffPhase = HandoffPhase::Inserting;
    try {
        insert(destination, prepared.tracks);
    }
    catch(...) {
        if(guard) {
            guard->m_handoffPhase = HandoffPhase::None;
            if(guard->m_stopping) {
                ++guard->m_failed;
                guard->finishIfReady();
            }
            else {
                guard->failRun(tr("The prepared tracks could not be added to the playlist."), true);
            }
        }
        return;
    }
    if(!guard) {
        return;
    }

    ++guard->m_added;
    guard->m_handoffPhase = HandoffPhase::None;
    if(guard->m_stopping) {
        guard->finishIfReady();
    }
    else {
        guard->pump();
    }
}

void BandcampImportController::failRun(QString reason, bool countCurrentScope, bool logFailure)
{
    if(m_finished || m_stopping) {
        return;
    }
    if(countCurrentScope) {
        ++m_failed;
    }
    m_stopping = true;
    m_stopReason = std::move(reason);
    clearPendingAsCancelled();
    const QString stopReason = m_stopReason;
    const BandcampOperationId operationId = m_operationId;
    if(logFailure) {
        qCWarning(BANDCAMP_UI).noquote()
            << formatTraceEvent(importCorrelation(operationId), QStringLiteral("import.stopped"),
                                {DiagnosticField::text(QStringLiteral("reason"), stopReason)});
    }
    const QPointer<BandcampImportController> guard{this};
    m_gateway->cancel();
    if(!guard) {
        return;
    }
    emit integrationError(stopReason);
    if(!guard) {
        return;
    }
    guard->requestResolutionCancellation();
    if(guard) {
        guard->finishIfReady();
    }
}

void BandcampImportController::requestResolutionCancellation()
{
    if(m_resolutionCancellationIssued) {
        return;
    }

    m_resolutionCancellationIssued = true;
    const BandcampOperationId operationId = m_operationId;
    const CancelOperation cancelOperation = m_cancelOperation;
    if(!cancelOperation) {
        return;
    }
    try {
        cancelOperation(operationId);
    }
    catch(...) {
        logCancelFailure(operationId, QStringLiteral("resolution cancellation threw"));
    }
}

void BandcampImportController::clearPendingAsCancelled()
{
    m_cancelled += m_pendingScopes.size();
    m_pendingScopes.clear();
}

void BandcampImportController::finishIfReady()
{
    if(m_finished || !m_resolutionFinished || m_handoffPhase != HandoffPhase::None || !m_pendingScopes.isEmpty()) {
        return;
    }
    m_finished = true;
    const BandcampImportSummary summary{m_operationId, m_added, m_failed, m_cancelled, m_stopReason};
    qCInfo(BANDCAMP_UI).noquote()
        << formatTraceEvent(importCorrelation(summary.operationId), QStringLiteral("import.completed"),
                            {DiagnosticField::number(QStringLiteral("added"), summary.added),
                             DiagnosticField::number(QStringLiteral("failed"), summary.failed),
                             DiagnosticField::number(QStringLiteral("cancelled"), summary.cancelled)});
    emit finished(summary);
}

bool BandcampImportController::playlistOwnersAvailable() const
{
    const QPointer<QObject> owner = m_playlistApi.owner;
    const auto ownersAvailable = m_playlistApi.ownersAvailable;
    const QThread* controllerThread = thread();
    if(!owner || owner->thread() != controllerThread || !ownersAvailable) {
        return false;
    }
    const QPointer<BandcampImportController> guard{const_cast<BandcampImportController*>(this)};
    try {
        const bool available = ownersAvailable();
        return guard && owner && owner->thread() == controllerThread && available;
    }
    catch(...) {
        return false;
    }
}

bool BandcampImportController::destinationWritable(const Fooyin::UId& id) const
{
    const QPointer<BandcampImportController> guard{const_cast<BandcampImportController*>(this)};
    const auto inspect = m_playlistApi.inspect;
    if(!id.isValid()) {
        return false;
    }
    const bool available = playlistOwnersAvailable();
    if(!guard || !available || !inspect) {
        return false;
    }
    try {
        const BandcampPlaylistAvailability state = inspect(id);
        return guard && state == BandcampPlaylistAvailability::Writable;
    }
    catch(...) {
        return false;
    }
}

bool BandcampImportController::prepareDestination(const BandcampScopeResult& scope, Fooyin::UId& id)
{
    const QPointer<BandcampImportController> guard{this};
    const bool available = playlistOwnersAvailable();
    if(!guard) {
        return false;
    }
    if(!available || QThread::currentThread() != thread()) {
        guard->failRun(tr("The playlist service is unavailable."), true);
        return false;
    }
    if(guard->m_pendingNewPlaylist) {
        std::optional<Fooyin::UId> created;
        const auto createNew = guard->m_playlistApi.createNew;
        const QString name
            = guard->newPlaylistName(std::get<ResolutionSuccess<BandcampResolvedData>>(scope.outcome).value);
        try {
            created = createNew(name);
        }
        catch(...) {
            if(guard) {
                guard->failRun(tr("A writable playlist could not be created."), true);
            }
            return false;
        }
        if(!guard) {
            return false;
        }
        if(guard->m_stopping || guard->m_finished) {
            return false;
        }
        if(!created || !created->isValid()) {
            guard->failRun(tr("A writable playlist could not be created."), true);
            return false;
        }
        guard->m_destinationId = *created;
        guard->m_pendingNewPlaylist = false;
    }
    const bool writable = guard->destinationWritable(guard->m_destinationId);
    if(!guard) {
        return false;
    }
    if(!writable) {
        guard->failRun(tr("The selected playlist is no longer available or writable."), true);
        return false;
    }
    id = guard->m_destinationId;
    return true;
}

QString BandcampImportController::newPlaylistName(const BandcampResolvedData& data) const
{
    if(m_submittedPages.size() != 1 || m_submittedPages.front().kind == BandcampPageKind::Discography) {
        return tr("Bandcamp");
    }
    if(const auto* album = std::get_if<BandcampAlbum>(&data)) {
        return album->metadata.title;
    }
    const BandcampResolvedTrack& track = std::get<BandcampResolvedTrack>(data);
    return track.album ? track.album->title : track.track.title;
}

bool BandcampImportController::isFatalGatewayFailure(const FooyinTrackCommitFailure& failure) const
{
    if(failure.code == FooyinTrackCommitErrorCode::LibraryUnavailable
       || failure.code == FooyinTrackCommitErrorCode::WrongThread) {
        return true;
    }
    return failure.code == FooyinTrackCommitErrorCode::HostOperationFailed
        && (!m_libraryOwner || m_libraryOwner->thread() != thread());
}
} // namespace Fooyin::Bandcamp
