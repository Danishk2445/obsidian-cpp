#include "editor/vim/vim_registers.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QRegularExpression>

namespace oc::vim {

QString InsertChanges::text() const
{
    QString out;
    for (const InsertOp& op : changes)
        out += op.text;
    return out;
}

Register::Register(const QString& text, bool lw, bool bw)
    : keyBuffer{text}
    , linewise(lw)
    , blockwise(bw)
{
}

void Register::setText(const QString& text, bool lw, bool bw)
{
    keyBuffer = QStringList{text};
    linewise = lw;
    blockwise = bw;
}

void Register::pushText(const QString& text, bool lw)
{
    // once linewise, always linewise
    if (lw) {
        if (!linewise)
            keyBuffer.push_back(QStringLiteral("\n"));
        linewise = true;
    }
    keyBuffer.push_back(text);
}

void Register::clear()
{
    keyBuffer.clear();
    insertModeChanges.clear();
    searchQueries.clear();
    linewise = false;
}

Clipboard Clipboard::system()
{
    Clipboard c;
    c.read = [] {
        QClipboard* cb = QGuiApplication::clipboard();
        return cb ? cb->text() : QString();
    };
    c.write = [](const QString& text) {
        if (QClipboard* cb = QGuiApplication::clipboard())
            cb->setText(text);
    };
    return c;
}

RegisterController::RegisterController()
    : m_clipboard(Clipboard::system())
{
    for (const char* n : {"\"", ".", ":", "/", "+"})
        m_registers.insert(QString::fromLatin1(n), Register());
}

bool RegisterController::isValid(const QString& name) const
{
    static const QRegularExpression word(QStringLiteral("^\\w$"), QRegularExpression::NoPatternOption);
    if (name.size() != 1)
        return false;
    static const QString valid = QStringLiteral("-\".:_/+*");
    return valid.contains(name) || (name.at(0).unicode() < 0x80 && word.match(name).hasMatch());
}

Register& RegisterController::get(const QString& name)
{
    if (!isValid(name))
        return unnamed();
    QString key = name.toLower();
    if (key == u"*")
        key = QStringLiteral("+");
    return m_registers[key];
}

void RegisterController::shiftNumericRegisters()
{
    for (int i = 9; i >= 2; --i) {
        const Register prev = get(QString::number(i - 1)); // copy: operator[] may rehash
        m_registers[QString::number(i)] = prev;
    }
}

void RegisterController::pushText(const QString& registerName, const QString& op, QString text, bool linewise, bool blockwise)
{
    if (registerName == u"_") // the black hole register
        return;
    if (linewise && !text.endsWith(u'\n'))
        text += u'\n';
    if (!isValid(registerName)) {
        if (op == u"yank") {
            m_registers[QStringLiteral("0")] = Register(text, linewise, blockwise);
        } else if (op == u"delete" || op == u"change") {
            if (!text.contains(u'\n')) {
                m_registers[QStringLiteral("-")] = Register(text, linewise);
            } else {
                shiftNumericRegisters();
                m_registers[QStringLiteral("1")] = Register(text, linewise);
            }
        }
        unnamed().setText(text, linewise, blockwise);
        return;
    }
    Register& reg = get(registerName);
    const bool append = registerName.at(0).isUpper();
    if (append)
        reg.pushText(text, linewise);
    else
        reg.setText(text, linewise, blockwise);
    if (isClipboard(registerName) && m_clipboard.write) {
        m_lastClipboardWrite = reg.toString();
        m_clipboard.write(m_lastClipboardWrite);
    }
    // the unnamed register always mirrors the last used register
    unnamed().setText(reg.toString(), linewise);
}

RegisterController::Contents RegisterController::contents(const QString& name)
{
    Register& reg = get(name);
    Contents c{reg.toString(), reg.linewise, reg.blockwise};
    if (isClipboard(name) && m_clipboard.read) {
        c.text = m_clipboard.read();
        if (c.text != m_lastClipboardWrite) { // copied elsewhere
            c.linewise = c.text.endsWith(u'\n');
            c.blockwise = false;
        }
    }
    return c;
}

} // namespace oc::vim
