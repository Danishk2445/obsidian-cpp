#pragma once

// The formula / filter language of Obsidian's Bases: a lexer and Pratt parser producing the AST of
// Obsidian's formula classes, and their evaluation.
//
//   literals        1  1.5  "text"  'text'  true  false  null  /regex/i  [1, 2]  {a: 1} (extension)
//   operators       ||  &&  ==  !=  <  >  <=  >=  +  -  *  /  %  !  -x  (grouping)
//   access          file.name  note.status  note["my prop"]  formula.total  this.file  list[0]
//   calls           if(c, a, b)  now()  date("2024-01-31")  x.contains("a")  list.map(value + index)
//
// Semantics follow Obsidian 1.13: && / || give booleans, == is loose equality, comparisons with a
// null side give null, dates compare by time and parse strings on the right, date ± duration
// ("1 week" strings convert), date - date gives a duration, "+" concatenates when a side is a
// string (null counts as ""), unknown methods on null give null. Errors are EvalError exceptions
// with Obsidian's messages ("Cannot find function \"x\" on type String", "Type error in ...").

#include "core/bases/base_value.h"

#include <QList>
#include <QString>

#include <memory>

namespace oc::bases {

struct ExprNode {
    enum class Kind {
        Invalid,
        Empty,
        Compare,      // op: || && == != < > <= >=
        Math,         // op: + - * / %
        Negate,
        Not,
        Call,         // name; subject (receiver) or null; args
        ArrayAccess,  // children: [array, index]
        ObjectAccess, // children: [object]; name
        Paren,
        Primitive,    // value
        RegExp,       // value
        Array,        // children
        Object,       // keys + children (an extension: Obsidian has no object literals)
        Ident,        // name
    };
    Kind kind = Kind::Empty;
    QString name; // operator, function name, identifier, member name
    Value value;
    std::shared_ptr<const ExprNode> subject;
    QList<std::shared_ptr<const ExprNode>> children;
    QStringList keys;
    int from = 0, to = 0; // source span
};
using ExprPtr = std::shared_ptr<const ExprNode>;

// One formula (Obsidian's fX): its text and parsed form.
class Formula {
public:
    explicit Formula(const QString& text = {});

    const QString& text() const { return m_text; }
    const ExprPtr& root() const { return m_root; }
    bool isInvalid() const { return m_root->kind == ExprNode::Kind::Invalid; }
    bool isEmpty() const { return m_root->kind == ExprNode::Kind::Empty; }
    // "Unable to parse formula. (position 7)", "Invalid function name", ...
    QString errorMessage() const;

    // The value; evaluation errors become Error values (Obsidian's fX.getValue).
    Value value(Scope& scope) const;
    // The value; throws EvalError.
    Value evaluate(Scope& scope) const;
    // Is it truthy? Throws EvalError (filters report them). Invalid / empty formulas are false.
    bool test(Scope& scope) const;

private:
    QString m_text;
    ExprPtr m_root;
};
using FormulaPtr = std::shared_ptr<const Formula>;

// Parse `text` (Obsidian's mX + fromParsedResult). Never null: parse errors give an Invalid node
// whose name is the message.
ExprPtr parseExpression(const QString& text);

// Evaluate a node (throws EvalError).
Value evaluate(const ExprNode& node, Scope& scope);

// Obsidian's gQ(): the expression text of a property id ("note.status" -> "status",
// "note.my prop" -> "note[\"my prop\"]", "formula.x" -> "formula.x", "file.file" -> "file").
QString propertyExpression(const QString& propertyId);

// Function metadata (for autocompletion / tests).
struct FunctionInfo {
    QString name;
    QString type; // "" for global functions, else the receiver class ("String", "List", ...)
};
QList<FunctionInfo> allFunctions();

} // namespace oc::bases
