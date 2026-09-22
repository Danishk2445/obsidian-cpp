#pragma once

#include "markdown/syntax_tree.h"
#include "text/text.h"

namespace oc::md {

// Read-only view of a parsed document (text + block tree + version). Implemented by oc::Document
// (live, editable) and by nested renderers (callout bodies, embeds, hover previews).
class DocSource {
public:
    virtual ~DocSource() = default;
    virtual const Text& text() const = 0;
    virtual const SyntaxTree& syntax() const = 0;
    virtual quint64 version() const = 0;
    // Vault path of the file this content comes from ("" for anonymous content).
    virtual QString sourcePath() const { return {}; }
};

// A standalone parsed snapshot (for nested rendering).
class StaticDocSource : public DocSource {
public:
    explicit StaticDocSource(const Text& t, QString path = {}) : m_text(t), m_path(std::move(path)) { m_tree.parse(m_text); }
    const Text& text() const override { return m_text; }
    const SyntaxTree& syntax() const override { return m_tree; }
    quint64 version() const override { return 1; }
    QString sourcePath() const override { return m_path; }

private:
    Text m_text;
    SyntaxTree m_tree;
    QString m_path;
};

} // namespace oc::md
