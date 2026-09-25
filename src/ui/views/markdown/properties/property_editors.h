#pragma once

// The value widgets of Obsidian's property editor (the metadataTypeManager's registered widgets):
// text (a wrapping text field; values that are links show as links with an edit flair), list /
// tags / aliases (pills + an input), number, checkbox, date, date & time, and the read-only
// "unknown" widget (JSON text) for nested maps and mixed lists.

#include <QDateTimeEdit>
#include <QLineEdit>
#include <QPointer>
#include <QTextEdit>
#include <QVariant>
#include <QWidget>

#include <functional>
#include <optional>

namespace oc {

class PropertySuggest;
class VaultSession;

// What a value widget needs from its property editor (Obsidian's render context).
struct PropertyEditorContext {
    std::function<VaultSession*()> session;
    std::function<QString()> sourcePath;
    QString key;
    // Links written in values: kind 0 = a vault link (linktext), 3 = an external URL.
    std::function<void(const QString& target, int kind, bool newLeaf)> openLink;
    std::function<void(const QString& tag)> openTag;
    // The mouse is over a vault link (page preview); empty linktext: it left.
    std::function<void(const QString& linktext, const QRect& globalRect, QWidget* hoverParent)> hoverLink;
    std::function<bool(const QString& linkpath)> isResolved;
    std::function<void(const QString& message)> notice;
};

// A link written as a property value (Obsidian's cN): "[[target|alias]]", "[text](url)", a bare URL
// or an e-mail address.
struct PropertyLink {
    bool internal = false;
    QString href;  // linktext for vault links, URL otherwise
    QString title; // what is shown
};
std::optional<PropertyLink> parsePropertyLink(const QString& value);

// Obsidian's tag rule for tag pills ("#" + tag characters, not only digits).
bool isValidTagValue(const QString& tag);

class PropertyValueEditor : public QWidget {
    Q_OBJECT
public:
    PropertyValueEditor(const PropertyEditorContext& ctx, QWidget* parent);

    virtual QString type() const = 0;
    virtual void setValue(const QVariant& value) = 0;
    // Focus the editor; where: "start", "end" or "both" (select all).
    virtual void focusEditor(const QString& where = QStringLiteral("both")) = 0;
    virtual void setReadOnly(bool ro) { m_readOnly = ro; }
    bool isReadOnly() const { return m_readOnly; }
    bool hasFocusWithin() const;

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

signals:
    void changed(const QVariant& value); // Obsidian's onChange: a new value to save
    void blurRequested();                // Obsidian's ctx.blur(): the property row gets the focus
    void heightChanged();

protected:
    PropertyEditorContext m_ctx;
    bool m_readOnly = false;
};

PropertyValueEditor* createPropertyEditor(const QString& type, const QVariant& value, const PropertyEditorContext& ctx,
                                          QWidget* parent);

// ---- text ------------------------------------------------------------------------------------------

// Obsidian's .metadata-input-longtext: wraps, shows at most three lines unless focused, placeholder
// "Empty"; Enter commits, Shift+Enter breaks the line, Escape reverts.
class LongTextEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit LongTextEdit(QWidget* parent = nullptr);
    void setPlaceholder(const QString& text) { m_placeholder = text; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void commitRequested();
    void revertRequested();
    void focusLost();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void insertFromMimeData(const QMimeData* source) override;

private:
    QString m_placeholder;
};

class TextPropertyEditor : public PropertyValueEditor {
    Q_OBJECT
public:
    TextPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent);
    QString type() const override { return QStringLiteral("text"); }
    void setValue(const QVariant& value) override;
    void focusEditor(const QString& where) override;
    void setReadOnly(bool ro) override;
    int heightForWidth(int w) const override;
    LongTextEdit* input() const { return m_input; }
    bool showsLink() const { return m_linkShown; }
    QString text() const { return m_value; }
    PropertySuggest* suggest() const { return m_suggest; }
    void commitText(const QString& text); // what typing + Enter does

protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    void render();
    QRect linkRect() const;
    QString m_value;
    LongTextEdit* m_input;
    PropertySuggest* m_suggest = nullptr;
    std::optional<PropertyLink> m_link;
    bool m_linkShown = false;
    bool m_hover = false;
    bool m_hoverLink = false;
    bool m_saving = false;
};

// ---- number ----------------------------------------------------------------------------------------

class NumberPropertyEditor : public PropertyValueEditor {
    Q_OBJECT
public:
    NumberPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent);
    QString type() const override { return QStringLiteral("number"); }
    void setValue(const QVariant& value) override;
    void focusEditor(const QString& where) override;
    void setReadOnly(bool ro) override;
    QLineEdit* input() const { return m_input; }
    bool commit(); // false (and an "Invalid number" notice) when the text is not a number

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    QLineEdit* m_input;
    QVariant m_value;
};

// ---- checkbox --------------------------------------------------------------------------------------

