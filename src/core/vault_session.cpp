#include "core/vault_session.h"

#include "core/config/clone_paths.h"
#include "core/config/config_store.h"

#include <QFileInfo>

namespace oc {

VaultSession::VaultSession(QString id, QString root, QObject* parent)
    : QObject(parent)
    , m_id(std::move(id))
    , m_root(std::move(root))
    , m_config(new ConfigStore(ClonePaths::vaultConfigDir(m_id), this))
{
}

VaultSession::~VaultSession() { m_config->flush(); }

QString VaultSession::name() const { return QFileInfo(m_root).fileName(); }

} // namespace oc
