#pragma once

namespace oc {

class FileRecovery;
class VaultSession;

// Connects a session's services to its vault's events, the way Obsidian wires them internally:
//  - MetadataCache: full index when the vault has loaded, re-parse on create/modify, follow
//    renames/deletes (its content provider reads through the vault);
//  - DocumentManager: external modifications are merged into open documents, renames/deletes are
//    followed, an offline vault makes documents read-only;
//  - FileRecovery (optional): notes are snapshotted when created or modified.
// Call after the services are set on the session and before Vault::load().
void bindVaultServices(VaultSession* session, FileRecovery* recovery = nullptr);

} // namespace oc
