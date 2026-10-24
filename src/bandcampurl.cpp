#include "bandcampurl.h"

#include <QStringList>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
constexpr int MaxUrlLength = 2048;

BandcampPageUrl rejected(const QString& reason)
{
    BandcampPageUrl result;
    result.error = reason;
    return result;
}
} // namespace

BandcampPageUrl parseBandcampPageUrl(const QString& text)
{
    const QString input = text.trimmed();
    if(input.isEmpty()) {
        return rejected(u"empty line"_s);
    }
    if(input.size() > MaxUrlLength) {
        return rejected(u"URL is longer than 2048 characters"_s);
    }

    const QUrl url{input, QUrl::StrictMode};
    if(!url.isValid()) {
        return rejected(u"not a valid URL"_s);
    }

    const QString scheme = url.scheme().toLower();
    if(scheme != u"http"_s && scheme != u"https"_s) {
        return rejected(u"expected an http:// or https:// URL"_s);
    }
    if(url.host().isEmpty()) {
        return rejected(u"missing host name"_s);
    }
    if(!url.userName().isEmpty() || !url.password().isEmpty()) {
        return rejected(u"URLs with credentials are not supported"_s);
    }

    QStringList segments = url.path().split(u'/', Qt::SkipEmptyParts);

    BandcampPageKind kind{BandcampPageKind::Discography};
    if(segments.isEmpty() || (segments.size() == 1 && segments.constFirst() == u"music"_s)) {
        segments = {u"music"_s};
    }
    else if(segments.size() == 2 && segments.constFirst() == u"album"_s) {
        kind = BandcampPageKind::Album;
    }
    else if(segments.size() == 2 && segments.constFirst() == u"track"_s) {
        kind = BandcampPageKind::Track;
    }
    else {
        return rejected(u"unsupported Bandcamp page (expected album, track, or discography)"_s);
    }

    QUrl canonical = url;
    canonical.setPath(u"/"_s + segments.join(u'/'));
    canonical.setQuery(QString());
    canonical.setFragment(QString());

    BandcampPageUrl result;
    result.kind = kind;
    result.url  = canonical.toString();
    return result;
}

QString pageKindName(BandcampPageKind kind)
{
    switch(kind) {
        case BandcampPageKind::Discography:
            return u"discography"_s;
        case BandcampPageKind::Album:
            return u"album"_s;
        case BandcampPageKind::Track:
            return u"track"_s;
    }
    return u"unknown"_s;
}
} // namespace Fooyin::Bandcamp