class CheckboxPropertyEditor : public PropertyValueEditor {
    Q_OBJECT
public:
    CheckboxPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent);
    QString type() const override { return QStringLiteral("checkbox"); }
    void setValue(const QVariant& value) override;
    void focusEditor(const QString& where) override;
    bool isChecked() const { return m_checked; }
    bool isIndeterminate() const { return m_indeterminate; }
    void toggle();
    QRect boxRect() const;

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void focusInEvent(QFocusEvent* e) override { QWidget::focusInEvent(e); update(); }
    void focusOutEvent(QFocusEvent* e) override { QWidget::focusOutEvent(e); update(); }

private:
    bool m_checked = false;
    bool m_indeterminate = false;
    bool m_hover = false;
};

// ---- date / date & time ----------------------------------------------------------------------------

// A QDateTimeEdit that can be empty (painting the format as placeholder, like Chromium's date input).
class DateInput : public QDateTimeEdit {
    Q_OBJECT
public:
    DateInput(bool withTime, QWidget* parent = nullptr);
    bool isEmpty() const { return m_empty; }
    void setEmpty(bool empty);
    bool isDirty() const { return m_dirty; }
    void setDirty(bool d) { m_dirty = d; }

signals:
    void commitRequested();
    void revertRequested();
    void focusLost();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    bool m_withTime;
    bool m_empty = true;
    bool m_dirty = false;
};

class DatePropertyEditor : public PropertyValueEditor {
    Q_OBJECT
public:
    DatePropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, bool withTime, QWidget* parent);
    QString type() const override { return m_withTime ? QStringLiteral("datetime") : QStringLiteral("date"); }
    void setValue(const QVariant& value) override;
    void focusEditor(const QString& where) override;
    void setReadOnly(bool ro) override;
    DateInput* input() const { return m_input; }
    QString formatted() const; // "YYYY-MM-DD" / "YYYY-MM-DDTHH:mm:ss", "" when empty
    void commit();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    bool m_withTime;
    DateInput* m_input;
    QString m_value;
};

// ---- lists: multitext, tags, aliases ---------------------------------------------------------------

class MultiSelectPropertyEditor;

// One value of a list (.multi-select-pill): text (or a link / tag) and a remove button.
class PropertyPill : public QWidget {
    Q_OBJECT
public:
    PropertyPill(MultiSelectPropertyEditor* owner, int index, const QString& value);
    QString value() const { return m_value; }
    int index() const { return m_index; }
    bool isInvalid() const { return m_invalid; }
    QRect removeRect() const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void focusInEvent(QFocusEvent* e) override { QWidget::focusInEvent(e); update(); }
    void focusOutEvent(QFocusEvent* e) override { QWidget::focusOutEvent(e); update(); }

private:
    friend class MultiSelectPropertyEditor;
    QString displayText() const;
    MultiSelectPropertyEditor* m_owner;
    int m_index;
    QString m_value;
    std::optional<PropertyLink> m_link;
    bool m_invalid = false;
    bool m_hover = false;
    bool m_hoverRemove = false;
};

class MultiSelectPropertyEditor : public PropertyValueEditor {
    Q_OBJECT
public:
    enum class Kind { List, Tags, Aliases };
    MultiSelectPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, Kind kind, QWidget* parent);
    QString type() const override;
    Kind kind() const { return m_kind; }
    void setValue(const QVariant& value) override;
    void focusEditor(const QString& where) override;
    void setReadOnly(bool ro) override;
    int heightForWidth(int w) const override;

    QStringList values() const { return m_values; }
    QLineEdit* input() const { return m_input; }
    PropertyPill* pill(int index) const { return m_pills.value(index); }
    int pillCount() const { return int(m_pills.size()); }
    PropertySuggest* suggest() const { return m_suggest; }
    // Obsidian's multi-select operations
    bool addValue(const QString& text);   // false when rejected (duplicate, invalid tag)
    void removeValue(int index, bool focusPrevious = false);
    void editValue(int index);
    void focusPill(int index);
    void activatePill(int index, bool newLeaf); // click on a pill (links, tags)
    void showPillMenu(int index, const QPoint& globalPos);

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    friend class PropertyPill;
    void rebuild();
    void relayout();
    int layoutPills(int width, bool apply) const;
    std::optional<QString> createOption(const QString& text); // Obsidian's allowCreatingOptions
    int findDuplicate(const QString& value, const QStringList& in) const;
    void triggerChange();
    void commitInput();

    Kind m_kind;
    QStringList m_values;
    QList<PropertyPill*> m_pills;
    QLineEdit* m_input;
    QLineEdit* m_editInput = nullptr; // editing a pill in place
    int m_editIndex = -1;
    PropertySuggest* m_suggest = nullptr;
};

// ---- unknown ---------------------------------------------------------------------------------------

class UnknownPropertyEditor : public PropertyValueEditor {
    Q_OBJECT
public:
    UnknownPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent);
    QString type() const override { return QStringLiteral("unknown"); }
    void setValue(const QVariant& value) override;
    void focusEditor(const QString& where) override;
    QString text() const { return m_text; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString m_text;
};

} // namespace oc
