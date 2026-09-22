// TEMPORARY link shim — delete this file once src/core/vault/vault.cpp (the Vault implementation,
// written in parallel on another branch) is merged.
//
// FileView::setState() resolves paths with Vault::file(), a non-inline member of the frozen Vault
// interface. Until the implementation lands, nothing defines that symbol, so oc_ui (and everything
// linking it) would fail to link. This *weak* definition only satisfies the linker: whenever the real
// vault.o is linked (i.e. whenever anything constructs a Vault, which is the only way
// VaultSession::vault() can be non-null), its strong definition takes precedence, and while no Vault
// exists the function is never called. Keeping it after the merge is harmless, but it should go.

#include "core/vault/vault.h"

namespace oc {

__attribute__((weak)) TFile* Vault::file(const QString& path) const
{
    Q_UNUSED(path);
    return nullptr;
}

} // namespace oc
