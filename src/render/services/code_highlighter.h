#pragma once

#include "render/layout/decoration.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/State>

#include <QHash>
#include <QString>

namespace oc {

// Syntax highlighting for fenced code blocks via KF6 SyntaxHighlighting. Token formats map to
// Obsidian's --code-* colours (StyleId::Code*), not to a KSH theme.
class CodeHighlighter {
public:
    static CodeHighlighter& instance();

    // Resolve an info-string language ("bash", "sh", "c", "ini", "js", "py", "c++", ...). Invalid when unknown.
    KSyntaxHighlighting::Definition definitionFor(const QString& language);

    // Highlight one line: appends marks (columns offset by `colOffset`) and returns the state for the next line.
    KSyntaxHighlighting::State highlightLine(const KSyntaxHighlighting::Definition& def, QStringView line,
                                            const KSyntaxHighlighting::State& previous, QList<Mark>& marks, int colOffset = 0);

private:
    CodeHighlighter();
    struct Impl;
    Impl* d;
};

} // namespace oc
