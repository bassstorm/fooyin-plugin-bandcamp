#pragma once

#include "bandcampplayback.h"
#include "bandcampresolutionservice.h"
#include "bandcampurl.h"

#include <core/engine/inputplugin.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <core/track.h>
#include <gui/plugins/guiplugin.h>
#include <gui/plugins/pluginconfigguiplugin.h>

#include <QList>
#include <QObject>
#include <QPointer>

#include <memory>

class QDialog;

namespace Fooyin {
class NetworkAccessManager;
class RemoteIoService;
class CurrentPlaylistController;
class PlaylistInteractor;
} // namespace Fooyin

namespace Fooyin::Bandcamp {
class BandcampImportController;
class BandcampRefreshController;
class BandcampStreamProbe;
class BandcampPlugin : public QObject,
                       public Plugin,
                       public CorePlugin,
                       public InputPlugin,
                       public PluginConfigGuiPlugin,
                       public GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "bandcamp.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::InputPlugin Fooyin::PluginConfigGuiPlugin Fooyin::GuiPlugin)

public:
    ~BandcampPlugin() override;
    void initialise(const CorePluginContext& context) override;
    [[nodiscard]] QString inputName() const override;
    [[nodiscard]] InputCreator inputCreator() const override;
    [[nodiscard]] std::unique_ptr<PluginSettingsProvider> settingsProvider() const override;
    void initialise(const GuiPluginContext& context) override;
    void shutdown() override;

private:
    void trackDialog(QDialog* dialog);
    void showAddUrlDialog();
    void onUrlsSubmitted(const QList<BandcampPageUrl>& urls, bool forceNewPlaylist);
    void refreshSelectedTracks(const Fooyin::TrackList& tracks);
    [[nodiscard]] bool operationRunning() const;

    // Kept alive for as long as the service that uses it.
    std::shared_ptr<NetworkAccessManager> m_network;
    // Owned by this plugin through the QObject parent tree.
    BandcampResolutionService* m_service{nullptr};
    QPointer<Fooyin::RemoteIoService> m_remoteIo;
    BandcampStreamProbe* m_streamProbe{nullptr};
    BandcampLibraryGateway* m_libraryGateway{nullptr};
    std::shared_ptr<BandcampPlaybackBridge> m_playbackBridge;
    QPointer<Fooyin::CurrentPlaylistController> m_playlistSelection;
    QPointer<Fooyin::PlaylistInteractor> m_playlistInteractor;
    QList<QPointer<QDialog>> m_dialogs;
    QList<QPointer<BandcampImportController>> m_imports;
    QList<QPointer<BandcampRefreshController>> m_refreshes;
};
} // namespace Fooyin::Bandcamp
