#pragma once

class QObject;
class QString;

namespace oc {

class TFile;
class VaultSession;

// The few Vault calls the workspace makes, kept in one translation unit. They are exempt from UBSan's
// vptr check, which would otherwise need the Vault's type info at link time — and the Vault
// implementation lives in another module (see vault_link_fallback.cpp).
TFile* vaultFile(VaultSession* session, const QString& path); // nullptr without a vault
QObject* vaultObject(VaultSession* session);                   // the session's Vault as a QObject
inline bool hasVault(VaultSession* session) { return vaultObject(session) != nullptr; }

} // namespace oc
