#pragma once

// The UI of one base (Obsidian's query controller nJ): the header toolbar (views menu, result
// count, Sort, Filter, Properties, Search, New), the search row, the error line and the current
// view's layout (table / cards / list), all driven by a QueryRunner over the session's vault.
//
// The model changes only through editBase()/editView(): the controller shows the new model at once
// and emits baseEdited() so that its host saves it — the .base file view writes the file, a
// ```base code block rewrites its fence, an embed writes the embedded file. Hosts call setBase()
// with the text they read (a parse error shows "Unable to parse your base file:" and the message).
//
// Mode::File is a .base tab (the layout scrolls, the header has side padding); Mode::Embed is a
// base inside a note (no vertical scrolling: preferredHeight() is the whole content).

#include "core/bases/base_runner.h"
#include "ui/views/bases/bases_render.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QLabel;
class QLineEdit;

namespace oc {
class VaultSession;
class SearchInput;
}

namespace oc::basesui {

class BaseLayout;
class MenuPanel;
class TextIconButton;

// A registered view type (Obsidian's registerView: table, cards, list).
struct ViewTypeInfo {
    QString id;
    QString name;
    QString icon;
};
const QList<ViewTypeInfo>& viewTypes();
const ViewTypeInfo* viewType(const QString& id);

class BasesController : public QWidget {
    Q_OBJECT
public:
    enum class Mode { File, Embed };

    BasesController(VaultSession* session, Mode mode, QWidget* parent = nullptr);
    ~BasesController() override;

    VaultSession* session() const;
    Mode mode() const { return m_mode; }
    bool isEmbedded() const { return m_mode == Mode::Embed; }

    // The model.
    void setBase(const bases::BaseFile& base);
    void setParseError(const QString& message);
    bool hasError() const { return !m_error.isEmpty(); }
    QString errorText() const { return m_error; }
    const bases::BaseFile& base() const { return m_base; }
    void editBase(const std::function<void(bases::BaseFile&)>& fn);
    void editView(const std::function<void(bases::BaseView&)>& fn);

    QString viewName() const { return m_viewName; }
    void selectView(const QString& name);
    const bases::BaseView* currentView() const;
    void setThisFile(const QString& path);
    QString thisFile() const;
    void setReadOnly(bool ro);
    bool isReadOnly() const { return m_readOnly; }

    bases::QueryRunner* runner() const { return m_runner; }
    const bases::ViewData& data() const;
    BaseLayout* layoutWidget() const { return m_layout; }
    bool isReady() const;
    bool isIdle() const; // the data is up to date (nothing scheduled)

    TextIconButton* viewsButton() const { return m_viewsButton; }
    TextIconButton* resultsButton() const { return m_resultsButton; }
    TextIconButton* sortButton() const { return m_sortButton; }
    TextIconButton* filterButton() const { return m_filterButton; }
    TextIconButton* propertiesButton() const { return m_propertiesButton; }
    TextIconButton* searchButton() const { return m_searchButton; }
    TextIconButton* newButton() const { return m_newButton; }
    SearchInput* searchInput() const { return m_searchInput; }
    QWidget* header() const { return m_header; }

    // Header + search row + content (embeds size to it).
    int preferredHeight() const;

    // Services for the layouts.
    void openLink(const QString& target, const QString& sourcePath, Qt::KeyboardModifiers mods);
    void openFile(const QString& path, Qt::KeyboardModifiers mods);
    void hoverLink(const QString& target, const QString& sourcePath, const QRect& globalRect, QWidget* hoverParent);
    void unhoverLink(QWidget* hoverParent);
    void openTag(const QString& tag);
    void openUrl(const QString& url);
    bool isResolved(const QString& linktext, const QString& sourcePath) const;
    QString imagePath(const QString& target, const QString& sourcePath = {}) const;
    // The property widget a value shows in (the property type manager's inferred type).
    QString propertyType(const QString& key, const QVariant& value) const;
    QString propertyIcon(const QString& key) const;
    // Set a note property (Obsidian's processFrontMatter through FrontmatterEditor: only that
    // property's lines change). `key` is matched case-insensitively against the existing keys.
    bool writeProperty(const QString& path, const QString& key, const QVariant& value);
    void showFileMenu(const QString& path, const QPoint& globalPos, QWidget* origin);
    void notice(const QString& message);
    PaintOptions paintOptions(const QFont& font, const QColor& color) const;

    // Toolbar actions.
    void toggleSearch();
    void setSearchText(const QString& text);
    MenuPanel* openViewsMenu();
    MenuPanel* openResultsMenu();
    MenuPanel* openSortMenu();
    MenuPanel* openFilterMenu();
    MenuPanel* openPropertiesMenu();
    MenuPanel* openConfigureView(const QString& name);
    void promptAddView();
    // "New": a note in the base's folder with the properties the filters imply, opened for editing.
    QString createNewItem();
    // The current view's table as TSV (+ Markdown for the clipboard); "Copy table to clipboard".
    void copyTable();
    bases::TableExport exportTable() const;

    // Obsidian's ephemeral state (scroll, active cell) per view name.
    QJsonObject ephemeralState() const;
    void setEphemeralState(const QJsonObject& state);

signals:
    void baseEdited(const oc::bases::BaseFile& base);
    void viewChanged(const QString& name);
    void preferredHeightChanged();
    void dataUpdated();

protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void apply(bool rerunQuery);
    void ensureLayout();
    void updateToolbar();
    void onDataUpdated();
    void relayoutHeader();

    QPointer<VaultSession> m_session;
    Mode m_mode;
    bases::BaseFile m_base;
    QString m_viewName;
    QString m_error;
    bool m_readOnly = false;
    bool m_hasBase = false;
    bases::QueryRunner* m_runner;
    QWidget* m_header;
    TextIconButton* m_viewsButton;
    TextIconButton* m_resultsButton;
    TextIconButton* m_sortButton;
    TextIconButton* m_filterButton;
    TextIconButton* m_propertiesButton;
    TextIconButton* m_searchButton;
    TextIconButton* m_newButton;
    QWidget* m_searchRow;
    SearchInput* m_searchInput;
    QLabel* m_errorLabel;
    BaseLayout* m_layout = nullptr;
    QString m_layoutType;
    QHash<QString, QJsonObject> m_viewStates;
    int m_lastPreferredHeight = -1;
};

} // namespace oc::basesui
