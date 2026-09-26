#include "ui/navigation/navigation_util.h"

#include "core/config/vault_config.h"
#include "core/search/search_query.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/workspace/view.h"

#include <QEvent>
#include <QJsonArray>

namespace oc {

Workspace::NewLeaf newLeafForModifiers(Qt::KeyboardModifiers mods)
{
    if (!(mods & Qt::ControlModifier))
        return Workspace::NewLeaf::No;
    if (mods & Qt::AltModifier)
        return (mods & Qt::ShiftModifier) ? Workspace::NewLeaf::Window : Workspace::NewLeaf::SplitRight;
    return Workspace::NewLeaf::Tab;
}

UserIgnoreFilters::UserIgnoreFilters(const VaultConfig* config)
{
    if (!config)
        return;
    const QJsonArray filters = config->get(QStringLiteral("userIgnoreFilters")).toArray();
    for (const QJsonValue& v : filters) {
        const QString f = v.toString();
        if (f.isEmpty())
            continue;
        if (f.size() > 2 && f.startsWith(QLatin1Char('/')) && f.endsWith(QLatin1Char('/'))) {
            QRegularExpression re(search::jsRegexToPcre(QStringView(f).mid(1, f.size() - 2)));
            if (re.isValid())
                m_regexes.append(re);
        } else {
            m_prefixes.append(f);
        }
    }
}

bool UserIgnoreFilters::matches(const QString& path) const
{
    for (const QString& p : m_prefixes)
        if (path.startsWith(p))
            return true;
    for (const QRegularExpression& re : m_regexes)
        if (re.match(path).hasMatch())
            return true;
    return false;
}

QString displayPath(const QString& path)
{
    if (path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) && path.size() > 3)
        return path.left(path.size() - 3);
    return path;
}

QString activeFilePath(VaultSession* session)
{
    Workspace* ws = session ? session->workspace() : nullptr;
    WorkspaceLeaf* leaf = ws ? ws->activeRootLeaf() : nullptr;
    auto* fv = leaf ? qobject_cast<FileView*>(leaf->view()) : nullptr;
    return fv && fv->file() ? fv->file()->path() : QString();
}

void showNotice(OverlayLayer* layer, const QString& message)
{
    if (!layer)
        return;
    if (auto* w = MainWindow::of(layer); w && w->notices())
        w->notices()->show(message);
}

namespace {
// Accepts the key events that reach the modal itself (i.e. that its children didn't take), so they stop there.
class KeyContainer : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (obj == parent()
            && (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease || ev->type() == QEvent::ShortcutOverride)) {
            ev->accept();
            return true;
        }
        return false;
    }
};
} // namespace

void containKeyEvents(QWidget* modal)
{
    if (!modal)
        return;
    modal->installEventFilter(new KeyContainer(modal));
}

} // namespace oc
