#include "bandcamptrackuri.h"

#include <core/engine/audioloader.h>
#include <core/engine/inputplugin.h>
#include <core/network/networkaccessmanager.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/coreplugincontext.h>
#include <core/plugins/plugin.h>
#include <core/track.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QFile>
#include <QPluginLoader>

#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication application{argc, argv};
    if(argc != 2) {
        std::cerr << "Expected the production Bandcamp plugin module path.\n";
        return 2;
    }

    QPluginLoader pluginLoader{QString::fromLocal8Bit(argv[1])};
    QObject* root = pluginLoader.instance();
    if(!root) {
        std::cerr << "Could not load the production Bandcamp plugin: " << pluginLoader.errorString().toStdString()
                  << '\n';
        return 1;
    }

    auto* plugin = qobject_cast<Fooyin::Plugin*>(root);
    if(!plugin) {
        std::cerr << "The production plugin does not expose Fooyin::Plugin.\n";
        return 1;
    }
    auto* inputPlugin = qobject_cast<Fooyin::InputPlugin*>(root);
    if(!inputPlugin) {
        std::cerr << "The production plugin does not expose Fooyin::InputPlugin.\n";
        return 1;
    }
    auto* corePlugin = qobject_cast<Fooyin::CorePlugin*>(root);
    if(!corePlugin) {
        std::cerr << "The production plugin does not expose Fooyin::CorePlugin.\n";
        return 1;
    }

    {
        const QString settingsPath
            = QCoreApplication::applicationDirPath()
            + QStringLiteral("/bandcamp-plugin-input-settings-%1.ini").arg(QCoreApplication::applicationPid());
        QFile::remove(settingsPath);
        {
            Fooyin::SettingsManager settings{settingsPath};
            auto network     = std::make_shared<Fooyin::NetworkAccessManager>(&settings);
            auto audioLoader = std::make_shared<Fooyin::AudioLoader>();
            {
                Fooyin::CorePluginContext context{nullptr,     nullptr, nullptr, nullptr, nullptr, &settings,
                                                  audioLoader, {},      nullptr, network, {},      nullptr};
                corePlugin->initialise(context);

                const auto creator = inputPlugin->inputCreator();
                if(!creator.decoder) {
                    std::cerr << "The production plugin did not provide a decoder.\n";
                    return 1;
                }

                audioLoader->addDecoder(inputPlugin->inputName(), creator.decoder, creator.priority);
                const auto candidates
                    = audioLoader->decodersForTrack(Fooyin::Track{Fooyin::Bandcamp::bandcampTrackUri(7)});
                if(candidates.size() != 1 || !candidates.front()
                   || !candidates.front()->supportedSchemes().contains(QStringLiteral("bandcamp"))) {
                    std::cerr << "Fooyin did not select the production decoder for a Bandcamp track URI.\n";
                    return 1;
                }
                plugin->shutdown();
            }
            audioLoader.reset();
            if(!pluginLoader.unload()) {
                std::cerr << "The initialized production plugin could not be unloaded.\n";
                return 1;
            }
        }
        QFile::remove(settingsPath);
    }
    return 0;
}
