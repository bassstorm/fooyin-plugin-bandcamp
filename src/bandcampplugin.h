#pragma once

#include "bandcampservice.h"
#include "bandcampurl.h"

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>
#include <gui/plugins/pluginconfigguiplugin.h>

#include <QList>
#include <QObject>
#include <QPointer>

#include <memory>

namespace Fooyin {
class NetworkAccessManager;
class CurrentPlaylistController;
class PlaylistInteractor;
} // namespace Fooyin

namespace Fooyin::Bandcamp {
class BandcampImportController;
class BandcampResultsDialog;
class BandcampPlugin : public QObject,
                       public Plugin,
                       public CorePlugin,
                       public PluginConfigGuiPlugin,
                       public GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "bandcamp.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::PluginConfigGuiPlugin Fooyin::GuiPlugin)

public:
    void initialise(const CorePluginContext& context) override;
    [[nodiscard]] std::unique_ptr<PluginSettingsProvider> settingsProvider() const override;
    void initialise(const GuiPluginContext& context) override;

private:
    void showAddUrlDialog();
    void onUrlsSubmitted(const QList<BandcampPageUrl>& urls, bool forceNewPlaylist);

    // Kept alive for as long as the service that uses it.
    std::shared_ptr<NetworkAccessManager> m_network;
    // Owned by this plugin through the QObject parent tree.
    BandcampService* m_service{nullptr};
    QPointer<Fooyin::CurrentPlaylistController> m_playlistSelection;
    QPointer<Fooyin::PlaylistInteractor> m_playlistInteractor;
};
} // namespace Fooyin::Bandcamp
