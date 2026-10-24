#pragma once

#include <QByteArray>
#include <QList>

// Synthetic pages shaped like Bandcamp's data-tralbum model. No real page is stored on disk;
// live checks against Bandcamp belong to the integration probe.
namespace BandcampTestPages {
// Wraps raw JSON the way Bandcamp does: as an HTML-escaped data-tralbum attribute.
inline QByteArray tralbumPage(QByteArray json)
{
    json.replace('"', "&quot;");
    return "<html><script data-band-follow-info=\"{}\" data-tralbum=\"" + json + "\"></script></html>";
}

// A trackinfo entry with a playable stream, so the track is kept. Fields are raw JSON members.
inline QByteArray streamEntry(const QByteArray& fields)
{
    return "{" + fields + ",\"file\":{\"mp3-128\":\"https://cdn.example.com/track.mp3\"}}";
}

// An album page with the given current-object JSON and raw trackinfo entries.
inline QByteArray albumPage(const QByteArray& currentJson, const QList<QByteArray>& entries)
{
    QByteArray list;
    for(const QByteArray& entry : entries) {
        if(!list.isEmpty()) {
            list += ',';
        }
        list += entry;
    }
    return tralbumPage("{\"current\":" + currentJson + ",\"trackinfo\":[" + list + "]}");
}
} // namespace BandcampTestPages
