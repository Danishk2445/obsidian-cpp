#pragma once

// Shared fixtures for the workspace tests: test views, a TFile factory (the Vault normally creates
// them) and a compact textual description of the layout tree.

#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"
#include "ui/workspace/workspace_p.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QMenu>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <memory>

namespace oc::test {

// ---- private field access for TFile fixtures (only the Vault may set them) ---------------------
// Explicit instantiation may name private members; this is the standard, well-defined way to reach
// them from a test without changing the frozen Vault header.
template <typename Tag, typename Tag::type M>
struct Rob {
    friend typename Tag::type get(Tag) { return M; }
};
struct TAbsPathTag {
    using type = QString TAbstractFile::*;
    friend type get(TAbsPathTag);
};
struct TAbsNameTag {
    using type = QString TAbstractFile::*;
    friend type get(TAbsNameTag);
};
struct TFileBaseTag {
    using type = QString TFile::*;
    friend type get(TFileBaseTag);
};
struct TFileExtTag {
    using type = QString TFile::*;
    friend type get(TFileExtTag);
};
template struct Rob<TAbsPathTag, &TAbstractFile::m_path>;
template struct Rob<TAbsNameTag, &TAbstractFile::m_name>;
template struct Rob<TFileBaseTag, &TFile::m_basename>;
template struct Rob<TFileExtTag, &TFile::m_extension>;

inline std::unique_ptr<TFile> makeFile(const QString& path)
{
    auto f = std::make_unique<TFile>();
    TAbstractFile* a = f.get();
    const QString name = path.section(QLatin1Char('/'), -1);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    a->*get(TAbsPathTag()) = path;
    a->*get(TAbsNameTag()) = name;
    f.get()->*get(TFileBaseTag()) = dot > 0 ? name.left(dot) : name;
    f.get()->*get(TFileExtTag()) = dot > 0 ? name.mid(dot + 1) : QString();
    return f;
}

// ---- test views ------------------------------------------------------------------------------
// A markdown-like file view: without a vault the path is kept by FileView and shown as the title.
class TestNoteView : public FileView {
public:
    using FileView::FileView;
    QString viewType() const override { return QStringLiteral("markdown"); }
    QString icon() const override { return QStringLiteral("file-text"); }
    bool canAcceptExtension(const QString& ext) const override { return ext == u"md"; }
    void loadFile(TFile* file) override
    {
        m_file = file;
        ++loads;
    }
    QJsonObject ephemeralState() const override { return eState; }
    void setEphemeralState(const QJsonObject& e) override { eState = e; }
    void populatePaneMenu(QMenu* menu) override { menu->addAction(QStringLiteral("Note action")); }
    QJsonObject eState;
    int loads = 0;
};

// A navigable non-file view with a persistent "name" and an ephemeral "scroll".
class TestGraphView : public View {
public:
    using View::View;
    QString viewType() const override { return QStringLiteral("graph"); }
    QString displayText() const override { return m_state.value(QStringLiteral("name")).toString(QStringLiteral("Graph")); }
    QString icon() const override { return QStringLiteral("git-fork"); }
    QJsonObject state() const override { return m_state; }
    void setState(const QJsonObject& s) override
    {
        m_state = s;
        emit titleChanged();
    }
    QJsonObject ephemeralState() const override { return m_eState; }
    void setEphemeralState(const QJsonObject& e) override { m_eState = e; }
    QJsonObject m_state, m_eState;
};

inline QJsonObject note(const QString& path) { return QJsonObject{{"type", "markdown"}, {"state", QJsonObject{{"file", path}}}}; }
inline QJsonObject graph(const QString& name) { return QJsonObject{{"type", "graph"}, {"state", QJsonObject{{"name", name}}}}; }

inline QJsonObject parseJson(const char* text)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(text), &err);
    EXPECT_EQ(err.error, QJsonParseError::NoError) << qPrintable(err.errorString());
    return doc.object();
}

// "V[T(a*,b),H[T(c*),T(d*)]]": V/H = vertical (side by side) / horizontal split, T = tab group,
// leaves by display text ("_" for an empty tab), * = current tab.
inline QString describe(const WorkspaceItem* item)
{
    if (!item)
        return QStringLiteral("null");
    if (item->isSplit()) {
        const auto* s = static_cast<const WorkspaceSplit*>(item);
        QStringList parts;
        for (const WorkspaceItem* c : s->children)
            parts << describe(c);
        return QString(s->vertical ? QStringLiteral("V[") : QStringLiteral("H[")) + parts.join(QLatin1Char(',')) + QLatin1Char(']');
    }
    const auto* t = static_cast<const WorkspaceTabs*>(item);
    QStringList parts;
    for (int i = 0; i < t->leaves.size(); ++i) {
        QString label = t->leaves[i]->displayText();
        if (t->leaves[i]->view() && t->leaves[i]->view()->viewType() == u"empty")
            label = QStringLiteral("_");
        if (i == t->currentTab)
            label += QLatin1Char('*');
        parts << label;
    }
    return QStringLiteral("T(") + parts.join(QLatin1Char(',')) + QLatin1Char(')');
}

class WorkspaceFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (m_tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (m_tmp.path() + "/cache").toUtf8());
        if (!ThemeManager::instance()) {
            static ThemeManager* tm = new ThemeManager(qApp);
            tm->setMode(ThemeManager::Mode::Dark);
            tm->apply();
        }
        registerCoreViews(m_registry);
        m_registry.registerView(QStringLiteral("markdown"), [](WorkspaceLeaf* l) -> View* { return new TestNoteView(l); });
        m_registry.registerExtensions({QStringLiteral("md")}, QStringLiteral("markdown"));
        m_registry.registerView(QStringLiteral("graph"), [](WorkspaceLeaf* l) -> View* { return new TestGraphView(l); });
        m_session = std::make_unique<VaultSession>(QStringLiteral("0123456789abcdef"), m_tmp.path() + "/vault");
    }
    void TearDown() override { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

    std::unique_ptr<Workspace> makeWorkspace(bool load = true)
    {
        auto ws = std::make_unique<Workspace>(m_session.get(), &m_registry, nullptr);
        if (load)
            ws->loadLayout();
        return ws;
    }
    static WorkspaceImpl* impl(Workspace* ws) { return WorkspaceTabs::impl(ws); }
    static WorkspaceTabs* rootTabs(Workspace* ws, int index = 0)
    {
        QList<WorkspaceTabs*> all;
        impl(ws)->forEachTabs(impl(ws)->root, [&](WorkspaceTabs* t) { all << t; });
        return all.value(index);
    }

    QTemporaryDir m_tmp;
    ViewRegistry m_registry;
    std::unique_ptr<VaultSession> m_session;
};

} // namespace oc::test
