#include "bandcampplugin.h"

#include "bandcampdiagnostics.h"
#include "bandcampimportcontroller.h"
#include "bandcamplogging.h"
#include "bandcampprogressdialog.h"
#include "bandcamprefreshcontroller.h"
#include "bandcampsettings.h"
#include "bandcampsettingsdialog.h"
#include "bandcampurldialog.h"

#include <core/coresettings.h>
#include <core/network/networkaccessmanager.h>
#include <core/network/remoteioservice.h>
#include <core/playlist/playlist.h>
#include <core/playlist/playlisthandler.h>

#include <core/plugins/coreplugincontext.h>
#include <gui/guiconstants.h>
#include <gui/playlist/currentplaylistcontroller.h>
#include <gui/playlist/playlistinteractor.h>
#include <gui/plugins/guiplugincontext.h>
#include <gui/trackselectioncontroller.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/utils.h>

#include <QAction>
#include <QDateTime>
#include <QMainWindow>
#include <QMenu>

#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
class BandcampSettingsProvider : public PluginSettingsProvider
{
protected:
    QDialog* createSettings(QWidget* parent) override
    {
        return new BandcampSettingsDialog(parent);
    }
};
} // namespace

void BandcampPlugin::initialise(const CorePluginContext& context)
{
    m_network  = context.networkAccess;
    m_service  = new BandcampResolutionService(m_network.get(), this);
    m_remoteIo = new Fooyin::RemoteIoService(context.networkAccess, context.settingsManager, this);
    m_playbackBridge
        = std::make_shared<BandcampPlaybackBridge>(m_service, context.audioLoader, m_remoteIo, context.settingsManager,
                                                   fooyinLibraryApi(context.library, context.playlistHandler));
}

BandcampPlugin::~BandcampPlugin()
{
    shutdown();
}

void BandcampPlugin::shutdown()
{
    const auto dialogs = std::exchange(m_dialogs, {});
    for(const QPointer<QDialog>& dialog : dialogs) {
        delete dialog.data();
    }

    const auto imports = std::exchange(m_imports, {});
    for(const QPointer<BandcampImportController>& controller : imports) {
        if(controller) {
            controller->cancel();
            delete controller.data();
        }
    }

    const auto refreshes = std::exchange(m_refreshes, {});
    for(const QPointer<BandcampRefreshController>& operation : refreshes) {
        if(operation) {
            operation->cancel();
            delete operation.data();
        }
    }

    if(m_playbackBridge) {
        m_playbackBridge->shutdown();
    }

    delete m_remoteIo.data();
    delete std::exchange(m_service, nullptr);
}

void BandcampPlugin::trackDialog(QDialog* dialog)
{
    m_dialogs.append(dialog);
    QObject::connect(dialog, &QObject::destroyed, this, [this, dialog]() { m_dialogs.removeAll(dialog); });
}

QString BandcampPlugin::inputName() const
{
    return tr("Bandcamp");
}

InputCreator BandcampPlugin::inputCreator() const
{
    return bandcampInputCreator(m_playbackBridge);
}

std::unique_ptr<PluginSettingsProvider> BandcampPlugin::settingsProvider() const
{
    return std::make_unique<BandcampSettingsProvider>();
}

void BandcampPlugin::initialise(const GuiPluginContext& context)
{
    m_playlistSelection  = context.playlistSelection;
    m_playlistInteractor = context.playlistInteractor;

    auto* addUrlAction = new QAction(tr("Add &Bandcamp URL…"), this);
    addUrlAction->setStatusTip(tr("Add Bandcamp albums, tracks, or discographies by URL"));
    auto* command = context.actionManager->registerAction(addUrlAction, "File.AddBandcampUrl");
    command->setCategories({tr("File")});
    context.actionManager->actionContainer(Constants::Menus::File)->addAction(command, Actions::Groups::One);

    QObject::connect(addUrlAction, &QAction::triggered, this, &BandcampPlugin::showAddUrlDialog);
    if(context.trackSelection) {
        const bool registered = context.trackSelection->registerTrackContextAction(
            this, TrackContextMenuArea::Track, Constants::Menus::Context::TrackSelection, "Bandcamp.RefreshStreams",
            tr("Refresh Bandcamp streams"),
            [this](QMenu* menu, const Fooyin::TrackSelection& selection) {
                auto* action = menu->addAction(tr("Refresh Bandcamp streams"));
                action->setEnabled(hasSelectedBandcampTracks(selection.tracks));
                connect(action, &QAction::triggered, this,
                        [this, tracks = selection.tracks]() { refreshSelectedTracks(tracks); });
            },
            Constants::Menus::Context::TrackFinalSeparator);
        if(!registered) {
            qCWarning(BANDCAMP_UI) << "Failed to register the Bandcamp stream refresh track context action";
        }
    }
    else {
        qCWarning(BANDCAMP_UI) << "Track selection controller unavailable; stream refresh action was not registered";
    }
}

