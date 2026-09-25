#pragma once

#include <QMenu>

namespace oc {

// A context menu for `owner` that may outlive it. QMenu::exec() runs a nested event loop in which an
// item (Close, Delete file...) or an external event can destroy the widget the menu was opened for; a
// stack `QMenu menu(owner)` would then be deleted twice. This menu is parented to owner's window
// (style, transient parent), closes when owner is destroyed, and is deleted after it hides.
inline QMenu* ownedMenu(QWidget* owner)
{
    auto* menu = new QMenu(owner->window());
    QObject::connect(owner, &QObject::destroyed, menu, &QMenu::close);
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    return menu;
}

} // namespace oc
