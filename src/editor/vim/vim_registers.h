#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace oc::vim {

// One edit made in insert mode, relative to the cursor it was made at: delete `delBefore`
// characters before it and `delAfter` after it, insert `text`, and leave the cursor `cursorBack`
// characters before the end of the inserted text. Replaying these reproduces typing, auto-pairs,
// list continuation and backspacing without going through the input handlers again.
struct InsertOp {
    int delBefore = 0;
    int delAfter = 0;
    QString text;
    int cursorBack = 0;
    bool overwrite = false; // replace mode: delAfter never reaches past the line end
    bool operator==(const InsertOp&) const = default;
};

struct InsertChanges {
    QList<InsertOp> changes;
    bool expectCursorActivityForChange = false;
    bool maybeReset = false; // the cursor moved outside an edit: the next edit starts over
    int visualBlock = 0;     // lines below the first one of a visual-block insert
    QString text() const;    // the inserted text (the "." register)
};

// A vim register (CodeMirror-vim's Register): text chunks plus, for macros, the insert-mode
// changes and search queries recorded with the keys.
class Register {
public:
    Register() = default;
    Register(const QString& text, bool linewise, bool blockwise = false);

    void setText(const QString& text, bool linewise = false, bool blockwise = false);
    void pushText(const QString& text, bool linewise = false); // append ("A-"Z)
    void pushInsertModeChanges(const InsertChanges& changes) { insertModeChanges.push_back(changes); }
    void pushSearchQuery(const QString& query) { searchQueries.push_back(query); }
    void clear();
    QString toString() const { return keyBuffer.join(QString()); }

    QStringList keyBuffer{QString()};
    QList<InsertChanges> insertModeChanges;
    QStringList searchQueries;
    bool linewise = false;
    bool blockwise = false;
};

// System clipboard access for the "+ and "* registers; replaceable (tests).
struct Clipboard {
    std::function<QString()> read;
    std::function<void(const QString&)> write;
    static Clipboard system(); // QGuiApplication::clipboard()
};

// The register file (CodeMirror-vim's RegisterController) as Obsidian configures it: the unnamed
// register is internal (yanks do not touch the system clipboard); "+ writes and reads the
// clipboard; "* is an alias of "+ here (Obsidian has no "*); "0 holds the last unnamed yank; "1-"9
// shift on multi-line deletes and changes, "- takes small deletes; "A-"Z append; "_ discards.
class RegisterController {
public:
    RegisterController();

    // operator: "yank", "delete" or "change". An empty or invalid name uses the default registers.
    void pushText(const QString& registerName, const QString& op, QString text, bool linewise, bool blockwise);
    Register& get(const QString& name); // invalid names give the unnamed register
    Register& unnamed() { return m_registers[QStringLiteral("\"")]; }
    bool isValid(const QString& name) const;
    bool isClipboard(const QString& name) const { return name == u"+" || name == u"*"; }

    // Contents for pasting / inserting: the clipboard registers read the system clipboard (a text
    // vim did not put there counts as characterwise unless it ends with a newline).
    struct Contents {
        QString text;
        bool linewise = false;
        bool blockwise = false;
    };
    Contents contents(const QString& name);

    const QHash<QString, Register>& all() const { return m_registers; }
    void setClipboard(Clipboard clipboard) { m_clipboard = std::move(clipboard); }
    Clipboard& clipboard() { return m_clipboard; }

private:
    void shiftNumericRegisters();
    QHash<QString, Register> m_registers;
    Clipboard m_clipboard;
    QString m_lastClipboardWrite;
};

} // namespace oc::vim
