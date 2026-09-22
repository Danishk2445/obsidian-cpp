#pragma once

#include "app/cli.h"

#include <QObject>
#include <QPointer>

namespace oc {

class MainWindow;
class ThemeManager;
class VaultPicker;
class VaultRegistry;
class VaultSession;

// Startup and window management: vault picker, one MainWindow per vault, headless screenshot mode.
class Application : public QObject {
    Q_OBJECT
public:
    explicit Application(CliOptions opts, QObject* parent = nullptr);
    ~Application() override;

    int start(); // returns non-zero on fatal startup error

    VaultRegistry* registry() const { return m_registry; }
    ThemeManager* themes() const { return m_themes; }

    MainWindow* openVault(const QString& path);
    void showPicker();

private:
    void applyAppearance(VaultSession* session);
    void populateWindow(MainWindow* w);
    void finishHeadless(QWidget* target);

    CliOptions m_opts;
    ThemeManager* m_themes;
    VaultRegistry* m_registry;
    QPointer<VaultPicker> m_picker;
    QList<QPointer<MainWindow>> m_windows;
};

} // namespace oc
