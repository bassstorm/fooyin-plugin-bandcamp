#pragma once

#include <core/network/remotestreamdevice.h>

#include <algorithm>
#include <chrono>
#include <stop_token>
#include <thread>

namespace Fooyin::Bandcamp {
inline bool waitForRemoteStreamData(Fooyin::RemoteStreamDevice& remote, std::stop_token cancellation,
                                    std::chrono::steady_clock::time_point deadline)
{
    // https://github.com/fooyin/fooyin/issues/1861: early reads cache pre-header seekability.
    // Revisit this gate when supported Fooyin versions handle early reads safely.
    // QIODevice::waitForReadyRead() is optional, so use the remote buffer contract.
    while(!cancellation.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if(now >= deadline) {
            return false;
        }
        if(remote.bufferedByteCount() > 0) {
            return true;
        }
        std::this_thread::sleep_until(std::min(deadline, now + std::chrono::milliseconds{10}));
    }
    return false;
}
} // namespace Fooyin::Bandcamp
