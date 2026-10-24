#include "bandcampplugin.h"

#include "bandcamplogging.h"
#include "bandcampresultsdialog.h"
#include "bandcampsettings.h"
#include "bandcampsettingsdialog.h"
#include "bandcampurldialog.h"

#include <core/coresettings.h>
#include <core/network/networkaccessmanager.h>

#include <core/plugins/coreplugincontext.h>
#include <gui/guiconstants.h>
#include <gui/plugins/guiplugincontext.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/utils.h>

#include <QAction>
#include <QMainWindow>

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
    QObject::connect(m_service, &BandcampService::batchResolved, this, &BandcampPlugin::showResults);
}

std::unique_ptr<PluginSettingsProvider> BandcampPlugin::settingsProvider() const
{
    return std::make_unique<BandcampSettingsProvider>();
}

void BandcampPlugin::initialise(const GuiPluginContext& context)
{
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

void BandcampPlugin::onUrlsSubmitted(const QList<BandcampPageUrl>& urls)
{
    qCInfo(BANDCAMP_UI) << "Submitted" << urls.size() << "Bandcamp URL(s)";
    if(!m_service) {
        qCWarning(BANDCAMP_UI) << "Bandcamp service is not available; URLs were not resolved";
        return;
    }

    FySettings fySettings;
    const BandcampSettings settings = loadBandcampSettings(fySettings);
    qCInfo(BANDCAMP_SETTINGS) << "parallelism" << settings.parallelism << "item timeout"
                              << settings.itemTimeoutMs / 1000.0 << "s";

    m_service->setMaxItemsInProgress(settings.parallelism);
    m_service->setItemTimeoutMs(settings.itemTimeoutMs);
    m_service->resolve(urls);
}

void BandcampPlugin::showResults(const QList<BandcampResolvedPage>& results)
{
    auto* dialog = new BandcampResultsDialog(results, Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}
} // namespace Fooyin::Bandcamp
