#pragma once

#include "bandcampfooyintrack.h"
#include "bandcampservice.h"

#include <core/engine/inputplugin.h>

#include <chrono>
#include <memory>
#include <stop_token>
#include <variant>

class QUrl;

namespace Fooyin {
class AudioLoader;
class RemoteIoService;
class SettingsManager;
struct RemoteStreamSource;
} // namespace Fooyin

namespace Fooyin::Bandcamp {
class BandcampPlaybackBridge final
{
public:
    struct State;

    struct PreparedTransport
    {
        QString streamUrl;
        std::chrono::steady_clock::time_point deadline;
    };

    struct Failure
    {
        QString reason;
    };

    using Preparation = std::variant<PreparedTransport, Failure>;

    BandcampPlaybackBridge(BandcampService* service, std::shared_ptr<Fooyin::AudioLoader> audioLoader,
                           Fooyin::RemoteIoService* remoteIo, Fooyin::SettingsManager* settings,
                           FooyinLibraryApi libraryApi);
    ~BandcampPlaybackBridge();

    [[nodiscard]] std::shared_ptr<Fooyin::AudioLoader> audioLoader() const;
    [[nodiscard]] bool createRemoteStreamSource(const QUrl& url, Fooyin::RemoteStreamSource& source) const;
    [[nodiscard]] std::stop_token shutdownToken() const;
    [[nodiscard]] Preparation prepare(const Fooyin::Track& track, std::stop_token cancellation) const;
    void shutdown();

private:
    std::shared_ptr<State> m_state;
};

[[nodiscard]] Fooyin::InputCreator bandcampInputCreator(
    const std::weak_ptr<BandcampPlaybackBridge>& bridge);
} // namespace Fooyin::Bandcamp
