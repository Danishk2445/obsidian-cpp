#include "core/bases/base_expr.h"

#include "core/bases/base_functions_p.h"

#include <cmath>

namespace oc::bases {

using Kind = ExprNode::Kind;

namespace {

// ---------------------------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------------------------
struct Token {
    enum class Type { End, Number, String, RegExp, Ident, Punct };
    Type type = Type::End;
    QString text; // punctuation / identifier / regexp pattern
    QString str;  // decoded string, regexp flags
    double num = 0;
    int from = 0, to = 0;
};

struct ParseError {
    QString message;
    bool semantic = false; // reported as is ("Invalid function name"), not as a position
};

bool isIdentStart(QChar c) { return c.isLetter() || c == u'_' || c == u'$' || (c.unicode() >= 0x80 && !c.isSpace() && !c.isPunct()); }
bool isIdentPart(QChar c) { return isIdentStart(c) || c.isDigit() || c.isMark(); }

class Lexer {
public:
    explicit Lexer(const QString& s) : m_s(s) {}

    int pos() const { return m_pos; }
    void reset(int pos) { m_pos = pos; }

    Token next(bool regexAllowed)
    {
        skipSpace();
        Token t;
        t.from = m_pos;
        if (m_pos >= m_s.size()) {
            t.to = m_pos;
            return t;
        }
        const QChar c = m_s[m_pos];
        if (c.isDigit()) {
            int p = m_pos;
            while (p < m_s.size() && m_s[p].isDigit())
                ++p;
            if (p + 1 < m_s.size() && m_s[p] == u'.' && m_s[p + 1].isDigit()) {
                ++p;
                while (p < m_s.size() && m_s[p].isDigit())
                    ++p;
            }
            if (p < m_s.size() && (m_s[p] == u'e' || m_s[p] == u'E')) {
                int q = p + 1;
                if (q < m_s.size() && (m_s[q] == u'+' || m_s[q] == u'-'))
                    ++q;
                if (q < m_s.size() && m_s[q].isDigit()) {
                    while (q < m_s.size() && m_s[q].isDigit())
                        ++q;
                    p = q;
                }
            }
            t.type = Token::Type::Number;
            t.text = m_s.mid(m_pos, p - m_pos);
            t.num = t.text.toDouble();
            m_pos = p;
            t.to = p;
            return t;
        }
        if (c == u'"' || c == u'\'') {
            t.type = Token::Type::String;
            t.str = readString(c);
            t.to = m_pos;
            return t;
        }
        if (c == u'/' && regexAllowed) {
            int p = m_pos + 1;
            bool inClass = false;
            for (; p < m_s.size(); ++p) {
                const QChar ch = m_s[p];
                if (ch == u'\n')
                    throw ParseError{errorAt(t.from)};
                if (ch == u'\\') {
                    ++p;
                    continue;
                }
                if (ch == u'[')
                    inClass = true;
                else if (ch == u']')
                    inClass = false;
                else if (ch == u'/' && !inClass)
                    break;
            }
            if (p >= m_s.size())
                throw ParseError{errorAt(t.from)};
            t.type = Token::Type::RegExp;
            t.text = m_s.mid(m_pos + 1, p - m_pos - 1);
            int q = p + 1;
            while (q < m_s.size() && m_s[q].isLetter())
                ++q;
            t.str = m_s.mid(p + 1, q - p - 1);
            m_pos = q;
            t.to = q;
            return t;
        }
        if (isIdentStart(c)) {
            int p = m_pos + 1;
            while (p < m_s.size() && isIdentPart(m_s[p]))
                ++p;
            t.type = Token::Type::Ident;
            t.text = m_s.mid(m_pos, p - m_pos);
            m_pos = p;
            t.to = p;
            return t;
        }
        static const char* const two[] = {"||", "&&", "==", "!=", "<=", ">="};
        for (const char* op : two) {
            if (QStringView(m_s).mid(m_pos, 2) == QLatin1String(op)) {
                t.type = Token::Type::Punct;
                t.text = QLatin1String(op);
                m_pos += 2;
                t.to = m_pos;
                return t;
            }
        }
        static const QString one = QStringLiteral("<>+-*/%!(),[].{}:");
        if (one.contains(c)) {
            t.type = Token::Type::Punct;
            t.text = QString(c);
            ++m_pos;
            t.to = m_pos;
            return t;
        }
        throw ParseError{errorAt(t.from)};
    }

