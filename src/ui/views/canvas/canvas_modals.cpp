#include "ui/views/canvas/canvas_modals.h"

#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/files/file_manager.h"
#include "core/fuzzy/fuzzy.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/navigation/navigation_util.h"
#include "ui/navigation/quick_switcher.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QVBoxLayout>

namespace oc::canvasui {

using namespace Qt::StringLiterals;

namespace {
bool isImage(const QString& path)
{
    static const QStringList exts = {u"bmp"_s, u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"svg"_s, u"webp"_s, u"avif"_s};
    return exts.contains(path.section(u'.', -1).toLower());
}

bool passes(FileFilter filter, const QString& path)
{
    const QString ext = path.section(u'/', -1).contains(u'.') ? path.section(u'.', -1).toLower() : QString();
    switch (filter) {
    case FileFilter::Notes: return ext == u"md" || ext == u"canvas" || ext == u"base";
    case FileFilter::Media: return ext != u"md";
    case FileFilter::Images: return isImage(path);
    case FileFilter::Any: return true;
    }
    return true;
}

QString tr(const char* s) { return QCoreApplication::translate("oc::CanvasWidget", s); }
} // namespace

SuggestModal* openFilePicker(VaultSession* session, OverlayLayer* layer, FileFilter filter, const QString& sourcePath,
                             std::function<void(const QString& path)> chosen)
{
    if (!layer || !session)
        return nullptr;
    auto source = std::make_shared<SwitcherSource>(SwitcherSource::fromSession(session));
    source->unresolved.clear();
    QuickSwitcherOptions options;
    options.showExistingOnly = true;
    options.showAttachments = filter != FileFilter::Notes;
    options.showAllFileTypes = session->vaultConfig() && session->vaultConfig()->flag(u"showUnsupportedFiles"_s);
    auto items = std::make_shared<QList<SwitcherItem>>();
    auto* modal = new SuggestModal(layer);
    modal->setObjectName(u"ocCanvasFilePicker"_s);
    containKeyEvents(modal);
    modal->setPlaceholder(tr("Type to search..."));
    QList<SuggestModal::Instruction> instructions{{u"↑↓"_s, tr("to navigate")}, {u"↵"_s, tr("to open")}};
    if (filter == FileFilter::Notes)
        instructions << SuggestModal::Instruction{u"shift ↵"_s, tr("to create")};
    instructions << SuggestModal::Instruction{u"esc"_s, tr("to dismiss")};
    modal->setInstructions(instructions);
    modal->setEmptyText(filter == FileFilter::Images ? tr("No images found.") : filter == FileFilter::Media ? tr("No media found.")
                                                                                                            : tr("No notes found."));
    modal->setLimit(20);
    modal->setProvider([source, options, items, filter](const QString& q) {
        items->clear();
        QList<Suggestion> out;
        for (const SwitcherItem& it : quickSwitcherItems(q, *source, options)) {
            if (it.type != SwitcherItem::Type::File && it.type != SwitcherItem::Type::Alias)
                continue;
            if (!passes(filter, it.path))
                continue;
            Suggestion s;
            s.title = it.text;
            for (const MatchRange& m : it.matches)
                s.titleMatches.append({m.from, m.to});
            if (it.type == SwitcherItem::Type::Alias) {
                s.note = displayPath(it.path);
                s.auxIcon = u"forward"_s;
            }
            s.data = int(items->size());
            items->append(it);
            out << s;
        }
        return out;
    });
    QPointer<SuggestModal> m = modal;
    QPointer<VaultSession> sess = session;
    QObject::connect(modal, &SuggestModal::chosen, modal, [items, chosen](const Suggestion& s, Qt::KeyboardModifiers) {
        const int i = s.data.toInt();
        if (i >= 0 && i < items->size() && chosen)
            chosen(items->at(i).path);
    });
    if (filter == FileFilter::Notes) {
        modal->setKeyHandler([m, sess, sourcePath, chosen](QKeyEvent* e) {
            if (!m || !sess || !((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ShiftModifier)))
                return false;
            const QString name = m->query().trimmed();
            m->close();
            if (name.isEmpty() || !sess->fileManager())
                return true;
            QString err;
            TFile* f = sess->fileManager()->createNewMarkdownFile(sess->fileManager()->newFileParent(sourcePath), name, {}, &err);
            if (f && chosen)
                chosen(f->path());
            return true;
        });
    }
    modal->open();
    return modal;
}

Modal* promptText(OverlayLayer* layer, const QString& title, const QString& initial, const QString& placeholder,
                  std::function<QString(const QString&)> validate, std::function<void(const QString&)> done)
{
    if (!layer)
        return nullptr;
    auto* modal = new Modal(layer);
    modal->setObjectName(u"ocCanvasPrompt"_s);
    modal->setTitle(title);
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    auto* input = new QLineEdit(initial);
    input->setObjectName(u"ocCanvasPromptInput"_s);
    input->setPlaceholderText(placeholder);
    auto* error = new QLabel;
    error->setObjectName(u"ocCanvasPromptError"_s);
    error->hide();
    v->addWidget(input);
    v->addWidget(error);
    modal->addContentWidget(box);
    const int save = modal->addButton(tr("Save"), Modal::ButtonStyle::Cta, false);
    modal->setDefaultButton(save);
    QPointer<Modal> m = modal;
    auto submit = [m, input, error, validate, done] {
        if (!m)
            return;
        const QString text = input->text();
        const QString problem = validate ? validate(text) : QString();
        if (!problem.isEmpty()) {
            error->setText(problem);
            error->show();
            return;
        }
        m->close(0);
        if (done)
            done(text);
    };
    QObject::connect(modal, &Modal::buttonClicked, modal, [submit, save](int i) {
        if (i == save)
            submit();
    });
    QObject::connect(input, &QLineEdit::returnPressed, modal, submit);
    modal->open();
    input->setFocus();
    input->selectAll();
    return modal;
}

SuggestModal* openHeadingPicker(VaultSession* session, OverlayLayer* layer, const QString& notePath,
                                std::function<void(const QString& subpath)> chosen)
{
    if (!layer || !session || !session->metadata())
        return nullptr;
    QStringList headings;
    QList<int> levels;
    if (const md::CachedMetadata* meta = session->metadata()->fileCache(notePath))
        for (const md::HeadingCache& h : meta->headings) {
            headings << h.heading;
            levels << h.level;
        }
    auto shown = std::make_shared<QStringList>();
    auto* modal = new SuggestModal(layer);
    modal->setObjectName(u"ocCanvasHeadingPicker"_s);
    containKeyEvents(modal);
    modal->setPlaceholder(tr("Type to find file, heading, or block"));
    modal->setInstructions({{u"↑↓"_s, tr("to navigate")}, {u"↵"_s, tr("to embed heading")}, {u"esc"_s, tr("to dismiss")}});
    modal->setEmptyText(tr("No section found"));
    modal->setProvider([headings, levels, shown](const QString& q) {
        shown->clear();
        QList<Suggestion> out;
        QString query = q.trimmed();
        if (query.startsWith(u'#'))
            query = query.mid(1);
        if (query.isEmpty()) {
            Suggestion s;
            s.title = tr("Show entire file");
            s.data = -1;
            out << s;
        }
        const FuzzyQuery fq(query);
        QList<std::pair<double, int>> hits;
        for (int i = 0; i < headings.size(); ++i) {
            if (fq.isEmpty()) {
                hits << std::make_pair(0.0, i);
                continue;
            }
            if (const auto r = fq.match(headings[i]))
                hits << std::make_pair(r->score, i);
        }
        if (!fq.isEmpty())
            std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [score, i] : hits) {
            Q_UNUSED(score);
            Suggestion s;
            s.title = headings[i];
            s.auxText = u"H"_s + QString::number(levels[i]);
            s.data = i;
            out << s;
        }
        return out;
    });
    QObject::connect(modal, &SuggestModal::chosen, modal, [headings, chosen](const Suggestion& s, Qt::KeyboardModifiers) {
        const int i = s.data.toInt();
        if (chosen)
            chosen(i < 0 ? QString() : u"#"_s + headings.value(i));
    });
    modal->open();
    return modal;
}

