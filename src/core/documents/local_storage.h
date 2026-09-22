#pragma once

#include "core/vault/vault.h"

namespace oc {

// DocumentStorage over a plain directory (no watching, no model). Used for files opened outside a
// vault, the --edit dev harness and tests. Writes are atomic compare-and-swap like Vault::write.
class LocalFileStorage : public DocumentStorage {
public:
    explicit LocalFileStorage(QString rootDir);
    QString absolutePath(const QString& path) const;

    std::optional<QByteArray> read(const QString& path, DiskState* state, QString* error) override;
    WriteResult write(const QString& path, const QByteArray& bytes, const DiskState& expected, DiskState* newState, QString* error) override;
    DiskState stat(const QString& path) override;

private:
    QString m_root;
};

} // namespace oc
