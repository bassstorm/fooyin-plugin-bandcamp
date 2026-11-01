#include "bandcampplugin.h"

#include "bandcampimportcontroller.h"
#include "bandcamplogging.h"
#include "bandcampresultsdialog.h"
#include "bandcampsettings.h"
#include "bandcampsettingsdialog.h"
#include "bandcampurldialog.h"

#include <core/coresettings.h>
#include <core/network/networkaccessmanager.h>
#include <core/playlist/playlisthandler.h>
#include <core/playlist/playlist.h>

#include <core/plugins/coreplugincontext.h>
#include <gui/guiconstants.h>
#include <gui/plugins/guiplugincontext.h>
#include <gui/playlist/currentplaylistcontroller.h>
#include <gui/playlist/playlistinteractor.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/utils.h>

#include <QAction>
#include <QMainWindow>

#include <stdexcept>

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
    m_network = context.networkAccess;
    m_service = new BandcampService(m_network.get(), this);
}

std::unique_ptr<PluginSettingsProvider> BandcampPlugin::settingsProvider() const
{
    return std::make_unique<BandcampSettingsProvider>();
}

void BandcampPlugin::initialise(const GuiPluginContext& context)
{
    m_playlistSelection = context.playlistSelection;
    m_playlistInteractor = context.playlistInteractor;

    auto* addUrlAction = new QAction(tr("Add &Bandcamp URL…"), this);
    addUrlAction->setStatusTip(tr("Add Bandcamp albums, tracks, or discographies by URL"));
    auto* command = context.actionManager->registerAction(addUrlAction, "File.AddBandcampUrl");
    command->setCategories({tr("File")});
    context.actionManager->actionContainer(Constants::Menus::File)->addAction(command, Actions::Groups::One);

    QObject::connect(addUrlAction, &QAction::triggered, this, &BandcampPlugin::showAddUrlDialog);
}

void BandcampPlugin::showAddUrlDialog()
{
    auto* dialog = new BandcampUrlDialog(Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &BandcampUrlDialog::urlsSubmitted, this, &BandcampPlugin::onUrlsSubmitted);
    dialog->open();
}

void BandcampPlugin::onUrlsSubmitted(const QList<BandcampPageUrl>& urls, bool forceNewPlaylist)
{
    qCInfo(BANDCAMP_UI) << "Submitted" << urls.size() << "Bandcamp URL(s)";
    if(!m_service) {
        qCWarning(BANDCAMP_UI) << "Bandcamp service is not available; URLs were not resolved";
        return;
    }

    FySettings fySettings;
    const BandcampSettings settings = loadBandcampSettings(fySettings);
    qCInfo(BANDCAMP_SETTINGS) << "parallelism" << settings.parallelism << "item timeout"
                              << settings.itemTimeoutMs / 1000.0 << "s"
                              << "discography order" << static_cast<int>(settings.discographyOrder)
                              << "pre-playback refresh window" << settings.prePlaybackRefreshWindowMinutes << "min";

    OperationOptions options;
    options.parallelism                = settings.parallelism;
    options.itemTimeoutMs              = settings.itemTimeoutMs;
    options.discographyOrder           = settings.discographyOrder;
    options.prePlaybackRefreshWindowMs = refreshWindowMilliseconds(settings.prePlaybackRefreshWindowMinutes);
    const Fooyin::UId selectedPlaylistId
        = m_playlistSelection ? m_playlistSelection->currentPlaylistId() : Fooyin::UId{};
    const auto submission = m_service->submit(urls, options);
    if(const auto* rejected = std::get_if<BandcampSubmissionRejected>(&submission)) {
        qCWarning(BANDCAMP_UI).noquote() << "Bandcamp request was rejected:" << rejected->error.reason;
        auto* dialog = new BandcampResultsDialog(Utils::getMainWindow());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->showRejection(rejected->error);
        dialog->open();
        return;
    }

    const BandcampOperationId id = std::get<BandcampOperationAccepted>(submission).id;
    QPointer<Fooyin::PlaylistInteractor> interactor{m_playlistInteractor};
    QPointer<Fooyin::PlaylistHandler> handler{interactor ? interactor->handler() : nullptr};

    BandcampPlaylistApi playlistApi;
    playlistApi.owner = interactor;
    playlistApi.ownersAvailable = [interactor, handler]() { return interactor && handler; };
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
        if(!interactor) {
            throw std::runtime_error{"The playlist interactor was destroyed."};
        }
        interactor->tracksToPlaylist(tracks, playlistId);
    };

    auto* dialog = new BandcampResultsDialog(Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* controller = new BandcampImportController{
        id,
        urls,
        forceNewPlaylist,
        selectedPlaylistId,
        settings.itemTimeoutMs,
        fooyinLibraryApi(interactor ? interactor->library() : nullptr, handler),
        std::move(playlistApi),
        [service = QPointer<BandcampService>{m_service}](BandcampOperationId operationId) {
            if(service) {
                service->cancel(operationId);
            }
        },
        this};

    QObject::connect(m_service, &BandcampService::scopeResolved, controller,
                     [controller](const BandcampScopeResult& result) { controller->acceptScopeResult(result); });
    QObject::connect(m_service, &BandcampService::progressChanged, controller,
                     [controller](const BandcampOperationProgress& progress) {
                         controller->acceptProgress(progress);
                     });
    QObject::connect(m_service, &BandcampService::operationFinished, controller,
                     [controller](const BandcampOperationSummary& summary) {
                         controller->finishResolution(summary);
                     });
    QObject::connect(controller, &BandcampImportController::progressChanged, dialog,
                     &BandcampResultsDialog::updateProgress);
    QObject::connect(controller, &BandcampImportController::importing, dialog,
                     &BandcampResultsDialog::showImporting);
    QObject::connect(controller, &BandcampImportController::integrationError, dialog,
                     &BandcampResultsDialog::showIntegrationError);
    QObject::connect(controller, &BandcampImportController::finished, dialog,
                     &BandcampResultsDialog::finish);
    QObject::connect(controller, &BandcampImportController::finished, controller, [controller]() {
        controller->deleteLater();
    });
    QObject::connect(dialog, &QDialog::finished, controller, &BandcampImportController::cancel);
    dialog->open();
    controller->start();
}
} // namespace Fooyin::Bandcamp
