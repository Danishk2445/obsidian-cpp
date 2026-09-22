#include "render/services/code_highlighter.h"

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>

namespace oc {

namespace {
StyleId styleFor(KSyntaxHighlighting::Theme::TextStyle s)
{
    using TS = KSyntaxHighlighting::Theme::TextStyle;
    switch (s) {
    case TS::Keyword:
    case TS::ControlFlow:
    case TS::Import:
        return StyleId::CodeKeyword;
    case TS::Function:
    case TS::Extension:
    case TS::DataType:
        return StyleId::CodeFunction;
    case TS::Variable:
    case TS::Attribute:
    case TS::Preprocessor:
        return StyleId::CodeProperty;
    case TS::Operator:
        return StyleId::CodeOperator;
    case TS::BuiltIn:
    case TS::DecVal:
    case TS::BaseN:
    case TS::Float:
    case TS::Constant:
        return StyleId::CodeValue;
    case TS::Char:
    case TS::SpecialChar:
    case TS::String:
    case TS::VerbatimString:
    case TS::SpecialString:
        return StyleId::CodeString;
    case TS::Comment:
    case TS::Documentation:
    case TS::Annotation:
    case TS::CommentVar:
    case TS::RegionMarker:
        return StyleId::CodeComment;
    case TS::Information:
    case TS::Warning:
    case TS::Alert:
    case TS::Error:
        return StyleId::CodeImportant;
    case TS::Others:
        return StyleId::CodePunctuation;
    default:
        return StyleId::Normal;
    }
}

class Collector : public KSyntaxHighlighting::AbstractHighlighter {
public:
    QList<Mark>* out = nullptr;
    int offset = 0;
    KSyntaxHighlighting::State run(QStringView line, const KSyntaxHighlighting::State& prev) { return highlightLine(line, prev); }

protected:
    void applyFormat(int offsetCol, int length, const KSyntaxHighlighting::Format& format) override
    {
        if (!out || length <= 0)
            return;
        const StyleId s = styleFor(format.textStyle());
        if (s == StyleId::Normal)
            return;
        out->push_back({offset + offsetCol, offset + offsetCol + length, s});
    }
};
} // namespace

struct CodeHighlighter::Impl {
    KSyntaxHighlighting::Repository repo;
    QHash<QString, KSyntaxHighlighting::Definition> cache;
    Collector collector;
};

CodeHighlighter& CodeHighlighter::instance()
{
    static CodeHighlighter h;
    return h;
}

CodeHighlighter::CodeHighlighter()
    : d(new Impl)
{
}

KSyntaxHighlighting::Definition CodeHighlighter::definitionFor(const QString& language)
{
    const QString lang = language.trimmed().toLower();
    if (lang.isEmpty())
        return {};
    auto it = d->cache.constFind(lang);
    if (it != d->cache.cend())
        return *it;
    // Common Markdown/Prism aliases -> KSH definition names.
    static const QHash<QString, QString> alias = {
        {QStringLiteral("sh"), QStringLiteral("Bash")},          {QStringLiteral("shell"), QStringLiteral("Bash")},
        {QStringLiteral("bash"), QStringLiteral("Bash")},        {QStringLiteral("zsh"), QStringLiteral("Zsh")},
        {QStringLiteral("console"), QStringLiteral("Bash")},     {QStringLiteral("c"), QStringLiteral("C")},
        {QStringLiteral("h"), QStringLiteral("C")},              {QStringLiteral("cpp"), QStringLiteral("C++")},
        {QStringLiteral("c++"), QStringLiteral("C++")},          {QStringLiteral("cc"), QStringLiteral("C++")},
        {QStringLiteral("hpp"), QStringLiteral("C++")},          {QStringLiteral("ini"), QStringLiteral("INI Files")},
        {QStringLiteral("conf"), QStringLiteral("INI Files")},   {QStringLiteral("toml"), QStringLiteral("TOML")},
        {QStringLiteral("js"), QStringLiteral("JavaScript")},    {QStringLiteral("javascript"), QStringLiteral("JavaScript")},
        {QStringLiteral("ts"), QStringLiteral("TypeScript")},    {QStringLiteral("typescript"), QStringLiteral("TypeScript")},
        {QStringLiteral("py"), QStringLiteral("Python")},        {QStringLiteral("python"), QStringLiteral("Python")},
        {QStringLiteral("rs"), QStringLiteral("Rust")},          {QStringLiteral("rust"), QStringLiteral("Rust")},
        {QStringLiteral("go"), QStringLiteral("Go")},            {QStringLiteral("java"), QStringLiteral("Java")},
        {QStringLiteral("json"), QStringLiteral("JSON")},        {QStringLiteral("yaml"), QStringLiteral("YAML")},
        {QStringLiteral("yml"), QStringLiteral("YAML")},         {QStringLiteral("xml"), QStringLiteral("XML")},
        {QStringLiteral("html"), QStringLiteral("HTML")},        {QStringLiteral("css"), QStringLiteral("CSS")},
        {QStringLiteral("sql"), QStringLiteral("SQL")},          {QStringLiteral("lua"), QStringLiteral("Lua")},
        {QStringLiteral("md"), QStringLiteral("Markdown")},      {QStringLiteral("markdown"), QStringLiteral("Markdown")},
        {QStringLiteral("diff"), QStringLiteral("Diff")},        {QStringLiteral("patch"), QStringLiteral("Diff")},
        {QStringLiteral("cmake"), QStringLiteral("CMake")},      {QStringLiteral("make"), QStringLiteral("Makefile")},
        {QStringLiteral("makefile"), QStringLiteral("Makefile")},{QStringLiteral("dockerfile"), QStringLiteral("Dockerfile")},
        {QStringLiteral("ps1"), QStringLiteral("PowerShell")},   {QStringLiteral("powershell"), QStringLiteral("PowerShell")},
        {QStringLiteral("rb"), QStringLiteral("Ruby")},          {QStringLiteral("ruby"), QStringLiteral("Ruby")},
        {QStringLiteral("php"), QStringLiteral("PHP (HTML)")},   {QStringLiteral("kt"), QStringLiteral("Kotlin")},
        {QStringLiteral("swift"), QStringLiteral("Swift")},      {QStringLiteral("nix"), QStringLiteral("Nix")},
        {QStringLiteral("fstab"), QStringLiteral("fstab")},      {QStringLiteral("tex"), QStringLiteral("LaTeX")},
        {QStringLiteral("latex"), QStringLiteral("LaTeX")},      {QStringLiteral("vim"), QStringLiteral("Vim Script")},
        {QStringLiteral("just"), QStringLiteral("Just")},        {QStringLiteral("tmux"), QStringLiteral("Bash")},
    };
    KSyntaxHighlighting::Definition def;
    if (auto a = alias.constFind(lang); a != alias.cend())
        def = d->repo.definitionForName(*a);
    if (!def.isValid())
        def = d->repo.definitionForName(language.trimmed());
    if (!def.isValid())
        def = d->repo.definitionForFileName(QStringLiteral("x.") + lang);
    d->cache.insert(lang, def);
    return def;
}

KSyntaxHighlighting::State CodeHighlighter::highlightLine(const KSyntaxHighlighting::Definition& def, QStringView line,
                                                         const KSyntaxHighlighting::State& previous, QList<Mark>& marks, int colOffset)
{
    d->collector.setDefinition(def);
    d->collector.out = &marks;
    d->collector.offset = colOffset;
    return d->collector.run(line, previous);
}

} // namespace oc