SuggestModal* openGroupPicker(OverlayLayer* layer, const canvas::CanvasData& data, std::function<void(const QString& nodeId)> chosen)
{
    if (!layer)
        return nullptr;
    QList<std::pair<QString, QString>> groups; // id, label
    for (const canvas::Node& n : data.nodes)
        if (n.isShown() && n.isGroup())
            groups << std::make_pair(n.id, n.label.isEmpty() ? tr("Untitled group") : n.label);
    auto* modal = new SuggestModal(layer);
    modal->setObjectName(u"ocCanvasGroupPicker"_s);
    containKeyEvents(modal);
    modal->setPlaceholder(tr("Jump to group"));
    modal->setInstructions({{u"↑↓"_s, tr("to navigate")}, {u"↵"_s, tr("to navigate to group")}, {u"esc"_s, tr("to dismiss")}});
    modal->setEmptyText(tr("No groups found"));
    modal->setProvider([groups](const QString& q) {
        QList<Suggestion> out;
        const FuzzyQuery fq(q.trimmed());
        QList<std::pair<double, int>> hits;
        for (int i = 0; i < groups.size(); ++i) {
            if (fq.isEmpty()) {
                hits << std::make_pair(0.0, i);
            } else if (const auto r = fq.match(groups[i].second)) {
                hits << std::make_pair(r->score, i);
            }
        }
        if (!fq.isEmpty())
            std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [score, i] : hits) {
            Q_UNUSED(score);
            Suggestion s;
            s.title = groups[i].second;
            if (!fq.isEmpty())
                if (const auto r = fq.match(groups[i].second))
                    for (const MatchRange& m : matchIndicesToRanges(r->matches))
                        s.titleMatches.append({m.from, m.to});
            s.data = groups[i].first;
            out << s;
        }
        return out;
    });
    QObject::connect(modal, &SuggestModal::chosen, modal, [chosen](const Suggestion& s, Qt::KeyboardModifiers) {
        if (chosen)
            chosen(s.data.toString());
    });
    modal->open();
    return modal;
}

} // namespace oc::canvasui
