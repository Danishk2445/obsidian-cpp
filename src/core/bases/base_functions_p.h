#pragma once

// Internal: the function registry of the formula language (Obsidian's RK), used by the evaluator.

#include "core/bases/base_value.h"

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace oc::bases::detail {

struct Param {
    QString name;
    QList<ValueClass> types;
    bool optional = false;
    bool variadic = false;
};

// What a function body can reach: the data source (links, files) and the calling scope.
struct CallContext {
    Scope& scope;
    const DataSource* ds() const { return scope.dataSource(); }
};

// Arguments after type checking: an absent optional argument is nullopt.
using Args = QList<std::optional<Value>>;

struct Function {
    QString name; // as registered ("containsAny"); lookups are case-insensitive
    QList<Param> params;
    std::function<Value(CallContext&, const Args&)> apply;
};

// Instance function for `receiver` (walking its class chain), nullptr when none.
const Function* findMethod(const Value& receiver, const QString& name);
// Global function, nullptr when none.
const Function* findGlobal(const QString& name);
// The class name used in type error messages for a parameter type ("String", "Any", ...).
QString className(ValueClass c);

// Every registered function (global ones have an empty type).
QList<QPair<QString, QString>> registeredFunctions();

} // namespace oc::bases::detail
