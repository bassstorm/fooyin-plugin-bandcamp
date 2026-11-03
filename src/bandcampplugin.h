#pragma once

#include "bandcampservice.h"
#include "bandcampplayback.h"
#include "bandcampurl.h"

#include <core/plugins/coreplugin.h>
#include <core/engine/inputplugin.h>
#include <core/track.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>
#include <gui/plugins/pluginconfigguiplugin.h>

#include <QList>
#include <QObject>
#include <QPointer>

#include <memory>

namespace Fooyin {
class NetworkAccessManager;
class RemoteIoService;
class CurrentPlaylistController;
class PlaylistInteractor;
} // namespace Fooyin

namespace Fooyin::Bandcamp {
class BandcampImportController;
class BandcampResultsDialog;
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

private:
    void showAddUrlDialog();
    void onUrlsSubmitted(const QList<BandcampPageUrl>& urls, bool forceNewPlaylist);
    void refreshSelectedTracks(const Fooyin::TrackList& tracks);

    // Kept alive for as long as the service that uses it.
    std::shared_ptr<NetworkAccessManager> m_network;
    // Owned by this plugin through the QObject parent tree.
    BandcampService* m_service{nullptr};
    QPointer<Fooyin::RemoteIoService> m_remoteIo;
    std::shared_ptr<BandcampPlaybackBridge> m_playbackBridge;
    QPointer<Fooyin::CurrentPlaylistController> m_playlistSelection;
    QPointer<Fooyin::PlaylistInteractor> m_playlistInteractor;
};
} // namespace Fooyin::Bandcamp
