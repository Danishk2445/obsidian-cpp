#pragma once

#include "core/properties/frontmatter_editor.h"
#include "ui/views/markdown/properties/property_editors.h"

#include <QList>
#include <QPointer>
#include <QWidget>

#include <functional>

class QLineEdit;
class QMenu;

namespace oc {

class PropertiesWidget;
class PropertySuggest;
class VaultSession;

// One property of the metadata editor (Obsidian's .metadata-property): the type icon (click: the
// property menu), the key input (with key suggestions), the value widget of the property's type and a
// type-mismatch warning. The row itself takes the focus for keyboard navigation.
class PropertyRow : public QWidget {
    Q_OBJECT
public:
    PropertyRow(PropertiesWidget* owner, const props::Property& entry);

    const props::Property& entry() const { return m_entry; }
    QString key() const { return m_entry.key; }
    // Obsidian's renderProperty(entry, force, useExpected): the value widget follows the type info; it
    // is left alone while it has the focus or when neither the value nor the type changed.
    void render(const props::Property& entry, bool force = false, bool useExpected = false);
    QString widgetType() const;               // the value widget shown
    QString expectedType() const { return m_expected; }
    QString inferredType() const { return m_inferred; }
    bool hasTypeWarning() const { return m_warning; }
    QLineEdit* keyInput() const { return m_keyInput; }
    PropertySuggest* keySuggest() const { return m_keySuggest; }
    PropertyValueEditor* editor() const { return m_editor; }

    void focusProperty();
    void focusKey();
    void focusValue(const QString& where = QStringLiteral("both"));
    bool hasFocusWithin() const;
    void setSelected(bool on);
    bool isSelected() const { return m_selected; }
    void setReadOnly(bool ro);

    // Obsidian's property menu (Property type submenu, Cut / Copy / Paste, Remove).
    void populateMenu(QMenu* menu);
    void showMenu(const QPoint& globalPos);
    void changeType(const QString& type); // "" = automatic
    bool commitKey(const QString& text);  // handleUpdateKey
    void updateValue(const QVariant& value); // handleUpdateValue
    void showDisplayAsPrompt();           // the type-mismatch dialog ("Display as ...?")

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    QRect iconRect() const;
    QRect warningRect() const;

signals:
    void heightChanged();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void focusInEvent(QFocusEvent* e) override;
    bool focusNextPrevChild(bool next) override;

private:
    void layoutChildren();
    void createEditor(const QString& type);
    PropertyEditorContext editorContext() const;

    PropertiesWidget* m_owner;
    props::Property m_entry;
    QString m_expected, m_inferred, m_shown;
    QLineEdit* m_keyInput;
    PropertySuggest* m_keySuggest = nullptr;
    PropertyValueEditor* m_editor = nullptr;
    bool m_warning = false;
    bool m_selected = false;
    bool m_readOnly = false;
    bool m_hoverIcon = false;
    bool m_hoverWarning = false;
    bool m_rendered = false;
};

// Obsidian's metadata editor (.metadata-container): the "Properties" heading (collapsible, with a
// menu), the properties, the "Add property" button, or — for frontmatter that isn't valid YAML — the
// "Invalid properties" box with "Show source". It edits the note through the host's edit handler (one
// transaction per change); the host calls synchronize() whenever the frontmatter changes.
//
// Keyboard (a property focused): ↑/↓ (k/j, Shift+Tab/Tab) move between properties, ←/→ (h/l) go to the
// key / value, Enter or Space edit the value, Shift+↑/↓ extend the selection, Mod+A selects all,
// Delete removes, "o" adds a property, Home or Alt+↓ go to the text, Mod+Z / Mod+Shift+Z undo / redo,
// Mod+C / Mod+X / Mod+V copy, cut and paste properties.
class PropertiesWidget : public QWidget {
    Q_OBJECT
public:
    enum class Display { Normal, ErrorOnly };
    using Edit = std::function<void(props::FrontmatterEditor&)>;
    using EditHandler = std::function<bool(const Edit&)>;

