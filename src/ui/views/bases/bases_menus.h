#pragma once

// The toolbar menus of a base (Obsidian's .bases-toolbar-menu popups), as MenuPanels:
//  - views: the base's views (search, click to switch, chevron to configure), "Add view";
//    "Configure view": name, layout (Table / Cards / List) and the layout's options (row height;
//    card size, image property, image fit, aspect ratio; markers, indent properties, separator),
//    ⋮ Default view / Duplicate view / Delete view;
//  - results: "Limit number of results", Show all, Copy to clipboard, Export CSV...;
//  - sort: "Group by" (property + direction) and "Sort by" rows (property + direction), Add sort;
//  - filter: "All views" (the base's filters) and "This view" filter groups — conjunction (all /
//    any / none of the following), rows of property + operator + value (the operators the
//    property's type offers: is, contains, has tag, in folder, links to, is empty... and their
//    negations) or an advanced formula, nested groups;
//  - properties: every property with a visibility checkbox (the view's columns, in order), find or
//    create, "Add formula", "Hide all", and a page per property: display name, formula editor;
//  - summaries: the summary functions for a column's values (with their results), custom ones,
//    "Add summary", "Hide summary".

#include "core/bases/base_value.h"
#include "ui/views/bases/bases_widgets.h"

namespace oc::basesui {

class BasesController;

MenuPanel* openViewsMenu(BasesController* c, QWidget* anchor);
MenuPanel* openConfigureViewMenu(BasesController* c, QWidget* anchor, const QString& viewName);
MenuPanel* openResultsMenu(BasesController* c, QWidget* anchor);
MenuPanel* openSortMenu(BasesController* c, QWidget* anchor);
MenuPanel* openFilterMenu(BasesController* c, QWidget* anchor);
MenuPanel* openPropertiesMenu(BasesController* c, QWidget* anchor);
MenuPanel* openEditPropertyMenu(BasesController* c, const QString& propertyId);
MenuPanel* openSummaryMenu(BasesController* c, const QString& propertyId, const QList<bases::Value>& values, QWidget* anchor);

// The simple filter builder's view of one expression (Obsidian's pZ): property + operator + value,
// or nullopt when only the advanced (formula) mode can show it.
struct SimpleFilter {
    QString property;  // property id
    QString op;        // "==", "!=", "<", ">", "<=", ">=" or a function name ("contains", "hasTag"...)
    bool inverse = false;
    QStringList values; // the right-hand side (several for list functions)
    bool isList = false;
};
std::optional<SimpleFilter> parseSimpleFilter(const QString& expression);
QString serializeSimpleFilter(const SimpleFilter& f, bases::ValueType lhsType = bases::ValueType::String);
// The operators (value, display) the simple builder offers for a property whose values have type
// `type` (Obsidian's updateOperators()).
struct FilterOperator {
    QString value; // "==", "startswith", "!startswith", ...
    QString display;
    QString function; // "" for comparisons
    bool inverse = false;
    bool takesValue = true;
    bool variadic = false;
};
QList<FilterOperator> filterOperators(bases::ValueType type);

} // namespace oc::basesui
