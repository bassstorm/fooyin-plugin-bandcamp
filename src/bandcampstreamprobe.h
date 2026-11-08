#pragma once

#include "bandcampfooyintrack.h"

#include <core/engine/audioloader.h>
#include <core/network/remoteioservice.h>

#include <QHash>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <optional>

namespace Fooyin::Bandcamp {
inline constexpr qsizetype MaxProbeRequests = 2;

enum class BandcampStreamProbeFailure : quint8
{
    Cancelled,
    RemoteUnavailable,
    OpenFailed,
    TimedOut,
    EmptyStream,
    DecoderUnavailable,
    DecoderExhausted,
    DecoderLimitReached,
    ReadLimitReached,
    DecoderException,
    BatchDeadline,
    CapacityRejected,
};

struct BandcampStreamProbeResult
{
    qint64 trackId{0};
    BandcampStreamProperties properties;
    std::optional<BandcampStreamProbeFailure> failure;
};

struct BandcampStreamProbeSubmission
{
    quint64 token{0};
    std::optional<BandcampStreamProbeFailure> rejection;
};

class BandcampStreamProbe : public QObject
{
    Q_OBJECT

public:
    using Completion = std::function<void(QList<BandcampStreamProbeResult>)>;

    BandcampStreamProbe(std::shared_ptr<Fooyin::AudioLoader> audioLoader, Fooyin::RemoteIoService* remoteIo,
                        QObject* parent = nullptr);
    ~BandcampStreamProbe() override;

    [[nodiscard]] virtual BandcampStreamProbeSubmission probe(const QList<BandcampResolvedTrack>& tracks,
                                                              QObject* callbackContext, Completion completion);
    virtual void cancel(quint64 token);
    void shutdown();

private:
    struct State;
    struct Request;
    std::shared_ptr<State> m_state;
    QHash<quint64, std::shared_ptr<Request>> m_requests;
    quint64 m_nextToken{1};
    bool m_shuttingDown{false};
};
} // namespace Fooyin::Bandcamp
