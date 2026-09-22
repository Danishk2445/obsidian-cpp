#include "core/vault/trash.h"

#include "core/vault/fs_scan.h"
#include "core/vault/ignore_rules.h"
#include "core/vault/vault_io.h"

#include <KIO/CopyJob>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <cerrno>

#include <sys/stat.h>

namespace oc::vaultfs {

bool trashToSystem(const QString& absPath, QString* error)
{
    QString kioError;
    if (QCoreApplication::instance()) {
        KIO::CopyJob* job = KIO::trash(QUrl::fromLocalFile(absPath), KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        if (job->exec()) // stays alive until the next event loop pass (deleteLater)
            return true;
        kioError = job->errorString();
    } else {
        kioError = QStringLiteral("no application object");
    }
    struct stat st {};
    if (::lstat(QFile::encodeName(absPath).constData(), &st) != 0 && errno == ENOENT)
        return true; // the job failed late (e.g. while cleaning up) but the item is gone
    if (QFile::moveToTrash(absPath))
        return true;
    if (error)
        *error = QStringLiteral("Could not move to the system trash: ") + kioError;
    return false;
}

bool trashToLocalFolder(const QString& root, const QString& absPath, bool isFolder, QString* error,
                        QString* trashedPath)
{
    const QString trashDir = root + QStringLiteral("/.trash");
    const QByteArray trashEnc = encodeName(trashDir);
    if (::mkdir(trashEnc.constData(), 0777) != 0 && errno != EEXIST) {
        if (error)
            *error = QStringLiteral("Cannot create .trash: ") + errnoString(errno);
        return false;
    }
    const QString name = QFileInfo(absPath).fileName();
    QString base = name;
    QString ext;
    if (!isFolder)
        splitName(name, &base, &ext);
    const QByteArray from = encodeName(absPath);
    for (int n = 0; n < 100000; ++n) {
        QString candidate = n == 0 ? name : QString(base + u' ' + QString::number(n));
        if (n > 0 && !ext.isEmpty())
            candidate += u'.' + ext;
        const QString dest = trashDir + u'/' + candidate;
        const int e = renameNoReplace(from, encodeName(dest));
        if (e == 0) {
            if (trashedPath)
                *trashedPath = dest;
            return true;
        }
        if (e != EEXIST && e != ENOTEMPTY) {
            if (error)
                *error = QStringLiteral("Cannot move to .trash: ") + errnoString(e);
            return false;
        }
    }
    if (error)
        *error = QStringLiteral("Cannot move to .trash: too many name collisions");
    return false;
}

} // namespace oc::vaultfs
