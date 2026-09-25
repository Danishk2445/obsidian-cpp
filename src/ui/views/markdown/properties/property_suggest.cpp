#include "ui/views/markdown/properties/property_suggest.h"

#include "core/config/vault_config.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/fuzzy/fuzzy.h"
#include "core/metadata/metadata_cache.h"
#include "core/properties/property_types.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "text/codec.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/workspace/workspace.h"
#include "ui/views/markdown/editor_suggest.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTextEdit>

namespace oc {

namespace {

constexpr int kLimit = 100;

QList<QPair<int, int>> pairs(const QList<MatchRange>& ranges, int shift = 0)
{
    QList<QPair<int, int>> out;
    for (const MatchRange& r : ranges) {
        const int a = std::max(0, r.from + shift), b = r.to + shift;
        if (b > a)
            out.append({a, b});
    }
    return out;
}

Suggestion renderLink(const LinkSuggestion& s, bool global)
{
    using T = LinkSuggestion::Type;
    Suggestion out;
    out.downranked = s.downranked;
    auto withoutMd = [](const QString& p) { return p.endsWith(QLatin1String(".md")) ? p.chopped(3) : p; };
    if (global && (s.type == T::Heading || s.type == T::Block))
        out.note = withoutMd(s.file);
    switch (s.type) {
    case T::None:
        out.title = QObject::tr("No match found");
        break;
    case T::File: {
        const qsizetype slash = s.path.lastIndexOf(u'/');
        const QString folder = slash < 0 ? QString() : s.path.left(slash + 1);
        out.title = s.path.mid(folder.size());
        out.titleMatches = pairs(s.matches, -int(folder.size()));
        out.note = folder;
        break;
    }
    case T::Alias:
        out.title = s.alias.isEmpty() ? QObject::tr("Display text") : s.alias;
        out.titleMatches = pairs(s.matches);
        out.note = s.path;
        out.auxIcon = QStringLiteral("forward");
        break;
    case T::Linktext:
        out.title = s.path;
        out.titleMatches = pairs(s.matches);
        break;
    case T::Heading:
        out.title = s.heading.isEmpty() ? QObject::tr("Heading") : s.heading;
        out.titleMatches = pairs(s.matches);
        if (s.level > 0)
            out.auxText = QStringLiteral("H") + QString::number(s.level);
        break;
    case T::Block:
        out.title = s.display.section(u'\n', 0, 3);
        out.titleMatches = pairs(s.matches);
        break;
    }
    return out;
}

QString completeTagPrefix(const QString& tag, const QList<MatchRange>& matches)
{
    if (!matches.isEmpty()) {
        const qsizetype slash = tag.indexOf(u'/', matches.last().to);
        if (slash >= 0)
            return tag.left(slash + 1);
    }
    return tag;
}

} // namespace

PropertySuggest::PropertySuggest(QWidget* input, Mode mode, Source source, QObject* parent)
    : QObject(parent ? parent : input)
    , m_input(input)
    , m_mode(mode)
    , m_source(std::move(source))
{
    input->installEventFilter(this);
    if (auto* le = qobject_cast<QLineEdit*>(input)) {
        connect(le, &QLineEdit::textEdited, this, [this] { refresh(); });
        connect(le, &QLineEdit::cursorPositionChanged, this, [this] {
            if (m_open && m_linkMode)
                refresh();
        });
    } else if (auto* te = qobject_cast<QTextEdit*>(input)) {
        connect(te, &QTextEdit::textChanged, this, [this, te] {
            if (!m_settingText && te->hasFocus())
                refresh();
        });
    }
}

PropertySuggest::~PropertySuggest()
{
    if (m_input)
        m_input->setProperty("ocSuggestActive", false);
    delete m_popup;
}

void PropertySuggest::setEnabled(bool on)
{
    m_enabled = on;
    if (!on)
        close();
}

QString PropertySuggest::text() const
{
    if (auto* le = qobject_cast<QLineEdit*>(m_input.data()))
        return le->text();
    if (auto* te = qobject_cast<QTextEdit*>(m_input.data()))
        return te->toPlainText();
    return {};
}

int PropertySuggest::cursor() const
{
    if (auto* le = qobject_cast<QLineEdit*>(m_input.data()))
        return le->cursorPosition();
    if (auto* te = qobject_cast<QTextEdit*>(m_input.data()))
        return te->textCursor().position();
    return 0;
}

void PropertySuggest::setOpen(bool open)
{
    m_open = open;
    if (m_input)
        m_input->setProperty("ocSuggestActive", open);
}

void PropertySuggest::close()
{
    if (m_popup && m_popup->isVisible()) {
        m_popup->hide();
        if (auto* layer = qobject_cast<OverlayLayer*>(m_popup->parentWidget()))
            layer->refresh();
    }
    if (m_links)
        m_links->reset();
    setOpen(false);
}

void PropertySuggest::refresh()
{
    VaultSession* session = m_source.session ? m_source.session() : nullptr;
    if (!m_enabled || !m_input || !session) {
        close();
        return;
    }
    PropertyTypeManager* types = PropertyTypeManager::of(session);
    const QString value = text();
    QList<Suggestion> rows;
    QStringList texts;
    QList<SuggestModal::Instruction> instructions;
    bool selectNone = false;
    m_linkMode = false;
    m_linkItems.clear();
    auto excluded = [this](const QString& s) { return m_source.exclude && m_source.exclude(s); };

    switch (m_mode) {
    case Mode::Keys: {
        const SimpleSearch search(value.toLower());
        struct Row {
            QString name;
            QString icon;
            double score;
            QList<MatchRange> matches;
        };
        QList<Row> found;
        for (const PropertyInfo& p : types->allProperties()) {
            if (excluded(p.name))
                continue;
            const QString icon = props::propertyType(types->typeInfo(p.name).inferred).icon;
            if (search.isEmpty()) {
                found.append({p.name, icon, 0, {}});
            } else if (auto m = search.match(p.name.toLower())) {
                found.append({p.name, icon, m->score, m->matches});
            }
        }
        std::stable_sort(found.begin(), found.end(), [](const Row& a, const Row& b) {
            if (a.score != b.score)
                return a.score > b.score;
            return QString::localeAwareCompare(a.name, b.name) < 0;
        });
        for (const Row& r : std::as_const(found)) {
            Suggestion s;
            s.title = r.name;
            s.titleMatches = pairs(r.matches);
            s.icon = r.icon;
            rows << s;
            texts << r.name;
        }
        break;
    }
    case Mode::Values: {
        if (value.startsWith(QLatin1String("[["))) {
            m_linkMode = true;
            qsizetype end = value.indexOf(QLatin1String("]]"));
            if (end < 0)
                end = value.size();
            const int c = cursor();
            if (c < end)
                end = c;
            m_linkQuery = value.mid(2, std::max<qsizetype>(0, end - 2));
            if (!m_links) {
                m_links = std::make_unique<LinkSuggestProvider>(session->vault(), session->metadata());
                QPointer<VaultSession> s = session;
                m_links->setContentReader([s](const QString& path) -> std::optional<QString> {
                    if (!s)
                        return std::nullopt;
                    if (DocumentManager* dm = s->documents())
                        if (Document* d = dm->find(path))
                            return d->text().toString();
                    if (!s->vault())
                        return std::nullopt;
                    const std::optional<QByteArray> bytes = s->vault()->read(path, nullptr, nullptr);
                    if (!bytes)
                        return std::nullopt;
                    return decodeFile(*bytes).text.toString();
                });
            }
            m_linkItems = m_links->suggestions(m_linkQuery.trimmed(), m_source.sourcePath ? m_source.sourcePath() : QString());
            if (m_linkItems.size() > kLimit)
                m_linkItems.resize(kLimit);
            for (const LinkSuggestion& l : std::as_const(m_linkItems))
                rows << renderLink(l, m_links->isGlobal());
            for (const auto& [cmd, purpose] : m_links->instructions())
                instructions.append({cmd, purpose});
            selectNone = m_linkQuery.isEmpty() || m_linkQuery.startsWith(u'^');
        } else {
            const QString key = m_source.key ? m_source.key() : QString();
            const SimpleSearch search(value.toLower());
            struct Row {
                QString text;
                double score;
                QList<MatchRange> matches;
            };
            QList<Row> found;
            for (const QString& v : types->valuesForKey(key)) {
                if (excluded(v))
                    continue;
                if (value.isEmpty())
                    found.append({v, 0, {}});
                else if (auto m = search.match(v.toLower()))
                    found.append({v, m->score, m->matches});
            }
            std::stable_sort(found.begin(), found.end(), [](const Row& a, const Row& b) { return a.score > b.score; });
            for (const Row& r : std::as_const(found)) {
                Suggestion s;
                s.title = r.text;
                s.titleMatches = pairs(r.matches);
                if (r.text.startsWith(QLatin1String("[[")) && r.text.endsWith(QLatin1String("]]")))
                    s.auxIcon = QStringLiteral("link");
                rows << s;
                texts << r.text;
            }
            selectNone = value.isEmpty() || value.startsWith(u'#');
        }
        instructions.append({tr("Type [["), tr("to link note")});
        instructions.append({QStringLiteral("esc"), tr("to dismiss")});
        break;
    }
    case Mode::Tags: {
        MetadataCache* mc = session->metadata();
        if (!mc)
            break;
        const bool hash = value.startsWith(u'#');
        for (const TagSuggestion& t : tagSuggestions(*mc, hash ? value.mid(1) : value)) {
            if (excluded(t.tag) || excluded(u'#' + t.tag))
                continue;
            Suggestion s;
            s.title = t.tag;
            s.titleMatches = pairs(t.matches);
            s.data = QString((hash ? QStringLiteral("#") : QString()) + completeTagPrefix(t.tag, t.matches)); // Tab completion
            rows << s;
            texts << (hash ? u'#' + t.tag : t.tag);
            if (rows.size() >= kLimit)
                break;
        }
        selectNone = value.isEmpty();
        break;
    }
    }

    if (rows.isEmpty() || !m_input->hasFocus()) {
        close();
        return;
    }
    if (rows.size() > kLimit)
        rows.resize(kLimit);
    m_rows = rows;
    m_texts = texts;
    if (!m_popup) {
        auto* w = qobject_cast<MainWindow*>(m_input->window());
        if (!w || !w->overlay())
            return;
        m_popup = new SuggestPopup(w->overlay());
        connect(m_popup->list(), &SuggestionList::activated, this, [this](int index, Qt::KeyboardModifiers) { choose(index, QString()); });
    }
    m_popup->list()->setSuggestions(rows);
    if (selectNone)
        m_popup->list()->clearSelection();
    m_popup->setInstructions(instructions);
    setOpen(true);
    reposition();
}

void PropertySuggest::reposition()
{
    if (!m_popup || !m_input)
        return;
    QWidget* layer = m_popup->parentWidget();
    const QPoint tl = m_input->mapTo(layer->window(), QPoint(0, 0));
    const QPoint origin = layer->mapTo(layer->window(), QPoint(0, 0));
    m_popup->showAt(QRect(tl - origin, m_input->size()));
}

bool PropertySuggest::choose(int index, const QString& key)
{
    if (index < 0 || index >= m_rows.size())
        return false;
    if (m_linkMode) {
        if (index >= m_linkItems.size())
            return false;
        chooseLink(m_linkItems[index], key);
        return true;
    }
    if (m_mode == Mode::Tags && key == u"Tab") {
        const QString completed = m_rows[index].data.toString();
        close();
        emit accepted(completed, false, int(completed.size()));
        return true;
    }
    const QString chosen = m_texts.value(index);
    close();
    emit accepted(chosen, true, int(chosen.size()));
    return true;
}

void PropertySuggest::chooseLink(const LinkSuggestion& s, const QString& key)
{
    VaultSession* session = m_source.session ? m_source.session() : nullptr;
    if (!session)
        return;
    const QString value = text();
    const bool commit = key.isEmpty() || key == u"Enter";
    close();
    if (s.type == LinkSuggestion::Type::None) {
        QString v = value;
        if (v.startsWith(QLatin1String("[[")) && !v.endsWith(QLatin1String("]]")))
            v += QStringLiteral("]]");
        emit accepted(v, true, int(v.size()));
        return;
    }
    VaultConfig* cfg = session->vaultConfig();
    MetadataCache* mc = session->metadata();
    const LinkResolver::LinkFormat format = cfg ? cfg->newLinkFormat() : LinkResolver::LinkFormat::Shortest;
    auto linktext = [mc, format](const QString& target, const QString& source, bool omitMd) {
        return mc ? mc->resolver().linkText(target, source, format, omitMd) : target;
    };
    const QString source = m_source.sourcePath ? m_source.sourcePath() : QString();
    // Properties always hold wikilinks (Obsidian's "frontmatter" link context).
    const LinkReplacement r = linkSuggestReplacement(s, m_linkQuery, value, 0, int(value.size()), source, key, false, linktext);
    if (s.type == LinkSuggestion::Type::Block && !r.newBlockId.isEmpty() && !s.file.isEmpty()) {
        // the chosen block gets its new id in its note (like Obsidian)
        const BlockIdInsertion ins = blockIdInsertion(s, r.newBlockId);
        if (FileManager* fm = session->fileManager()) {
            const QString content = s.content;
            fm->editFile(s.file, [&](const Text& t) -> QList<ChangeSpec> {
                if (t.toString() != content)
                    return {};
                return {ChangeSpec{ins.at, ins.at, ins.text}};
            });
        }
    }
    const QString next = value.left(r.start) + r.replacement + value.mid(r.end);
    emit accepted(next, commit, commit ? int(next.size()) : r.selectionEnd);
}

bool PropertySuggest::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_input)
        return false;
    switch (ev->type()) {
    case QEvent::FocusIn:
        if (static_cast<QFocusEvent*>(ev)->reason() != Qt::PopupFocusReason)
            refresh();
        return false;
    case QEvent::FocusOut:
        if (static_cast<QFocusEvent*>(ev)->reason() != Qt::PopupFocusReason)
            close();
        return false;
    case QEvent::Resize:
    case QEvent::Move:
        if (m_open)
            reposition();
        return false;
    case QEvent::KeyPress:
    case QEvent::ShortcutOverride:
        break;
    default:
        return false;
    }
    if (!m_open || !m_popup)
        return false;
    auto* ke = static_cast<QKeyEvent*>(ev);
    const bool press = ev->type() == QEvent::KeyPress;
    SuggestionList* list = m_popup->list();
    const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
    auto handled = [&] {
        ke->accept();
        return true;
    };
    switch (ke->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods != Qt::NoModifier)
            return false;
        if (press) {
            const int cur = list->selectedIndex();
            list->setSelectedIndex(ke->key() == Qt::Key_Up ? (cur < 0 ? -1 : cur - 1) : cur + 1);
        }
        return handled();
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        if (press)
            ke->key() == Qt::Key_PageUp ? list->pageUp() : list->pageDown();
        return handled();
    case Qt::Key_Escape:
        if (press)
            close();
        return handled();
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (mods & Qt::ShiftModifier) {
            // Shift+Enter: the typed text as is
            if (press) {
                QString v = text();
                if (m_linkMode && !v.endsWith(QLatin1String("]]")))
                    v += QStringLiteral("]]");
                close();
                emit accepted(v, true, int(v.size()));
            }
            return handled();
        }
        if (list->selectedIndex() < 0) {
            if (press)
                close();
            return false;
        }
        if (press)
            choose(list->selectedIndex(), QStringLiteral("Enter"));
        return handled();
    case Qt::Key_Tab:
        if (mods != Qt::NoModifier || list->selectedIndex() < 0)
            return false;
        if (press)
            choose(list->selectedIndex(), QStringLiteral("Tab"));
        return handled();
    default:
        break;
    }
    if (m_linkMode && m_links && list->selectedIndex() >= 0 && (ke->text() == u"#" || ke->text() == u"^")) {
        const bool hash = ke->text() == u"#";
        const auto mode = m_links->mode();
        if (mode == LinkSuggestProvider::Mode::File || (hash && mode == LinkSuggestProvider::Mode::Heading)) {
            if (press)
                choose(list->selectedIndex(), ke->text());
            return handled();
        }
    }
    return false;
}

} // namespace oc