void BandcampPlugin::refreshSelectedTracks(const Fooyin::TrackList& tracks)
{
    if(!m_service || !hasSelectedBandcampTracks(tracks)) {
        return;
    }
    FySettings fySettings;
    const auto settings = loadBandcampSettings(fySettings);
    const QPointer<Fooyin::PlaylistInteractor> interactor{m_playlistInteractor};
    auto* operation = new BandcampRefreshController{
        m_service, tracks, makeOperationOptions(settings),
        fooyinLibraryApi(interactor ? interactor->library() : nullptr, interactor ? interactor->handler() : nullptr),
        this};
    m_refreshes.append(operation);
    QObject::connect(operation, &QObject::destroyed, this, [this, operation]() { m_refreshes.removeAll(operation); });
    auto* dialog = new BandcampProgressDialog(BandcampDialogMode::Refresh, Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    trackDialog(dialog);
    connect(operation, &BandcampRefreshController::progressChanged, dialog, &BandcampProgressDialog::updateProgress);
    connect(operation, &BandcampRefreshController::finished, dialog, &BandcampProgressDialog::finishRefresh);
    connect(operation, &BandcampRefreshController::finished, operation, &QObject::deleteLater);
    connect(dialog, &QDialog::finished, operation, &BandcampRefreshController::cancel);
    dialog->open();
    operation->start(QDateTime::currentMSecsSinceEpoch());
}

void BandcampPlugin::showAddUrlDialog()
{
    auto* dialog = new BandcampUrlDialog(Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    trackDialog(dialog);
    QObject::connect(dialog, &BandcampUrlDialog::urlsSubmitted, this, &BandcampPlugin::onUrlsSubmitted);
    dialog->open();
}

void BandcampPlugin::onUrlsSubmitted(const QList<BandcampPageUrl>& urls, bool forceNewPlaylist)
{
    qCInfo(BANDCAMP_UI).noquote() << formatTraceEvent(CorrelationContext{}, QStringLiteral("import_started"),
                                                      {DiagnosticField::number(QStringLiteral("urls"), urls.size())});
    if(!m_service) {
        qCWarning(BANDCAMP_UI) << "Bandcamp service is not available; URLs were not resolved";
        return;
    }

    FySettings fySettings;
    const BandcampSettings settings = loadBandcampSettings(fySettings);
    qCDebug(BANDCAMP_SETTINGS).noquote() << formatTraceEvent(
        CorrelationContext{}, QStringLiteral("settings"),
        {DiagnosticField::number(QStringLiteral("parallelism"), settings.parallelism),
         DiagnosticField::number(QStringLiteral("timeoutMs"), settings.attemptTimeoutMs),
         DiagnosticField::number(QStringLiteral("maxAttempts"), settings.maxAttempts),
         DiagnosticField::number(QStringLiteral("cooldownMs"), settings.attemptCooldownMs),
         DiagnosticField::number(QStringLiteral("discographyOrder"), static_cast<qint64>(settings.discographyOrder)),
         DiagnosticField::number(QStringLiteral("refreshWindowMin"), settings.prePlaybackRefreshWindowMinutes)});

    const Fooyin::UId selectedPlaylistId
        = m_playlistSelection ? m_playlistSelection->currentPlaylistId() : Fooyin::UId{};
    const auto submission = m_service->submit(urls, makeOperationOptions(settings));
    if(const auto* rejected = std::get_if<BandcampSubmissionRejected>(&submission)) {
        qCWarning(BANDCAMP_UI).noquote() << "Bandcamp request was rejected:" << rejected->error.reason;
        auto* dialog = new BandcampProgressDialog(BandcampDialogMode::Import, Utils::getMainWindow());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        trackDialog(dialog);
        dialog->showRejection(rejected->error);
        dialog->open();
        return;
    }

    const BandcampOperationId id = std::get<BandcampOperationAccepted>(submission).id;
    QPointer<Fooyin::PlaylistInteractor> interactor{m_playlistInteractor};
    QPointer<Fooyin::PlaylistHandler> handler{interactor ? interactor->handler() : nullptr};

    BandcampPlaylistApi playlistApi;
    playlistApi.owner           = interactor;
    playlistApi.ownersAvailable = [interactor, handler]() {
        return interactor && handler;
    };
    playlistApi.inspect = [interactor, handler](const Fooyin::UId& playlistId) {
        if(!interactor || !handler) {
            return BandcampPlaylistAvailability::OwnerUnavailable;
        }
        Fooyin::Playlist* playlist = handler->playlistById(playlistId);
        if(!playlist) {
            return BandcampPlaylistAvailability::Missing;
        }
        if(playlist->isTemporary()) {
            return BandcampPlaylistAvailability::Temporary;
        }
        if(playlist->isAutoPlaylist()) {
            return BandcampPlaylistAvailability::Automatic;
        }
        if(playlist->isLocked()) {
            return BandcampPlaylistAvailability::Locked;
        }
        return BandcampPlaylistAvailability::Writable;
    };
    playlistApi.createNew = [handler](const QString& name) -> std::optional<Fooyin::UId> {
        if(!handler) {
            return std::nullopt;
        }
        Fooyin::Playlist* playlist = handler->createNewPlaylist(name);
        if(!playlist) {
            return std::nullopt;
        }
        return playlist->id();
    };
    playlistApi.insert = [interactor](const Fooyin::UId& playlistId, const Fooyin::TrackList& tracks) {
        if(interactor) {
            interactor->tracksToPlaylist(tracks, playlistId);
        }
    };

    auto* dialog = new BandcampProgressDialog(BandcampDialogMode::Import, Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    trackDialog(dialog);
    auto* controller = new BandcampImportController{
        id,
        urls,
        forceNewPlaylist,
        selectedPlaylistId,
        fooyinLibraryApi(interactor ? interactor->library() : nullptr, handler),
        std::move(playlistApi),
        [service = QPointer<BandcampResolutionService>{m_service}](BandcampOperationId operationId) {
            if(service) {
                service->cancel(operationId);
            }
        },
        this};
    m_imports.append(controller);
    QObject::connect(controller, &QObject::destroyed, this, [this, controller]() { m_imports.removeAll(controller); });

    QObject::connect(m_service, &BandcampResolutionService::scopeResolved, controller,
                     [controller](const BandcampScopeResult& result) { controller->acceptScopeResult(result); });
    QObject::connect(m_service, &BandcampResolutionService::progressChanged, controller,
                     [controller](const BandcampOperationProgress& progress) { controller->acceptProgress(progress); });
    QObject::connect(m_service, &BandcampResolutionService::operationFinished, controller,
                     [controller](const BandcampOperationSummary& summary) { controller->finishResolution(summary); });
    QObject::connect(controller, &BandcampImportController::progressChanged, dialog,
                     &BandcampProgressDialog::updateProgress);
    QObject::connect(controller, &BandcampImportController::importing, dialog, &BandcampProgressDialog::showImporting);
    QObject::connect(controller, &BandcampImportController::integrationError, dialog,
                     &BandcampProgressDialog::showIntegrationError);
    QObject::connect(controller, &BandcampImportController::finished, dialog, &BandcampProgressDialog::finish);
    QObject::connect(controller, &BandcampImportController::finished, controller,
                     [controller]() { controller->deleteLater(); });
    QObject::connect(dialog, &QDialog::finished, controller, &BandcampImportController::cancel);
    dialog->open();
    controller->start();
}
} // namespace Fooyin::Bandcamp