    explicit PropertiesWidget(QWidget* parent = nullptr);
    ~PropertiesWidget() override;

    // Context for the value widgets (session, source path, link / tag handlers, page preview).
    void setContext(const PropertyEditorContext& ctx);
    const PropertyEditorContext& context() const { return m_ctx; }
    VaultSession* session() const { return m_ctx.session ? m_ctx.session() : nullptr; }
    void setEditHandler(EditHandler handler) { m_edit = std::move(handler); }
    // Obsidian's synchronize(): the note's frontmatter state and properties.
    void synchronize(props::FrontmatterEditor::State state, const QList<props::Property>& properties);
    void setDisplay(Display display);
    Display display() const { return m_display; }
    void setReadOnly(bool ro);
    bool isReadOnly() const { return m_readOnly; }
    // Content starts `px` from the left (room for the collapse indicator, like Obsidian's gutter).
    void setLeftInset(int px);
    int leftInset() const { return m_inset; }

    bool isInvalid() const { return m_state == props::FrontmatterEditor::State::Invalid; }
    bool shouldShow() const; // properties (or one being added) in Normal display, or an error
    int propertyCount() const { return int(m_rows.size()); }
    QStringList keys() const;
    PropertyRow* row(const QString& key) const;
    PropertyRow* rowAt(int index) const { return m_rows.value(index); }
    int indexOf(const PropertyRow* row) const { return int(m_rows.indexOf(const_cast<PropertyRow*>(row))); }
    PropertyRow* focusedRow() const;
    QWidget* headingWidget() const { return m_heading; }
    QWidget* addButton() const { return m_addButton; }
    QWidget* errorBox() const { return m_errorBox; }

    // Obsidian's metadataEditor API.
    void addProperty(const QString& key = {});
    bool focus(bool bottom);              // heading (or the last property / add button with bottom)
    void focusPropertyAt(int index);      // -1: the last one; past the end: the add button
    void focusProperty(const QString& key);
    void focusKey(const QString& key);
    void focusValue(const QString& key, const QString& where = QStringLiteral("both"));
    void removeProperties(QList<PropertyRow*> rows, bool refocus = true);
    void sortProperties(bool reverse);
    void clearProperties(); // "Clear file properties"
    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);
    bool hasFocusWithin() const;
    void selectAll();
    void clearSelection();
    QList<PropertyRow*> selectedRows() const;
    void copyProperties(const QList<PropertyRow*>& rows, bool cut);
    bool pasteProperties();
    void populateHeadingMenu(QMenu* menu);

    // Edits through the host (false: the note can't be edited).
    bool edit(const Edit& fn);
    void notify(const QString& message);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

    // Row keyboard handling (shared by the rows, the heading and the add button).
    bool handleKey(QKeyEvent* e, PropertyRow* from);
    void focusAfterEditor() { emit focusAfterRequested(); }

signals:
    void heightChanged();
    void focusAfterRequested();  // leave to the text (first line)
    void focusBeforeRequested(); // leave to the inline title
    void undoRequested();
    void redoRequested();
    void showSourceRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    friend class PropertyRow;
    int layoutContent(int width, bool apply) const;
    void relayout();
    void rowTypeChanged(const QString& lowerKey);
    PropertyRow* pendingRow() const;

    PropertyEditorContext m_ctx;
    EditHandler m_edit;
    props::FrontmatterEditor::State m_state = props::FrontmatterEditor::State::None;
    Display m_display = Display::Normal;
    QList<PropertyRow*> m_rows;
    QWidget* m_heading;
    QWidget* m_addButton;
    QWidget* m_errorBox;
    int m_inset = 0;
    int m_lastHeight = -1;
    bool m_readOnly = false;
    bool m_collapsed = false;
    QMetaObject::Connection m_typesConn;
    QPointer<QObject> m_typesOwner;
};

} // namespace oc
