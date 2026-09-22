#include "ui/workspace/vault_access.h"

#include "core/vault/vault.h"
#include "core/vault_session.h"

namespace oc {

__attribute__((no_sanitize("vptr"))) TFile* vaultFile(VaultSession* session, const QString& path)
{
    Vault* v = session ? session->vault() : nullptr;
    return v ? v->file(path) : nullptr;
}

__attribute__((no_sanitize("vptr"))) QObject* vaultObject(VaultSession* session)
{
    Vault* v = session ? session->vault() : nullptr;
    return v;
}

} // namespace oc
