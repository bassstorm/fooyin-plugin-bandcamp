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
    QObject::connect(m_service, &BandcampService::scopeResolved, this, &BandcampPlugin::onScopeResolved);
    QObject::connect(m_service, &BandcampService::progressChanged, this, &BandcampPlugin::onOperationProgress);
    QObject::connect(m_service, &BandcampService::operationFinished, this, &BandcampPlugin::onOperationFinished);
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

    const auto submission = m_service->submit(urls, OperationOptions{settings.parallelism, settings.itemTimeoutMs});
    if(const auto* rejected = std::get_if<BandcampSubmissionRejected>(&submission)) {
        qCWarning(BANDCAMP_UI).noquote() << "Bandcamp request was rejected:" << rejected->error.reason;
        auto* dialog = new BandcampResultsDialog(Utils::getMainWindow());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->showRejection(rejected->error);
        dialog->open();
        return;
    }

    const BandcampOperationId id = std::get<BandcampOperationAccepted>(submission).id;
    auto* dialog                 = new BandcampResultsDialog(Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_resultDialogs.insert(id, dialog);
    QObject::connect(dialog, &QDialog::finished, this, [this, id]() {
        if(m_service) {
            m_service->cancel(id);
        }
    });
    QObject::connect(dialog, &QObject::destroyed, this, [this, id]() { m_resultDialogs.remove(id); });
    dialog->open();
}

void BandcampPlugin::onScopeResolved(const BandcampScopeResult& result)
{
    const auto it = m_resultDialogs.constFind(result.operationId);
    if(it != m_resultDialogs.cend() && *it) {
        (*it)->appendResult(result);
    }
}

void BandcampPlugin::onOperationProgress(const BandcampOperationProgress& progress)
{
    const auto it = m_resultDialogs.constFind(progress.operationId);
    if(it != m_resultDialogs.cend() && *it) {
        (*it)->updateProgress(progress);
    }
}

void BandcampPlugin::onOperationFinished(const BandcampOperationSummary& summary)
{
    const auto it = m_resultDialogs.find(summary.operationId);
    if(it != m_resultDialogs.end()) {
        if(it.value()) {
            it.value()->finish(summary);
        }
        m_resultDialogs.erase(it);
    }
}
} // namespace Fooyin::Bandcamp