    static QString errorAt(int pos) { return QStringLiteral("Unable to parse formula. (position %1)").arg(pos); }

private:
    void skipSpace()
    {
        while (m_pos < m_s.size() && m_s[m_pos].isSpace())
            ++m_pos;
    }

    // A JSON-escaped string body ('...' allows \' and a bare ").
    QString readString(QChar quote)
    {
        const int start = m_pos;
        QString out;
        int p = m_pos + 1;
        for (; p < m_s.size(); ++p) {
            const QChar ch = m_s[p];
            if (ch == quote)
                break;
            if (ch == u'\n' || ch.unicode() < 0x20)
                throw ParseError{errorAt(start)};
            if (ch != u'\\') {
                out += ch;
                continue;
            }
            if (p + 1 >= m_s.size())
                throw ParseError{errorAt(start)};
            const QChar e = m_s[++p];
            switch (e.unicode()) {
            case u'"': out += u'"'; break;
            case u'\\': out += u'\\'; break;
            case u'/': out += u'/'; break;
            case u'b': out += u'\b'; break;
            case u'f': out += u'\f'; break;
            case u'n': out += u'\n'; break;
            case u'r': out += u'\r'; break;
            case u't': out += u'\t'; break;
            case u'u': {
                bool ok = false;
                const uint code = m_s.mid(p + 1, 4).toUInt(&ok, 16);
                if (!ok || m_s.mid(p + 1, 4).size() != 4)
                    throw ParseError{errorAt(start)};
                out += QChar(char16_t(code));
                p += 4;
                break;
            }
            case u'\'':
                if (quote == u'\'') {
                    out += u'\'';
                    break;
                }
                [[fallthrough]];
            default:
                throw ParseError{errorAt(start)};
            }
        }
        if (p >= m_s.size())
            throw ParseError{errorAt(start)};
        m_pos = p + 1;
        return out;
    }

    const QString& m_s;
    int m_pos = 0;
};

// ---------------------------------------------------------------------------------------------
// Parser (Pratt)
// ---------------------------------------------------------------------------------------------
std::shared_ptr<ExprNode> makeNode(Kind k, int from, int to)
{
    auto n = std::make_shared<ExprNode>();
    n->kind = k;
    n->from = from;
    n->to = to;
    return n;
}

class Parser {
public:
    explicit Parser(const QString& s) : m_s(s), m_lex(s) {}

    ExprPtr parseProgram()
    {
        advance(true);
        const int start = m_tok.from;
        ExprPtr e;
        try {
            e = parseExpr(0);
        } catch (const ParseError& e) {
            if (e.semantic)
                throw;
            throw ParseError{Lexer::errorAt(start)};
        }
        if (m_tok.type != Token::Type::End)
            throw ParseError{Lexer::errorAt(e->to)};
        return e;
    }

private:
    void advance(bool regexAllowed) { m_tok = m_lex.next(regexAllowed); }
    bool isPunct(const char* p) const { return m_tok.type == Token::Type::Punct && m_tok.text == QLatin1String(p); }
    void expectPunct(const char* p)
    {
        if (!isPunct(p))
            throw ParseError{Lexer::errorAt(m_tok.from)};
    }

    static int infixPower(const QString& op, int* right)
    {
        struct P {
            const char* op;
            int l, r;
        };
        static const P table[] = {{"||", 1, 2}, {"&&", 3, 4}, {"==", 5, 6}, {"!=", 5, 6}, {"<", 7, 8}, {">", 7, 8}, {"<=", 7, 8},
                                  {">=", 7, 8}, {"+", 9, 10}, {"-", 9, 10}, {"*", 11, 12}, {"/", 11, 12}, {"%", 11, 12}};
        for (const P& p : table) {
            if (op == QLatin1String(p.op)) {
                *right = p.r;
                return p.l;
            }
        }
        return -1;
    }

    ExprPtr parseExpr(int minPower)
    {
        ExprPtr left = parsePrefix();
        for (;;) {
            if (m_tok.type != Token::Type::Punct)
                break;
            const QString op = m_tok.text;
            if (op == u"(" || op == u"." || op == u"[") {
                left = parsePostfix(left);
                continue;
            }
            int right = 0;
            const int power = infixPower(op, &right);
            if (power < 0 || power < minPower)
                break;
            advance(true);
            ExprPtr rhs = parseExpr(right);
            const bool compare = op == u"||" || op == u"&&" || op == u"==" || op == u"!=" || op == u"<" || op == u">" || op == u"<="
                                 || op == u">=";
            auto n = makeNode(compare ? Kind::Compare : Kind::Math, left->from, rhs->to);
            n->name = op;
            n->children = {left, rhs};
            left = n;
        }
        return left;
    }

    ExprPtr parsePostfix(ExprPtr left)
    {
        if (isPunct("(")) {
            // a call: the callee must be a name or a member access ending in a name
            QList<ExprPtr> args;
            advance(true);
            while (!isPunct(")")) {
                args << parseExpr(0);
                if (isPunct(","))
                    advance(true);
                else if (!isPunct(")"))
                    throw ParseError{Lexer::errorAt(m_tok.from)};
            }
            const int to = m_tok.to;
            advance(false);
            auto n = makeNode(Kind::Call, left->from, to);
            if (left->kind == Kind::ObjectAccess) {
                n->subject = left->children.value(0);
                n->name = left->name;
            } else if (left->kind == Kind::Ident) {
                n->name = left->name;
            } else {
                throw ParseError{QStringLiteral("Invalid function name"), true};
            }
            n->children = args;
            return n;
        }
        if (isPunct(".")) {
            advance(false);
            if (m_tok.type != Token::Type::Ident || isKeyword(m_tok.text))
                throw ParseError{Lexer::errorAt(m_tok.from)};
            auto n = makeNode(Kind::ObjectAccess, left->from, m_tok.to);
            n->name = m_tok.text;
            n->children = {left};
            advance(false);
            return n;
        }
        // "["
        advance(true);
        ExprPtr index = parseExpr(0);
        expectPunct("]");
        auto n = makeNode(Kind::ArrayAccess, left->from, m_tok.to);
        n->children = {left, index};
        advance(false);
        return n;
    }

    static bool isKeyword(const QString& s) { return s == u"true" || s == u"false" || s == u"null"; }

    ExprPtr parsePrefix()
    {
        const Token t = m_tok;
        switch (t.type) {
        case Token::Type::End:
            throw ParseError{Lexer::errorAt(t.from)};
        case Token::Type::Number: {
            auto n = makeNode(Kind::Primitive, t.from, t.to);
            n->value = Value::number(t.num);
            advance(false);
            return n;
        }
        case Token::Type::String: {
            auto n = makeNode(Kind::Primitive, t.from, t.to);
            n->value = Value::string(t.str);
            advance(false);
            return n;
        }
        case Token::Type::RegExp: {
            auto n = makeNode(Kind::RegExp, t.from, t.to);
            n->value = Value::regexp(t.text, t.str);
            advance(false);
            return n;
        }
        case Token::Type::Ident: {
            auto n = makeNode(Kind::Primitive, t.from, t.to);
            if (t.text == u"true" || t.text == u"false") {
                n->value = Value::boolean(t.text == u"true");
            } else if (t.text == u"null") {
                n->value = Value();
            } else {
                n->kind = Kind::Ident;
                n->name = t.text;
            }
            advance(false);
            return n;
        }
        case Token::Type::Punct:
            break;
        }
        if (t.text == u"(") {
            advance(true);
            ExprPtr inner = parseExpr(0);
            expectPunct(")");
            auto n = makeNode(Kind::Paren, t.from, m_tok.to);
            n->children = {inner};
            advance(false);
            return n;
        }
        if (t.text == u"[") {
            advance(true);
            QList<ExprPtr> items;
            while (!isPunct("]")) {
                items << parseExpr(0);
                if (isPunct(","))
                    advance(true);
                else if (!isPunct("]"))
                    throw ParseError{Lexer::errorAt(m_tok.from)};
            }
            auto n = makeNode(Kind::Array, t.from, m_tok.to);
            n->children = items;
            advance(false);
            return n;
        }
        if (t.text == u"{") {
            advance(false);
            auto n = makeNode(Kind::Object, t.from, t.to);
            while (!isPunct("}")) {
                QString key;
                if (m_tok.type == Token::Type::Ident)
                    key = m_tok.text;
                else if (m_tok.type == Token::Type::String)
                    key = m_tok.str;
                else if (m_tok.type == Token::Type::Number)
                    key = m_tok.text;
                else
                    throw ParseError{Lexer::errorAt(m_tok.from)};
                advance(false);
                expectPunct(":");
                advance(true);
                n->keys << key;
                n->children << parseExpr(0);
                if (isPunct(","))
                    advance(false);
                else if (!isPunct("}"))
                    throw ParseError{Lexer::errorAt(m_tok.from)};
            }
            n->to = m_tok.to;
            advance(false);
            return n;
        }
        if (t.text == u"!" || t.text == u"-") {
            advance(true);
            ExprPtr operand = parseExpr(13);
            if (t.text == u"-" && operand->kind == Kind::Primitive && operand->value.type() == ValueType::Number) {
                auto n = makeNode(Kind::Primitive, t.from, operand->to);
                n->value = Value::number(-operand->value.number());
                return n;
            }
            auto n = makeNode(t.text == u"!" ? Kind::Not : Kind::Negate, t.from, operand->to);
            n->children = {operand};
            return n;
        }
        throw ParseError{Lexer::errorAt(t.from)};
    }

    const QString& m_s;
    Lexer m_lex;
    Token m_tok;
};

// ---------------------------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------------------------

// A callback's scope (list.map(value + index), object.filter(key != "x"), reduce's acc).
class CallbackScope : public Scope {
public:
    CallbackScope(Scope& parent, QList<QPair<QString, Value>> vars) : m_parent(parent), m_vars(std::move(vars)) {}
    Value getByIdentifier(const QString& id) override
    {
        for (const auto& [name, v] : m_vars)
            if (name == id)
                return v;
        return m_parent.getByIdentifier(id);
    }
    QStringList keys() override
    {
        QStringList k = m_parent.keys();
        for (const auto& [name, v] : m_vars)
            k << name;
        return k;
    }
    const DataSource* dataSource() const override { return m_parent.dataSource(); }
    std::optional<Value> formulaValue(const QString& name) override { return m_parent.formulaValue(name); }
    QStringList formulaNames() override { return m_parent.formulaNames(); }

private:
    Scope& m_parent;
    QList<QPair<QString, Value>> m_vars;
};

// JavaScript's relational comparison of two primitives (numbers or strings).
struct JsPrim {
    bool isNumber = false;
    double n = 0;
    QString s;
};

int jsCompare(const JsPrim& a, const JsPrim& b, bool* undefinedResult)
{
    *undefinedResult = false;
    if (!a.isNumber && !b.isNumber) {
        const int c = QString::compare(a.s, b.s, Qt::CaseSensitive);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    const double x = a.isNumber ? a.n : Value::jsToNumber(a.s);
    const double y = b.isNumber ? b.n : Value::jsToNumber(b.s);
    if (std::isnan(x) || std::isnan(y)) {
        *undefinedResult = true;
        return 0;
    }
    return x < y ? -1 : x > y ? 1 : 0;
}

Value evalCompare(const ExprNode& n, Scope& scope)
{
    const ExprNode& l = *n.children[0];
    const ExprNode& r = *n.children[1];
    const QString& op = n.name;
    if (op == u"&&")
        return Value::boolean(evaluate(l, scope).isTruthy() && evaluate(r, scope).isTruthy());
    if (op == u"||")
        return Value::boolean(evaluate(l, scope).isTruthy() || evaluate(r, scope).isTruthy());
    const Value o = evaluate(l, scope);
    Value a = evaluate(r, scope);
    if (op == u"==")
        return Value::boolean(Value::looseEquals(o, a));
    if (op == u"!=")
        return Value::boolean(!Value::looseEquals(o, a));
    if (o.isNull() || a.isNull())
        return Value();
    JsPrim i, k;
    if (o.is(ValueClass::Date)) {
        i.isNumber = true;
        i.n = o.dateMs();
        if (a.is(ValueClass::String)) {
            if (auto d = Value::parseDate(a.text()))
                a = *d;
        }
    } else if (o.type() == ValueType::Duration) {
        i.isNumber = true;
        i.n = o.durationMs();
        if (a.is(ValueClass::String)) {
            if (auto d = Value::parseDuration(a.text()))
                a = *d;
        }
    } else if (o.type() == ValueType::Number) {
        i.isNumber = true;
        i.n = o.number();
    } else {
        i.s = o.toString();
    }
    if (a.is(ValueClass::Date)) {
        k.isNumber = true;
        k.n = a.dateMs();
    } else if (a.type() == ValueType::Duration) {
        k.isNumber = true;
        k.n = a.durationMs();
    } else if (a.type() == ValueType::Number) {
        k.isNumber = true;
        k.n = a.number();
    } else {
        k.s = a.toString();
    }
    bool undef = false;
    const int c = jsCompare(i, k, &undef);
    if (undef)
        return Value::boolean(false);
    if (op == u"<")
        return Value::boolean(c < 0);
    if (op == u">")
        return Value::boolean(c > 0);
    if (op == u"<=")
        return Value::boolean(c <= 0);
    if (op == u">=")
        return Value::boolean(c >= 0);
    return Value();
}

Value evalMath(const ExprNode& n, Scope& scope)
{
    const Value t = evaluate(*n.children[0], scope);
    Value r = evaluate(*n.children[1], scope);
    const QString& op = n.name;
    if (t.type() == ValueType::Number && r.type() == ValueType::Number) {
        const double a = t.number(), b = r.number();
        if (op == u"+")
            return Value::number(a + b);
        if (op == u"-")
            return Value::number(a - b);
        if (op == u"*")
            return Value::number(a * b);
        if (op == u"/")
            return Value::number(a / b);
        if (op == u"%")
            return Value::number(std::fmod(a, b));
        throw EvalError{QStringLiteral("Invalid operator")};
    }
    if (t.is(ValueClass::Date) && r.is(ValueClass::String)) {
        if (auto d = Value::parseDuration(r.text()))
            r = *d;
    }
    if (t.is(ValueClass::Date) && r.type() == ValueType::Duration) {
        if (op != u"+" && op != u"-")
            throw EvalError{QStringLiteral("Invalid date operator")};
        return r.addToDate(t, op == u"-");
    }
    if (t.type() == ValueType::Duration && r.type() == ValueType::Number && (op == u"*" || op == u"/")) {
        const double f = op == u"*" ? r.number() : 1 / r.number();
        const DurationParts& d = t.durationParts();
        DurationParts out;
        out.years = d.years * f;
        out.months = d.months * f;
        out.days = d.days * f;
        out.hours = d.hours * f;
        out.minutes = d.minutes * f;
        out.seconds = d.seconds * f;
        out.milliseconds = d.milliseconds * f;
        return Value::duration(out);
    }
    if (t.type() == ValueType::Duration && r.type() == ValueType::Duration && (op == u"+" || op == u"-")) {
        const double s = op == u"+" ? 1 : -1;
        const DurationParts& a = t.durationParts();
        const DurationParts& b = r.durationParts();
        DurationParts out;
        out.years = a.years + b.years * s;
        out.months = a.months + b.months * s;
        out.days = a.days + b.days * s;
        out.hours = a.hours + b.hours * s;
        out.minutes = a.minutes + b.minutes * s;
        out.seconds = a.seconds + b.seconds * s;
        out.milliseconds = a.milliseconds + b.milliseconds * s;
        return Value::duration(out);
    }
    if (t.is(ValueClass::List) && r.is(ValueClass::List) && op == u"+")
        return Value::list(t.items() + r.items());
    if (op == u"+" && (t.is(ValueClass::String) || r.is(ValueClass::String)))
        return Value::string((t.isNull() ? QString() : t.toString()) + (r.isNull() ? QString() : r.toString()));
    if (op == u"-" && t.is(ValueClass::Date) && r.is(ValueClass::Date)) {
        DurationParts d;
        d.milliseconds = t.dateMs() - r.dateMs();
        return Value::duration(d);
    }
    if (t.isNull() || r.isNull())
        return Value();
    throw EvalError{QStringLiteral("Invalid operator between ") + t.typeName() + QStringLiteral(" and ") + r.typeName()};
}

QString notEnough(const QString& fn) { return QStringLiteral("Cannot call function \"%1\", not enough arguments.").arg(fn); }
QString tooMany(const QString& fn) { return QStringLiteral("Cannot call function \"%1\", too many arguments.").arg(fn); }

Value evalCall(const ExprNode& n, Scope& scope)
{
    const QString& name = n.name;
    const QList<ExprPtr>& args = n.children;
    if (name == u"if") {
        const int count = int(args.size()) + (n.subject ? 1 : 0);
        if (count < 2)
            throw EvalError{notEnough(name)};
        if (count > 3)
            throw EvalError{tooMany(name)};
        const ExprNode* cond = n.subject ? n.subject.get() : args[0].get();
        const qsizetype first = n.subject ? 0 : 1;
        if (evaluate(*cond, scope).isTruthy())
            return evaluate(*args[first], scope);
        return args.size() - first == 2 ? evaluate(*args[first + 1], scope) : Value();
    }
    std::optional<Value> subject;
    const detail::Function* fn = nullptr;
    if (n.subject) {
        const Value s = evaluate(*n.subject, scope);
        fn = detail::findMethod(s, name);
        if (!fn && s.isNull())
            return Value();
        subject = s;
    } else {
        fn = detail::findGlobal(name);
    }
    if (subject && subject->is(ValueClass::List)) {
        const Value& list = *subject;
        if (name == u"reduce") {
            if (args.size() < 2)
                throw EvalError{notEnough(name)};
            if (args.size() > 2)
                throw EvalError{tooMany(name)};
            Value acc = evaluate(*args[1], scope);
            const QList<Value>& items = list.items();
            for (qsizetype i = 0; i < items.size(); ++i) {
                CallbackScope cb(scope, {{QStringLiteral("index"), Value::number(double(i))}, {QStringLiteral("value"), items[i]},
                                         {QStringLiteral("acc"), acc}});
                acc = evaluate(*args[0], cb);
            }
            return acc;
        }
        if (name == u"filter" || name == u"map") {
            if (args.isEmpty())
                throw EvalError{notEnough(name)};
            if (args.size() > 1)
                throw EvalError{tooMany(name)};
            QList<Value> out;
            const QList<Value>& items = list.items();
            for (qsizetype i = 0; i < items.size(); ++i) {
                CallbackScope cb(scope, {{QStringLiteral("index"), Value::number(double(i))}, {QStringLiteral("value"), items[i]}});
                if (name == u"filter") {
                    if (evaluate(*args[0], cb).isTruthy())
                        out << items[i];
                } else {
                    out << evaluate(*args[0], cb);
                }
            }
            return Value::list(out);
        }
    } else if (subject && subject->type() == ValueType::Object && (name == u"filter" || name == u"map")) {
        if (args.isEmpty())
            throw EvalError{notEnough(name)};
        if (args.size() > 1)
            throw EvalError{tooMany(name)};
        const ObjectData* o = subject->objectData();
        if (name == u"filter") {
            auto data = std::make_shared<ObjectData>();
            for (int i = 0; i < o->keys.size(); ++i) {
                const Value v = o->at(i);
                CallbackScope cb(scope, {{QStringLiteral("key"), Value::string(o->keys[i])}, {QStringLiteral("value"), v}});
                if (evaluate(*args[0], cb).isTruthy()) {
                    data->keys << o->keys[i];
                    data->values << std::make_shared<Value>(v);
                }
            }
            return Value::object(data);
        }
        QList<Value> out;
        for (int i = 0; i < o->keys.size(); ++i) {
            CallbackScope cb(scope, {{QStringLiteral("key"), Value::string(o->keys[i])}, {QStringLiteral("value"), o->at(i)}});
            out << evaluate(*args[0], cb);
        }
        return Value::list(out);
    }
    if (!fn) {
        if (subject)
            throw EvalError{QStringLiteral("Cannot find function \"%1\" on type %2").arg(name, subject->typeName())};
        throw EvalError{QStringLiteral("Cannot find function \"%1\"").arg(name)};
    }
    const QList<detail::Param>& params = fn->params;
    int x = 0;
    detail::Args typed;
    auto push = [&](const Value& e) -> bool {
        if (x == params.size()) {
            if (params.isEmpty() || !params.last().variadic)
                throw EvalError{tooMany(name)};
            --x;
        }
        const detail::Param& p = params[x];
        if (p.optional && e.isNull()) {
            typed << std::nullopt;
        } else {
            bool ok = false;
            for (ValueClass c : p.types)
                ok |= e.is(c);
            if (!ok) {
                if (e.isNull())
                    return false;
                throw EvalError{QStringLiteral("Type error in \"%1\", parameter \"%2\". Expected %3, given %4.")
                                    .arg(name, p.name, detail::className(p.types.value(0)), e.typeName())};
            }
            typed << e;
        }
        ++x;
        return true;
    };
    if (subject && !push(*subject))
        return Value();
    for (const ExprPtr& a : args)
        if (!push(evaluate(*a, scope)))
            return Value();
    for (int h = x; h < params.size(); ++h)
        if (!params[h].optional && !params[h].variadic)
            throw EvalError{notEnough(name)};
    detail::CallContext ctx{scope};
    return fn->apply(ctx, typed);
}

Value evalArrayAccess(const ExprNode& n, Scope& scope)
{
    const Value t = evaluate(*n.children[0], scope);
    if (t.isNull())
        return Value();
    const Value idx = evaluate(*n.children[1], scope);
    if (idx.isNull())
        return Value();
    if (t.is(ValueClass::List)) {
        if (idx.type() != ValueType::Number)
            throw EvalError{QStringLiteral("Array index must be an integer. Got ") + idx.typeName()};
        double i = idx.number();
        const QList<Value>& items = t.items();
        if (i < 0)
            i += double(items.size());
        if (i != std::floor(i) || i < 0 || i >= double(items.size()))
            return Value();
        return items[qsizetype(i)];
    }
    if (t.type() == ValueType::Object) {
        if (!idx.is(ValueClass::String))
            throw EvalError{QStringLiteral("Object access must be an string. Got ") + idx.typeName()};
        return t.objectAccess(idx.text()).value_or(Value());
    }
    if (t.is(ValueClass::String)) {
        if (idx.type() != ValueType::Number)
            throw EvalError{QStringLiteral("String index must be an integer. Got ") + idx.typeName()};
        const double i = idx.number();
        // String.charAt(): ToIntegerOrInfinity, "" out of range
        const double k = std::isnan(i) ? 0 : std::trunc(i);
        if (k < 0 || k >= double(t.text().size()))
            return Value::string(QString());
        return Value::string(QString(t.text().at(qsizetype(k))));
    }
    if (idx.is(ValueClass::String)) {
        std::optional<Value> r = t.objectAccess(idx.toString());
        if (!r)
            throw EvalError{QStringLiteral("Cannot find \"%1\" on type %2").arg(idx.text(), t.typeName())};
        return *r;
    }
    throw EvalError{QStringLiteral("Cannot access index \"%1\" of non-array value").arg(idx.toString())};
}

Value evalObjectAccess(const ExprNode& n, Scope& scope)
{
    const Value t = evaluate(*n.children[0], scope);
    if (t.isNull())
        return Value();
    std::optional<Value> r = t.objectAccess(n.name);
    if (!r)
        throw EvalError{QStringLiteral("Cannot find \"%1\" on type %2").arg(n.name, t.typeName())};
    return *r;
}

} // namespace

Value evaluate(const ExprNode& node, Scope& scope)
{
    switch (node.kind) {
    case Kind::Invalid:
    case Kind::Empty:
        return Value();
    case Kind::Compare:
        return evalCompare(node, scope);
    case Kind::Math:
        return evalMath(node, scope);
    case Kind::Negate: {
        const Value v = evaluate(*node.children[0], scope);
        if (v.isNull())
            return v;
        if (v.type() != ValueType::Number)
            throw EvalError{QStringLiteral("Unable to negate ") + v.toString()};
        return Value::number(-v.number());
    }
    case Kind::Not: {
        const Value v = evaluate(*node.children[0], scope);
        return v.isNull() ? v : Value::boolean(!v.isTruthy());
    }
    case Kind::Call:
        return evalCall(node, scope);
    case Kind::ArrayAccess:
        return evalArrayAccess(node, scope);
    case Kind::ObjectAccess:
        return evalObjectAccess(node, scope);
    case Kind::Paren:
        return evaluate(*node.children[0], scope);
    case Kind::Primitive:
    case Kind::RegExp:
        return node.value;
    case Kind::Array: {
        QList<Value> items;
        items.reserve(node.children.size());
        for (const ExprPtr& c : node.children)
            items << evaluate(*c, scope);
        return Value::list(items);
    }
    case Kind::Object: {
        auto data = std::make_shared<ObjectData>();
        for (qsizetype i = 0; i < node.children.size(); ++i) {
            const Value v = evaluate(*node.children[i], scope);
            const int existing = data->indexOf(node.keys[i]);
            if (existing >= 0) {
                data->values[existing] = std::make_shared<Value>(v);
                continue;
            }
            data->keys << node.keys[i];
            data->values << std::make_shared<Value>(v);
        }
        return Value::object(data);
    }
    case Kind::Ident:
        return scope.getByIdentifier(node.name);
    }
    return Value();
}

ExprPtr parseExpression(const QString& text)
{
    if (text.trimmed().isEmpty())
        return makeNode(Kind::Empty, 0, 0);
    try {
        Parser p(text);
        return p.parseProgram();
    } catch (const ParseError& e) {
        auto n = makeNode(Kind::Invalid, 0, int(text.size()));
        n->name = e.message;
        return n;
    }
}

// ---------------------------------------------------------------------------------------------
// Formula
// ---------------------------------------------------------------------------------------------
Formula::Formula(const QString& text)
    : m_text(text)
    , m_root(parseExpression(text))
{
}

QString Formula::errorMessage() const
{
    if (!isInvalid())
        return {};
    return m_root->name.isEmpty() ? QStringLiteral("Unable to parse formula.") : m_root->name;
}

Value Formula::value(Scope& scope) const
{
    try {
        return bases::evaluate(*m_root, scope);
    } catch (const EvalError& e) {
        return Value::error(e.message);
    }
}

Value Formula::evaluate(Scope& scope) const { return bases::evaluate(*m_root, scope); }

bool Formula::test(Scope& scope) const
{
    if (isInvalid() || isEmpty())
        return false;
    return bases::evaluate(*m_root, scope).isTruthy();
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
QString propertyExpression(const QString& propertyId)
{
    if (propertyId.isEmpty())
        return {};
    const qsizetype dot = propertyId.indexOf(u'.');
    QString type = QStringLiteral("note");
    QString name = propertyId;
    if (dot >= 0) {
        const QString prefix = propertyId.left(dot);
        if (prefix == u"note" || prefix == u"formula" || prefix == u"file") {
            type = prefix;
            name = propertyId.mid(dot + 1);
        }
    }
    if (type == u"file" && name == u"file")
        return QStringLiteral("file");
    // yX(): is the name not a plain identifier on its own?
    const ExprPtr parsed = parseExpression(name);
    const bool plain = parsed->kind == Kind::Ident && parsed->name == name;
    if (!plain) {
        QString quoted = name;
        quoted.replace(u'\\', QLatin1String("\\\\")).replace(u'"', QLatin1String("\\\""));
        return type + QStringLiteral("[\"") + quoted + QStringLiteral("\"]");
    }
    return type == u"note" ? name : type + u'.' + name;
}

QList<FunctionInfo> allFunctions()
{
    QList<FunctionInfo> out;
    for (const auto& [name, type] : detail::registeredFunctions())
        out << FunctionInfo{name, type};
    return out;
}

} // namespace oc::bases
