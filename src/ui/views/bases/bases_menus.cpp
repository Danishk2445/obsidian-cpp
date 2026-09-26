#include "ui/views/bases/bases_menus.h"

#include "core/bases/base_file.h"
#include "core/fuzzy/fuzzy.h"
#include "core/properties/property_types.h"
#include "core/util/owned_menu.h"
#include "render/theme/icons.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_style.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/widgets/value_slider.h"

#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace oc::basesui {

using namespace oc::bases;

namespace {

QString tr(const char* s) { return QCoreApplication::translate("oc::basesui::Menus", s); }

QFrame* divider(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(basesStyle().border.name(QColor::HexArgb)));
    return line;
}

QWidget* page(QVBoxLayout** layout, int margin = 4)
{
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(margin, margin, margin, margin);
    l->setSpacing(0);
    *layout = l;
    return w;
}

// .input-row: a label and its control.
QWidget* inputRow(const QString& label, QWidget* control, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(8, 2, 8, 2);
    l->setSpacing(8);
    if (!label.isEmpty()) {
        auto* lab = new QLabel(label, row);
        lab->setFont(basesStyle().uiSmall);
        QPalette pal = lab->palette();
        pal.setColor(QPalette::WindowText, basesStyle().text);
        lab->setPalette(pal);
        lab->setMinimumWidth(110);
        l->addWidget(lab);
    }
    control->setParent(row);
    l->addWidget(control, 1);
    return row;
}

QLineEdit* textInput(const QString& value, const QString& placeholder)
{
    auto* e = new QLineEdit;
    e->setText(value);
    e->setPlaceholderText(placeholder);
    e->setFont(basesStyle().uiSmall);
    e->setMinimumHeight(28);
    return e;
}

// The property ids a picker offers: the base's (file, used note properties, formulas) and every
// note property of the vault.
QList<QPair<QString, QString>> propertyOptions(BasesController* c, const std::function<bool(const QString&)>& filter = {})
{
    QStringList ids = c->data().allProperties;
    if (ids.isEmpty())
        ids = fileProperties();
    for (const QString& f : c->base().formulaNames()) {
        const QString id = makePropertyId(QStringLiteral("formula"), f);
        if (!ids.contains(id))
            ids << id;
    }
    if (VaultSession* s = c->session())
        if (PropertyTypeManager* t = PropertyTypeManager::of(s))
            for (const PropertyInfo& info : t->allProperties()) {
                const QString id = makePropertyId(QStringLiteral("note"), info.name);
                bool known = false;
                for (const QString& e : std::as_const(ids))
                    known |= e.compare(id, Qt::CaseInsensitive) == 0;
                if (!known)
                    ids << id;
            }
    QList<QPair<QString, QString>> out;
    for (const QString& id : std::as_const(ids))
        if (!filter || filter(id))
            out.append({id, c->base().displayName(id)});
    return out;
}

// The type of a property's values for the filter / sort pickers (Obsidian's mock context).
ValueType lhsType(BasesController* c, const QString& id)
{
    const PropertyRef r = parsePropertyId(id);
    if (r.type == u"file") {
        if (r.name == u"file")
            return ValueType::File;
        if (r.name == u"ctime" || r.name == u"mtime")
            return ValueType::Date;
        if (r.name == u"size")
            return ValueType::Number;
        if (r.name == u"tags")
            return ValueType::TagList;
        if (r.name == u"links" || r.name == u"embeds" || r.name == u"backlinks")
            return ValueType::List;
        if (r.name == u"properties")
            return ValueType::Object;
        return ValueType::String;
    }
    if (r.type == u"formula") {
        for (const EntryPtr& e : c->data().rows) {
            const Value v = e->valueOrError(id);
            if (!v.isNull() && !v.isError())
                return v.type() == ValueType::RelativeDate ? ValueType::Date : v.type();
        }
        return ValueType::String;
    }
    const QString t = c->propertyType(r.name, QVariant());
    if (t == u"number")
        return ValueType::Number;
    if (t == u"checkbox")
        return ValueType::Boolean;
    if (t == u"date" || t == u"datetime")
        return ValueType::Date;
    if (t == u"tags")
        return ValueType::TagList;
    if (t == u"multitext" || t == u"aliases")
        return ValueType::List;
    if (t == u"text")
        return ValueType::String;
    return ValueType::Null;
}

QString valueIcon(const Value& v)
{
    switch (v.type()) {
    case ValueType::Number: return QStringLiteral("binary");
    case ValueType::Date:
    case ValueType::RelativeDate: return v.hasTime() ? QStringLiteral("clock") : QStringLiteral("calendar");
    case ValueType::Duration: return QStringLiteral("calendar-range");
    case ValueType::Boolean: return QStringLiteral("check-square");
    case ValueType::List:
    case ValueType::TagList: return QStringLiteral("list");
    case ValueType::Error: return QStringLiteral("alert-triangle");
    case ValueType::String: return QStringLiteral("text");
    case ValueType::Link: return QStringLiteral("link");
    default: return QStringLiteral("square-function");
    }
}

QString numberText(double n)
{
    if (!std::isfinite(n))
        return Value::jsNumber(n);
    QString s = QLocale(QLocale::English).toString(n, 'f', 3);
    if (s.contains(u'.')) {
        while (s.endsWith(u'0'))
            s.chop(1);
        if (s.endsWith(u'.'))
            s.chop(1);
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------------------
MenuPanel* openViewsMenu(BasesController* c, QWidget* anchor)
{
    auto* panel = new MenuPanel(anchor, 260);
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 4);
    auto* search = new SearchInput(p);
    search->setPlaceholderText(tr("Search..."));
    l->addWidget(search);
    l->addSpacing(4);
    auto* list = new QWidget(p);
    auto* ll = new QVBoxLayout(list);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    l->addWidget(list);
    l->addWidget(divider(p));
    auto* add = new MenuRow(QStringLiteral("plus"), tr("Add view"), p);
    add->setObjectName(QStringLiteral("ocBasesAddView"));
    l->addWidget(add);
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    auto render = [cc, list, ll, search, pp] {
        if (!cc)
            return;
        while (QLayoutItem* it = ll->takeAt(0)) {
            if (QWidget* w = it->widget())
                w->deleteLater();
            delete it;
        }
        const FuzzyQuery q(search->text());
        for (const BaseView& v : cc->base().views) {
            if (!search->text().isEmpty() && !q.match(v.name))
                continue;
            const ViewTypeInfo* info = viewType(v.type);
            auto* row = new MenuRow(info ? info->icon : QStringLiteral("table"), v.name, list);
            row->setActive(v.name == cc->viewName());
            row->setChevron(true);
            const QString name = v.name;
            QObject::connect(row, &MenuRow::clicked, row, [cc, pp, name] {
                if (cc)
                    cc->selectView(name);
                if (pp)
                    pp->close();
            });
            QObject::connect(row, &MenuRow::chevronClicked, row, [cc, pp, name] {
                if (cc && pp)
                    openConfigureViewMenu(cc, pp->anchor(), name), pp->close();
            });
            ll->addWidget(row);
        }
        if (pp)
            pp->reposition();
    };
    render();
    QObject::connect(search, &QLineEdit::textChanged, panel, render);
    QObject::connect(add, &MenuRow::clicked, panel, [cc, pp, search] {
        if (!cc)
            return;
        const QString base = search->text().trimmed().isEmpty() ? tr("View") : search->text().trimmed();
        const QString name = cc->base().uniqueViewName(base);
        cc->editBase([&](BaseFile& b) {
            BaseView v;
            v.type = QStringLiteral("table");
            v.name = name;
            b.views << v;
        });
        cc->selectView(name);
        QWidget* anchorWidget = pp ? pp->anchor() : nullptr;
        if (pp)
            pp->close();
        if (anchorWidget)
            openConfigureViewMenu(cc, anchorWidget, name);
    });
    panel->push(p);
    panel->popup();
    search->setFocus();
    return panel;
}

MenuPanel* openConfigureViewMenu(BasesController* c, QWidget* anchor, const QString& viewName)
{
    auto* panel = new MenuPanel(anchor, 300);
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    auto name = std::make_shared<QString>(viewName);
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 0);
    auto* header = new PageHeader(tr("Configure view"), true, true, true, p);
    l->addWidget(header);
    QObject::connect(header, &PageHeader::backClicked, panel, [cc, pp] {
        QWidget* a = pp ? pp->anchor() : nullptr;
        if (pp)
            pp->close();
        if (cc && a)
            openViewsMenu(cc, a);
    });
    QObject::connect(header, &PageHeader::closeClicked, panel, [pp] {
        if (pp)
            pp->close();
    });
    QObject::connect(header, &PageHeader::moreClicked, panel, [cc, pp, name, header] {
        if (!cc)
            return;
        QMenu* m = ownedMenu(header);
        const int index = cc->base().viewIndex(*name);
        QAction* def = m->addAction(IconProvider::instance().icon(QStringLiteral("layers-2"), 16), tr("Default view"));
        def->setCheckable(true);
        def->setChecked(index == 0);
        QObject::connect(def, &QAction::triggered, cc, [cc, name] {
            if (cc)
                cc->editBase([&](BaseFile& b) {
                    const int i = b.viewIndex(*name);
                    if (i > 0)
                        b.views.move(i, 0);
                });
        });
        QAction* dup = m->addAction(IconProvider::instance().icon(QStringLiteral("files"), 16), tr("Duplicate view"));
        QObject::connect(dup, &QAction::triggered, cc, [cc, pp, name] {
            if (!cc)
                return;
            const QString copy = cc->base().uniqueViewName(*name);
            cc->editBase([&](BaseFile& b) {
                const int i = b.viewIndex(*name);
                if (i < 0)
                    return;
                BaseView v = b.views[i];
                v.name = copy;
                b.views << v;
            });
            cc->selectView(copy);
            QWidget* a = pp ? pp->anchor() : nullptr;
            if (pp)
                pp->close();
            if (a)
                openConfigureViewMenu(cc, a, copy);
        });
        m->addSeparator();
        QAction* del = m->addAction(IconProvider::instance().icon(QStringLiteral("trash-2"), 16), tr("Delete view"));
        QObject::connect(del, &QAction::triggered, cc, [cc, pp, name] {
            if (!cc)
                return;
            QString next;
            cc->editBase([&](BaseFile& b) {
                const int i = b.viewIndex(*name);
                if (i < 0)
                    return;
                b.views.removeAt(i);
                if (b.views.isEmpty()) {
                    BaseView v;
                    v.name = QStringLiteral("Table");
                    b.views << v;
                }
                next = b.views[std::clamp(i - 1, 0, int(b.views.size()) - 1)].name;
            });
            cc->selectView(next);
            if (pp)
                pp->close();
        });
        m->popup(header->moreButton()->mapToGlobal(QPoint(0, header->moreButton()->height())));
    });
    auto* form = new QWidget(p);
    auto* fl = new QVBoxLayout(form);
    fl->setContentsMargins(0, 4, 0, 8);
    fl->setSpacing(6);
    l->addWidget(form);
    auto* nameInput = textInput(viewName, tr("View name"));
    nameInput->setObjectName(QStringLiteral("ocBasesViewName"));
    fl->addWidget(inputRow(QString(), nameInput, form));
    auto* error = new QLabel(form);
    error->setFont(basesStyle().uiSmaller);
    QPalette ep = error->palette();
    ep.setColor(QPalette::WindowText, basesStyle().error);
    error->setPalette(ep);
    error->setContentsMargins(10, 0, 8, 0);
    error->hide();
    fl->addWidget(error);
    auto commitName = [cc, name, nameInput, error] {
        if (!cc)
            return;
        const QString n = nameInput->text().trimmed();
        QString msg;
        static const QRegularExpression invalid(QStringLiteral("[#:|^]|%%|\\[\\[|\\]\\]"));
        if (n.isEmpty())
            msg = tr("View name cannot be empty.");
        else if (invalid.match(n).hasMatch())
            msg = tr("View name cannot contain: ") + QStringLiteral("# : | ^ %% [[ ]]");
        else if (n != *name && cc->base().viewIndex(n) >= 0)
            msg = tr("View name already exists.");
        error->setText(msg);
        error->setVisible(!msg.isEmpty());
        if (!msg.isEmpty() || n == *name)
            return;
        const QString old = *name;
        const bool current = cc->viewName() == old;
        cc->editBase([&](BaseFile& b) {
            if (BaseView* v = b.view(old))
                v->name = n;
        });
        *name = n;
        if (current)
            cc->selectView(n);
    };
    QObject::connect(nameInput, &QLineEdit::editingFinished, panel, commitName);
    // layout
    auto* layoutDrop = new Dropdown;
    for (const ViewTypeInfo& t : viewTypes())
        layoutDrop->addOption(t.id, t.name);
    const BaseView* view = c->base().view(viewName);
    layoutDrop->setValue(view ? view->type : QStringLiteral("table"));
    fl->addWidget(inputRow(tr("Layout"), layoutDrop, form));
    auto* options = new QWidget(form);
    auto* ol = new QVBoxLayout(options);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->setSpacing(6);
    fl->addWidget(options);
    auto setOption = [cc, name](const QString& key, const YamlNode& value) {
        if (!cc)
            return;
        cc->editBase([&](BaseFile& b) {
            if (BaseView* v = b.view(*name))
                v->setOption(key, value);
        });
    };
    std::function<void()> renderOptions;
    renderOptions = [cc, name, ol, options, setOption, pp] {
        if (!cc)
            return;
        while (QLayoutItem* it = ol->takeAt(0)) {
            if (QWidget* w = it->widget())
                w->deleteLater();
            delete it;
        }
        const BaseView* v = cc->base().view(*name);
        if (!v)
            return;
        auto dropdown = [&](const QString& label, const QString& key, const QList<QPair<QString, QString>>& opts, const QString& def) {
            auto* d = new Dropdown;
            d->setOptions(opts);
            const YamlNode cur = v->option(key);
            d->setValue(cur.isString() ? cur.toString() : def);
            QObject::connect(d, &Dropdown::valueChanged, d, [setOption, key, def](const QString& value) {
                setOption(key, value.isEmpty() || (value == def && def.isEmpty()) ? YamlNode() : YamlNode::string(value));
            });
            ol->addWidget(inputRow(label, d, options));
        };
        auto slider = [&](const QString& label, const QString& key, double min, double max, double step, double def) {
            auto* s = new ValueSlider;
            s->setRange(min, max);
            s->setStep(step);
            const YamlNode cur = v->option(key);
            s->setValue(cur.isNumber() ? cur.toNumber() : def);
            QObject::connect(s, &ValueSlider::valueChanged, s, [setOption, key](double value) { setOption(key, YamlNode::number(value)); });
            ol->addWidget(inputRow(label, s, options));
        };
        if (v->type == u"table") {
            dropdown(tr("Row height"), QStringLiteral("rowHeight"),
                     {{QString(), tr("Short")}, {QStringLiteral("medium"), tr("Medium")}, {QStringLiteral("tall"), tr("Tall")},
                      {QStringLiteral("extra"), tr("Extra tall")}},
                     QString());
        } else if (v->type == u"cards") {
            slider(tr("Card size"), QStringLiteral("cardSize"), 50, 800, 10, 200);
            auto* prop = new Dropdown;
            QList<QPair<QString, QString>> opts{{QString(), tr("Property")}};
            opts += propertyOptions(cc, [](const QString& id) { return id == u"file.file" || !id.startsWith(QLatin1String("file.")); });
            prop->setOptions(opts);
            const YamlNode cur = v->option(QStringLiteral("image"));
            prop->setValue(cur.isString() ? propertyIdFromWritten(cur.toString()) : QString());
            QObject::connect(prop, &Dropdown::valueChanged, prop, [setOption](const QString& value) {
                setOption(QStringLiteral("image"), value.isEmpty() ? YamlNode() : YamlNode::string(value));
            });
            ol->addWidget(inputRow(tr("Image property"), prop, options));
            dropdown(tr("Image fit"), QStringLiteral("imageFit"), {{QString(), tr("Cover")}, {QStringLiteral("contain"), tr("Contain")}}, QString());
            slider(tr("Image aspect ratio"), QStringLiteral("imageAspectRatio"), 0.25, 2.5, 0.05, 1);
        } else if (v->type == u"list") {
            dropdown(tr("Markers"), QStringLiteral("markers"),
                     {{QStringLiteral("bullet"), tr("Bullet")}, {QStringLiteral("number"), tr("Number")}, {QStringLiteral("none"), tr("None")}},
                     QStringLiteral("bullet"));
            auto* toggle = new ToggleSwitch;
            toggle->setChecked(v->option(QStringLiteral("indentProperties")).toBool());
            QObject::connect(toggle, &QAbstractButton::toggled, toggle, [setOption](bool on) {
                setOption(QStringLiteral("indentProperties"), YamlNode::boolean(on));
            });
            auto* row = inputRow(tr("Indent properties"), toggle, options);
            ol->addWidget(row);
            if (!v->option(QStringLiteral("indentProperties")).toBool()) {
                const YamlNode sep = v->option(QStringLiteral("separator"));
                auto* e = textInput(sep.isString() ? sep.toString() : QStringLiteral(", "), QString());
                QObject::connect(e, &QLineEdit::editingFinished, e, [setOption, e] { setOption(QStringLiteral("separator"), YamlNode::string(e->text())); });
                ol->addWidget(inputRow(tr("Property separator"), e, options));
            }
        }
        if (pp)
            QTimer::singleShot(0, pp, [pp] {
                if (pp)
                    pp->reposition();
            });
    };
    renderOptions();
    QObject::connect(layoutDrop, &Dropdown::valueChanged, panel, [cc, name, renderOptions](const QString& type) {
        if (!cc)
            return;
        cc->editBase([&](BaseFile& b) {
            if (BaseView* v = b.view(*name))
                v->type = type;
        });
        renderOptions();
    });
    QObject::connect(c, &BasesController::baseEdited, panel, [renderOptions, name, cc](const BaseFile&) {
        // the list options depend on "indentProperties"
        if (cc && cc->base().view(*name) && cc->base().view(*name)->type == u"list")
            QTimer::singleShot(0, cc, renderOptions);
    });
    panel->push(p);
    panel->popup();
    nameInput->setFocus();
    nameInput->selectAll();
    return panel;
}

// ---------------------------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------------------------
MenuPanel* openResultsMenu(BasesController* c, QWidget* anchor)
{
    auto* panel = new MenuPanel(anchor, 260);
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 4);
    const BaseView* v = c->currentView();
    const double limit = v ? v->limit : 0;
    auto* input = textInput(limit > 0 ? Value::jsNumber(limit) : QString(), tr("e.g. 10"));
    input->setObjectName(QStringLiteral("ocBasesLimit"));
    l->addWidget(inputRow(tr("Limit number of results"), input, p));
    l->addSpacing(4);
    l->addWidget(divider(p));
    auto* showAll = new MenuRow(QStringLiteral("rotate-ccw"), QString(), p);
    auto updateShowAll = [cc, showAll] {
        if (!cc)
            return;
        const BaseView* view = cc->currentView();
        const double lim = view ? view->limit : 0;
        showAll->setText(lim == 0 ? tr("Show all") : tr("Show all (%1)").arg(cc->runner()->resultCount()));
        showAll->setVisible(lim != 0);
    };
    updateShowAll();
    l->addWidget(showAll);
    QObject::connect(input, &QLineEdit::textEdited, panel, [cc, input, updateShowAll](const QString& text) {
        if (!cc)
            return;
        if (text.trimmed().isEmpty()) {
            cc->editView([](BaseView& view) { view.limit = 0; });
            input->setStyleSheet(QString());
        } else {
            bool ok = false;
            const int n = text.trimmed().toInt(&ok);
            if (!ok || n < 0) {
                input->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid %1; }").arg(basesStyle().error.name()));
                return;
            }
            input->setStyleSheet(QString());
            cc->editView([n](BaseView& view) { view.limit = n; });
        }
        updateShowAll();
    });
    QObject::connect(input, &QLineEdit::returnPressed, panel, [pp] {
        if (pp)
            pp->close();
    });
    QObject::connect(showAll, &MenuRow::clicked, panel, [cc, pp] {
        if (cc)
            cc->editView([](BaseView& view) { view.limit = 0; });
        if (pp)
            pp->close();
    });
    auto* copy = new MenuRow(QStringLiteral("copy"), tr("Copy to clipboard"), p);
    l->addWidget(copy);
    QObject::connect(copy, &MenuRow::clicked, panel, [cc, pp] {
        if (pp)
            pp->close();
        if (cc)
            cc->copyTable();
    });
    auto* csv = new MenuRow(QStringLiteral("file-down"), tr("Export CSV..."), p);
    l->addWidget(csv);
    QObject::connect(csv, &MenuRow::clicked, panel, [cc, pp] {
        if (pp)
            pp->close();
        if (!cc)
            return;
        const QString text = cc->exportTable().toCsv();
        const QString suggested = (cc->viewName().isEmpty() ? QStringLiteral("export") : cc->viewName()) + QStringLiteral(".csv");
        auto* dlg = new QFileDialog(cc, tr("Export CSV..."), suggested, tr("CSV files (*.csv)"));
        dlg->setAcceptMode(QFileDialog::AcceptSave);
        dlg->setOption(QFileDialog::DontUseNativeDialog);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(dlg, &QFileDialog::fileSelected, cc, [text](const QString& path) {
            QSaveFile f(path);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(text.toUtf8());
                f.commit();
            }
        });
        dlg->open();
    });
    panel->push(p);
    panel->popup();
    input->setFocus();
    return panel;
}

// ---------------------------------------------------------------------------------------------
// Sort / group
// ---------------------------------------------------------------------------------------------
namespace {

QList<QPair<QString, QString>> directionOptions(BasesController* c, const QString& prop)
{
    const ValueType t = prop.isEmpty() ? ValueType::String : lhsType(c, prop);
    if (t == ValueType::Date)
        return {{QStringLiteral("ASC"), tr("Old to new")}, {QStringLiteral("DESC"), tr("New to old")}};
    if (t == ValueType::Number)
        return {{QStringLiteral("ASC"), tr("0 → 1")}, {QStringLiteral("DESC"), tr("1 → 0")}};
    return {{QStringLiteral("ASC"), tr("A → Z")}, {QStringLiteral("DESC"), tr("Z → A")}};
}

// A sort / group row: property picker, direction, delete.
QWidget* sortRow(BasesController* c, const QString& property, const QString& direction, const std::function<bool(const QString&)>& filter,
                 const std::function<void(const QString&, const QString&)>& changed, const std::function<void()>& removed)
{
    auto* row = new QWidget;
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(8, 2, 8, 2);
    l->setSpacing(4);
    auto* prop = new Dropdown(row);
    QList<QPair<QString, QString>> opts{{QString(), tr("Property")}};
    opts += propertyOptions(c, filter);
    prop->setOptions(opts);
    prop->setValue(property);
    auto* dir = new Dropdown(row);
    dir->setOptions(directionOptions(c, property));
    dir->setValue(direction.isEmpty() ? QStringLiteral("ASC") : direction);
    auto* del = new TextIconButton(QStringLiteral("trash-2"), QString(), row);
    l->addWidget(prop, 1);
    l->addWidget(dir);
    l->addWidget(del);
    QPointer<BasesController> cc = c;
    QObject::connect(prop, &Dropdown::valueChanged, row, [cc, dir, changed](const QString& v) {
        if (cc) {
            const QString d = dir->value();
            dir->setOptions(directionOptions(cc, v));
            dir->setValue(d);
        }
        changed(v, dir->value());
    });
    QObject::connect(dir, &Dropdown::valueChanged, row, [prop, changed](const QString& d) { changed(prop->value(), d); });
    QObject::connect(del, &QAbstractButton::clicked, row, removed);
    return row;
}

} // namespace

MenuPanel* openSortMenu(BasesController* c, QWidget* anchor)
{
    auto* panel = new MenuPanel(anchor, 340);
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 0);
    // group by
    l->addWidget(new SectionHeader(tr("Group by"), false, p));
    auto* groupHolder = new QWidget(p);
    auto* gl = new QVBoxLayout(groupHolder);
    gl->setContentsMargins(0, 0, 0, 8);
    l->addWidget(groupHolder);
    l->addWidget(divider(p));
    l->addWidget(new SectionHeader(tr("Sort by"), false, p));
    auto* sortHolder = new QWidget(p);
    auto* sl = new QVBoxLayout(sortHolder);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(4);
    l->addWidget(sortHolder);
    auto* add = new TextIconButton(QStringLiteral("plus"), tr("Add sort"), p);
    add->setObjectName(QStringLiteral("ocBasesAddSort"));
    auto* addRowW = new QWidget(p);
    auto* al = new QHBoxLayout(addRowW);
    al->setContentsMargins(8, 4, 8, 8);
    al->addWidget(add);
    al->addStretch();
    l->addWidget(addRowW);

    auto pending = std::make_shared<int>(0); // empty "Add sort" rows
    auto render = std::make_shared<std::function<void()>>();
    *render = [cc, pp, gl, groupHolder, sl, sortHolder, pending, render] {
        if (!cc)
            return;
        for (QVBoxLayout* layout : {gl, sl})
            while (QLayoutItem* it = layout->takeAt(0)) {
                if (QWidget* w = it->widget())
                    w->deleteLater();
                delete it;
            }
        const BaseView* v = cc->currentView();
        static const QStringList noGroup = {QStringLiteral("file.file"), QStringLiteral("file.path"), QStringLiteral("file.ctime"),
                                            QStringLiteral("file.mtime"), QStringLiteral("file.name"), QStringLiteral("file.fullname"),
                                            QStringLiteral("file.basename")};
        const std::optional<SortSpec> group = v ? v->groupBy : std::nullopt;
        gl->addWidget(sortRow(
            cc, group ? group->property : QString(), group ? group->direction : QStringLiteral("ASC"),
            [](const QString& id) { return !noGroup.contains(id); },
            [cc](const QString& prop, const QString& dir) {
                if (cc)
                    cc->editView([&](BaseView& view) {
                        if (prop.isEmpty())
                            view.groupBy.reset();
                        else
                            view.groupBy = SortSpec{prop, dir};
                    });
            },
            [cc, render] {
                if (cc)
                    cc->editView([](BaseView& view) { view.groupBy.reset(); });
                QTimer::singleShot(0, cc, *render);
            }));
        const QList<SortSpec> sorts = v ? v->sortSpecs() : QList<SortSpec>();
        auto sortFilter = [](const QString& id) { return id != u"file.file" && id != u"file.backlinks"; };
        for (int i = 0; i < sorts.size(); ++i) {
            sl->addWidget(sortRow(
                cc, sorts[i].property, sorts[i].direction, sortFilter,
                [cc, i](const QString& prop, const QString& dir) {
                    if (cc)
                        cc->editView([&](BaseView& view) {
                            QList<SortSpec> s = view.sortSpecs();
                            if (i >= s.size())
                                return;
                            if (prop.isEmpty())
                                s.removeAt(i);
                            else
                                s[i] = SortSpec{prop, dir};
                            view.sort = s;
                        });
                },
                [cc, i, render] {
                    if (cc)
                        cc->editView([&](BaseView& view) {
                            QList<SortSpec> s = view.sortSpecs();
                            if (i < s.size())
                                s.removeAt(i);
                            view.sort = s;
                        });
                    QTimer::singleShot(0, cc, *render);
                }));
        }
        const int empties = sorts.isEmpty() && *pending == 0 ? 1 : *pending;
        for (int k = 0; k < empties; ++k) {
            sl->addWidget(sortRow(
                cc, QString(), QStringLiteral("ASC"), sortFilter,
                [cc, pending, render](const QString& prop, const QString& dir) {
                    if (!cc || prop.isEmpty())
                        return;
                    *pending = std::max(0, *pending - 1);
                    cc->editView([&](BaseView& view) {
                        QList<SortSpec> s = view.sortSpecs();
                        s << SortSpec{prop, dir};
                        view.sort = s;
                    });
                    QTimer::singleShot(0, cc, *render);
                },
                [pending, render, cc] {
                    *pending = std::max(0, *pending - 1);
                    QTimer::singleShot(0, cc, *render);
                }));
        }
        if (pp)
            QTimer::singleShot(0, pp, [pp] {
                if (pp)
                    pp->reposition();
            });
        Q_UNUSED(groupHolder);
        Q_UNUSED(sortHolder);
    };
    (*render)();
    QObject::connect(add, &QAbstractButton::clicked, panel, [pending, render] {
        ++*pending;
        (*render)();
    });
    panel->push(p);
    panel->popup();
    return panel;
}

// ---------------------------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------------------------
namespace {

struct OpConfig {
    const char* func;
    const char* display;
    const char* inverse;
};
const OpConfig kOpConfigs[] = {
    {"hasProperty", "has property", "does not have property"},
    {"isEmpty", "is empty", "is not empty"},
    {"startsWith", "starts with", "does not start with"},
    {"endsWith", "ends with", "does not end with"},
    {"contains", "contains", "does not contain"},
    {"containsAny", "contains any of", "does not contain any of"},
    {"containsAll", "contains all of", "does not contain all of"},
    {"matches", "matches", "does not match"},
    {"hasLink", "links to", "does not link to"},
    {"inFolder", "in folder", "is not in folder"},
    {"hasTag", "has tag", "does not have tag"},
};

QString opDisplay(const QString& func, bool inverse)
{
    for (const OpConfig& c : kOpConfigs)
        if (func.compare(QLatin1String(c.func), Qt::CaseInsensitive) == 0)
            return QLatin1String(inverse ? c.inverse : c.display);
    return inverse ? u'!' + func : func;
}

// Obsidian's dZ(): the property a filter's left-hand side names.
std::optional<QString> lhsProperty(const ExprNode& e)
{
    using K = ExprNode::Kind;
    if (e.kind == K::ObjectAccess) {
        const ExprNode& o = *e.children[0];
        if (o.kind == K::Ident && (o.name == u"note" || o.name == u"formula" || o.name == u"file"))
            return o.name + u'.' + e.name;
    } else if (e.kind == K::ArrayAccess) {
        const ExprNode& a = *e.children[0];
        const ExprNode& i = *e.children[1];
        if (a.kind == K::Ident && (a.name == u"file" || a.name == u"formula" || a.name == u"note") && i.kind == K::Primitive)
            return a.name + u'.' + i.value.toString();
    } else if (e.kind == K::Ident) {
        if (e.name == u"this")
            return std::nullopt;
        return propertyIdFromWritten(e.name);
    }
    return std::nullopt;
}

// Obsidian's fZ(): the right-hand side values.
std::optional<QStringList> rhsValues(const QList<ExprPtr>& args, bool* isList)
{
    using K = ExprNode::Kind;
    *isList = false;
    if (args.isEmpty())
        return QStringList{QString()};
    if (args.size() > 1) {
        QStringList out;
        for (const ExprPtr& a : args) {
            if (a->kind == K::Primitive)
                out << a->value.toString();
            else if (a->kind == K::Call && a->name == u"link" && !a->subject && a->children.size() == 1 && a->children[0]->kind == K::Primitive)
                out << QStringLiteral("[[") + a->children[0]->value.toString() + QStringLiteral("]]");
            else
                return std::nullopt;
        }
        *isList = true;
        return out;
    }
    const ExprNode& r = *args[0];
    if (r.kind == K::Call && r.name == u"link" && !r.subject && !r.children.isEmpty() && r.children[0]->kind == K::Primitive) {
        const QString target = r.children[0]->value.toString();
        if (r.children.size() > 1 && r.children[1]->kind == K::Primitive)
            return QStringList{QStringLiteral("[[") + target + u'|' + r.children[1]->value.toString() + QStringLiteral("]]")};
        return QStringList{QStringLiteral("[[") + target + QStringLiteral("]]")};
    }
    if (r.kind == K::Array) {
        QStringList out;
        for (const ExprPtr& el : r.children) {
            if (el->kind != K::Primitive)
                return std::nullopt;
            out << el->value.toString();
        }
        *isList = true;
        return out;
    }
    if (r.kind == K::Primitive)
        return QStringList{r.value.isNull() ? QString() : r.value.toString()};
    return std::nullopt;
}

QString quoteValue(const QString& v, ValueType lhs)
{
    if (v.startsWith(QLatin1String("[[")) && v.endsWith(QLatin1String("]]"))) {
        const QString inner = v.mid(2, v.size() - 4);
        const qsizetype pipe = inner.indexOf(u'|');
        if (pipe >= 0)
            return QStringLiteral("link(\"%1\", \"%2\")").arg(inner.left(pipe), inner.mid(pipe + 1));
        return QStringLiteral("link(\"%1\")").arg(inner);
    }
    const QStringList parts = v.split(u'.');
    if (parts.size() >= 2) {
        static const QStringList scopes = {QStringLiteral("note"), QStringLiteral("formula"), QStringLiteral("file"), QStringLiteral("this")};
        if (scopes.contains(parts[1]))
            return v;
    }
    if (lhs == ValueType::Number) {
        bool ok = false;
        v.trimmed().toDouble(&ok);
        if (ok)
            return v.trimmed();
    }
    if (lhs == ValueType::Boolean && (v == u"true" || v == u"false"))
        return v;
    return u'"' + v + u'"';
}

} // namespace

std::optional<SimpleFilter> parseSimpleFilter(const QString& expression)
{
    using K = ExprNode::Kind;
    const ExprPtr root = parseExpression(expression);
    if (root->kind == K::Empty)
        return SimpleFilter{};
    const ExprNode* n = root.get();
    SimpleFilter f;
    if (n->kind == K::Not) {
        f.inverse = true;
        n = n->children[0].get();
    }
    if (n->kind == K::Compare) {
        static const QStringList ops = {QStringLiteral("=="), QStringLiteral("!="), QStringLiteral("<"), QStringLiteral(">"),
                                        QStringLiteral("<="), QStringLiteral(">=")};
        if (!ops.contains(n->name) || f.inverse)
            return std::nullopt;
        const std::optional<QString> lhs = lhsProperty(*n->children[0]);
        bool list = false;
        const std::optional<QStringList> rhs = rhsValues({n->children[1]}, &list);
        if (!lhs || !rhs)
            return std::nullopt;
        f.property = *lhs;
        f.op = n->name;
        f.values = *rhs;
        f.isList = list;
        return f;
    }
    if (n->kind == K::Call && n->subject) {
        const std::optional<QString> lhs = lhsProperty(*n->subject);
        bool list = false;
        const std::optional<QStringList> rhs = rhsValues(n->children, &list);
        if (!lhs || !rhs)
            return std::nullopt;
        f.property = *lhs;
        f.op = n->name;
        f.values = n->children.isEmpty() ? QStringList() : *rhs;
        f.isList = list;
        return f;
    }
    return std::nullopt;
}

QString serializeSimpleFilter(const SimpleFilter& f, ValueType lhsType)
{
    if (f.property.isEmpty())
        return {};
    const QString lhs = propertyExpression(f.property);
    static const QStringList ops = {QStringLiteral("=="), QStringLiteral("!="), QStringLiteral("<"), QStringLiteral(">"), QStringLiteral("<="),
                                    QStringLiteral(">=")};
    if (ops.contains(f.op)) {
        if (f.values.isEmpty() || f.values[0].isEmpty())
            return {};
        return lhs + u' ' + f.op + u' ' + quoteValue(f.values[0], lhsType);
    }
    QStringList args;
    for (const QString& v : f.values)
        if (!v.isEmpty())
            args << quoteValue(v, ValueType::String);
    const bool takesValue = f.op.compare(QLatin1String("isEmpty"), Qt::CaseInsensitive) != 0;
    if (takesValue && args.isEmpty())
        return {};
    const QString call = lhs + u'.' + f.op + u'(' + args.join(QStringLiteral(", ")) + u')';
    return f.inverse ? u'!' + call : call;
}

QList<FilterOperator> filterOperators(ValueType t)
{
    QList<FilterOperator> out;
    auto cmp = [&](const QString& value, const QString& display) { out << FilterOperator{value, display, QString(), false, true, false}; };
    if (t != ValueType::File) {
        const bool number = t == ValueType::Number, date = t == ValueType::Date, list = t == ValueType::List || t == ValueType::TagList;
        cmp(QStringLiteral("=="), number ? QStringLiteral("=") : date ? tr("on") : list ? tr("is exactly") : tr("is"));
        cmp(QStringLiteral("!="), number ? QStringLiteral("≠") : date ? tr("not on") : list ? tr("is not exactly") : tr("is not"));
        if (number || date) {
            cmp(QStringLiteral("<"), number ? QStringLiteral("<") : tr("before"));
            cmp(QStringLiteral("<="), number ? QStringLiteral("≤") : tr("on or before"));
            cmp(QStringLiteral(">"), number ? QStringLiteral(">") : tr("after"));
            cmp(QStringLiteral(">="), number ? QStringLiteral("≥") : tr("on or after"));
        }
    }
    // the boolean functions of the type (Obsidian's OK functions), then their negations
    QStringList funcs;
    switch (t) {
    case ValueType::String:
    case ValueType::Link:
    case ValueType::Url:
        funcs = {QStringLiteral("startsWith"), QStringLiteral("endsWith"), QStringLiteral("isEmpty"), QStringLiteral("contains"),
                 QStringLiteral("containsAny"), QStringLiteral("containsAll")};
        if (t == ValueType::Link)
            funcs << QStringLiteral("linksTo");
        break;
    case ValueType::Number:
    case ValueType::Date:
    case ValueType::Null:
        funcs = {QStringLiteral("isEmpty")};
        break;
    case ValueType::List:
    case ValueType::TagList:
        funcs = {QStringLiteral("isEmpty"), QStringLiteral("contains"), QStringLiteral("containsAny"), QStringLiteral("containsAll")};
        break;
    case ValueType::Object:
        funcs = {QStringLiteral("isEmpty")};
        break;
    case ValueType::File:
        funcs = {QStringLiteral("hasLink"), QStringLiteral("inFolder"), QStringLiteral("hasTag"), QStringLiteral("hasProperty")};
        break;
    default:
        break;
    }
    for (int inv = 0; inv < 2; ++inv)
        for (const QString& f : std::as_const(funcs)) {
            FilterOperator op;
            op.function = f;
            op.inverse = inv == 1;
            op.value = (op.inverse ? QStringLiteral("!") : QString()) + f.toLower();
            op.display = opDisplay(f, op.inverse);
            op.takesValue = f != u"isEmpty";
            op.variadic = f == u"containsAny" || f == u"containsAll" || f == u"hasTag";
            out << op;
        }
    return out;
}

namespace {

class FilterGroupEditor;

// One statement (Obsidian's pZ): property + operator + value, or an advanced formula.
class FilterRowEditor : public QWidget {
public:
    FilterRowEditor(BasesController* c, const QString& text, std::function<void()> changed, std::function<void()> removed)
        : m_controller(c)
        , m_changed(std::move(changed))
    {
        setObjectName(QStringLiteral("ocBasesFilterRow"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);
        auto* line = new QWidget(this);
        auto* l = new QHBoxLayout(line);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(2);
        m_property = new Dropdown(line);
        QList<QPair<QString, QString>> opts{{QString(), tr("Property")}};
        opts += propertyOptions(c, [](const QString& id) { return id != u"note.tags" && id != u"file.backlinks"; });
        m_property->setOptions(opts);
        m_operator = new Dropdown(line);
        m_value = new QLineEdit(line);
        m_value->setFont(basesStyle().uiSmall);
        m_value->setMinimumWidth(80);
        m_advanced = new QLineEdit(line);
        m_advanced->setFont(currentTheme()->monospaceFont(basesStyle().uiSmall.pixelSize()));
        m_advanced->setPlaceholderText(tr("Advanced filter"));
        m_mode = new TextIconButton(QStringLiteral("code-xml"), QString(), line);
        m_mode->setToolTip(tr("Advanced filter"));
        auto* del = new TextIconButton(QStringLiteral("trash-2"), QString(), line);
        del->setToolTip(tr("Remove filter"));
        l->addWidget(m_property, 2);
        l->addWidget(m_operator, 1);
        l->addWidget(m_value, 2);
        l->addWidget(m_advanced, 5);
        l->addWidget(m_mode);
        l->addWidget(del);
        v->addWidget(line);
        m_error = new QLabel(this);
        m_error->setFont(basesStyle().uiSmaller);
        QPalette ep = m_error->palette();
        ep.setColor(QPalette::WindowText, basesStyle().error);
        m_error->setPalette(ep);
        m_error->hide();
        v->addWidget(m_error);
        connect(del, &QAbstractButton::clicked, this, [removed] { QTimer::singleShot(0, removed); });
        connect(m_mode, &QAbstractButton::clicked, this, [this] { toggleMode(); });
        connect(m_property, &Dropdown::valueChanged, this, [this] {
            updateOperators();
            emitChange();
        });
        connect(m_operator, &Dropdown::valueChanged, this, [this] {
            updateValueWidget();
            emitChange();
        });
        connect(m_value, &QLineEdit::editingFinished, this, [this] { emitChange(); });
        connect(m_advanced, &QLineEdit::editingFinished, this, [this] { emitChange(); });
        setText(text);
    }

    QString text() const
    {
        if (m_advancedMode)
            return m_advanced->text();
        SimpleFilter f;
        f.property = m_property->value();
        const QString op = m_operator->value();
        f.inverse = op.startsWith(u'!');
        QString bare = f.inverse ? op.mid(1) : op;
        // the function's spelling
        for (const FilterOperator& o : filterOperators(lhsType(m_controller, f.property)))
            if (o.value == op && !o.function.isEmpty())
                bare = o.function;
        f.op = bare;
        const bool variadic = isVariadic(op);
        const QString raw = m_value->text();
        if (variadic) {
            for (const QString& part : raw.split(u','))
                if (!part.trimmed().isEmpty())
                    f.values << part.trimmed();
        } else {
            f.values << raw;
        }
        return serializeSimpleFilter(f, lhsType(m_controller, f.property));
    }

    void setText(const QString& text)
    {
        const std::optional<SimpleFilter> f = parseSimpleFilter(text);
        m_advancedMode = !f;
        if (f && !f->property.isEmpty()) {
            m_property->setValue(f->property);
            if (m_property->value() != f->property) {
                m_property->addOption(f->property, m_controller->base().displayName(f->property));
                m_property->setValue(f->property);
            }
        } else if (f) {
            m_property->setValue(QStringLiteral("file.file"));
        }
        updateOperators();
        if (f && !f->op.isEmpty()) {
            const QString value = (f->inverse ? QStringLiteral("!") : QString()) + (f->op.startsWith(u'=') || f->op.startsWith(u'<') || f->op.startsWith(u'>')
                                                                                            || f->op.startsWith(u'!')
                                                                                        ? f->op
                                                                                        : f->op.toLower());
            m_operator->setValue(value);
            if (m_operator->value() != value)
                m_advancedMode = true; // an operator the property's type doesn't offer
        }
        m_value->setText(f ? f->values.join(QStringLiteral(", ")) : QString());
        m_advanced->setText(text);
        updateValueWidget();
        reconcile();
    }

private:
    bool isVariadic(const QString& op) const
    {
        for (const FilterOperator& o : filterOperators(lhsType(m_controller, m_property->value())))
            if (o.value == op)
                return o.variadic;
        return false;
    }
    void updateOperators()
    {
        const QString cur = m_operator->value();
        QList<QPair<QString, QString>> opts;
        for (const FilterOperator& o : filterOperators(lhsType(m_controller, m_property->value())))
            opts.append({o.value, o.display});
        m_operator->setOptions(opts);
        m_operator->setValue(cur);
        if (m_operator->value() != cur && !opts.isEmpty())
            m_operator->setValue(opts.first().first);
        updateValueWidget();
    }
    void updateValueWidget()
    {
        bool takes = true;
        for (const FilterOperator& o : filterOperators(lhsType(m_controller, m_property->value())))
            if (o.value == m_operator->value())
                takes = o.takesValue;
        m_value->setVisible(!m_advancedMode && takes);
    }
    void reconcile()
    {
        m_property->setVisible(!m_advancedMode);
        m_operator->setVisible(!m_advancedMode);
        updateValueWidget();
        m_advanced->setVisible(m_advancedMode);
        m_mode->setIconName(m_advancedMode ? QStringLiteral("mouse-pointer-click") : QStringLiteral("code-xml"));
        m_mode->setToolTip(m_advancedMode ? tr("Simple filter") : tr("Advanced filter"));
        const Formula f(text());
        m_error->setVisible(m_advancedMode && f.isInvalid() && !text().isEmpty());
        m_error->setText(f.errorMessage());
    }
    void toggleMode()
    {
        const QString current = text();
        if (m_advancedMode) {
            if (current.trimmed().isEmpty()) {
                m_advancedMode = false;
                setText(QString());
                return;
            }
            const Formula f(current);
            if (f.isInvalid()) {
                m_controller->notice(tr("Filter cannot be parsed. Basic filter builder cannot be used."));
                reconcile();
                return;
            }
            setText(current);
            if (m_advancedMode)
                m_controller->notice(tr("This filter cannot be represented by the simple filter builder."));
            return;
        }
        m_advancedMode = true;
        m_advanced->setText(current);
        reconcile();
    }
    void emitChange()
    {
        reconcile();
        if (m_changed)
            m_changed();
    }

    BasesController* m_controller;
    std::function<void()> m_changed;
    Dropdown* m_property;
    Dropdown* m_operator;
    QLineEdit* m_value;
    QLineEdit* m_advanced;
    TextIconButton* m_mode;
    QLabel* m_error;
    bool m_advancedMode = false;
};

// A group (Obsidian's hZ): conjunction, statements, "Add filter" / "Add filter group".
class FilterGroupEditor : public QFrame {
public:
    FilterGroupEditor(BasesController* c, const Filter& group, bool nested, std::function<void()> changed, std::function<void()> removed)
        : m_controller(c)
        , m_changed(std::move(changed))
        , m_nested(nested)
    {
        setObjectName(QStringLiteral("ocBasesFilterGroup"));
        if (nested)
            setStyleSheet(QStringLiteral("#ocBasesFilterGroup { background: %1; border: 1px solid %2; border-radius: 8px; }")
                              .arg(basesStyle().bgPrimaryAlt.name(QColor::HexArgb), basesStyle().tableBorder.name(QColor::HexArgb)));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(nested ? 6 : 0, nested ? 6 : 0, nested ? 6 : 0, nested ? 6 : 0);
        v->setSpacing(6);
        auto* head = new QWidget(this);
        auto* hl = new QHBoxLayout(head);
        hl->setContentsMargins(0, 0, 0, 0);
        m_conj = new Dropdown(head);
        m_conj->setOptions({{QStringLiteral("and"), tr("All the following are true")},
                            {QStringLiteral("or"), tr("Any of the following are true")},
                            {QStringLiteral("not"), tr("None of the following are true")}});
        m_conj->setValue(group.kind == Filter::Kind::Or ? QStringLiteral("or") : group.kind == Filter::Kind::Not ? QStringLiteral("not")
                                                                                                                   : QStringLiteral("and"));
        hl->addWidget(m_conj);
        hl->addStretch();
        if (nested) {
            auto* del = new TextIconButton(QStringLiteral("trash-2"), QString(), head);
            connect(del, &QAbstractButton::clicked, this, [removed] { QTimer::singleShot(0, removed); });
            hl->addWidget(del);
        }
        v->addWidget(head);
        m_rows = new QWidget(this);
        m_rowsLayout = new QVBoxLayout(m_rows);
        m_rowsLayout->setContentsMargins(0, 0, 0, 0);
        m_rowsLayout->setSpacing(6);
        v->addWidget(m_rows);
        auto* actions = new QWidget(this);
        auto* al = new QHBoxLayout(actions);
        al->setContentsMargins(0, 0, 0, 0);
        auto* addFilter = new TextIconButton(QStringLiteral("plus"), tr("Add filter"), actions);
        addFilter->setObjectName(QStringLiteral("ocBasesAddFilter"));
        auto* addGroup = new TextIconButton(QStringLiteral("plus"), tr("Add filter group"), actions);
        al->addWidget(addFilter);
        al->addWidget(addGroup);
        al->addStretch();
        v->addWidget(actions);
        connect(m_conj, &Dropdown::valueChanged, this, [this] {
            renderLabels();
            notify();
        });
        connect(addFilter, &QAbstractButton::clicked, this, [this] {
            addChild(Filter::expr(QString()));
            renderLabels();
        });
        connect(addGroup, &QAbstractButton::clicked, this, [this] {
            addChild(Filter::group(Filter::Kind::And, {}));
            renderLabels();
        });
        if (group.children.isEmpty())
            addChild(Filter::expr(QString()));
        for (const Filter& child : group.children)
            addChild(child);
        renderLabels();
    }

    // Obsidian's serialize(): null when no statement is complete.
    std::optional<Filter> filter() const
    {
        QList<Filter> kids;
        for (const Child& c : m_children) {
            if (c.row) {
                const QString t = c.row->text();
                if (!t.isEmpty())
                    kids << Filter::expr(t);
            } else if (c.group) {
                if (std::optional<Filter> f = c.group->filter())
                    kids << *f;
            }
        }
        if (kids.isEmpty())
            return std::nullopt;
        const QString conj = m_conj->value();
        return Filter::group(conj == u"or" ? Filter::Kind::Or : conj == u"not" ? Filter::Kind::Not : Filter::Kind::And, kids);
    }

private:
    struct Child {
        QWidget* holder = nullptr;
        QLabel* label = nullptr;
        FilterRowEditor* row = nullptr;
        FilterGroupEditor* group = nullptr;
    };

    void addChild(const Filter& f)
    {
        auto* holder = new QWidget(m_rows);
        auto* hl = new QHBoxLayout(holder);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(8);
        auto* label = new QLabel(holder);
        label->setFont(basesStyle().uiSmall);
        label->setFixedWidth(52);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QPalette lp = label->palette();
        lp.setColor(QPalette::WindowText, basesStyle().muted);
        label->setPalette(lp);
        hl->addWidget(label, 0, Qt::AlignTop);
        Child c;
        c.holder = holder;
        c.label = label;
        auto remove = [this, holder] { removeChild(holder); };
        if (f.kind == Filter::Kind::Expr) {
            c.row = new FilterRowEditor(m_controller, f.text, [this] { notify(); }, remove);
            hl->addWidget(c.row, 1);
        } else {
            c.group = new FilterGroupEditor(m_controller, f, true, [this] { notify(); }, remove);
            label->hide();
            hl->addWidget(c.group, 1);
        }
        m_rowsLayout->addWidget(holder);
        m_children << c;
    }

    void removeChild(QWidget* holder)
    {
        for (qsizetype i = 0; i < m_children.size(); ++i) {
            if (m_children[i].holder != holder)
                continue;
            m_children.removeAt(i);
            holder->hide();
            holder->deleteLater();
            break;
        }
        if (m_children.isEmpty())
            addChild(Filter::expr(QString()));
        renderLabels();
        notify();
    }

    void renderLabels()
    {
        const QString conj = m_conj->value();
        for (qsizetype i = 0; i < m_children.size(); ++i)
            m_children[i].label->setText(i == 0 ? tr("where") : conj == u"and" ? tr("and") : tr("or"));
    }

    void notify()
    {
        if (m_changed)
            m_changed();
    }

    BasesController* m_controller;
    std::function<void()> m_changed;
    bool m_nested;
    Dropdown* m_conj;
    QWidget* m_rows;
    QVBoxLayout* m_rowsLayout;
    QList<Child> m_children;
};

int filterCount(const std::optional<Filter>& f)
{
    if (!f)
        return 0;
    return f->kind == Filter::Kind::Expr ? 1 : int(f->children.size());
}

} // namespace

MenuPanel* openFilterMenu(BasesController* c, QWidget* anchor)
{
    const int width = int(currentTheme()->px(QStringLiteral("--bases-filter-menu-width"), -1, 520));
    auto* panel = new MenuPanel(anchor, width);
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 0);
    auto section = [&](const QString& title, bool global, bool collapsed) {
        auto* header = new SectionHeader(title, true, p);
        header->setCollapsed(collapsed);
        l->addWidget(header);
        auto* body = new QWidget(p);
        auto* bl = new QVBoxLayout(body);
        bl->setContentsMargins(8, 0, 8, 8);
        std::optional<Filter> current = global ? cc->base().filters : (cc->currentView() ? cc->currentView()->filters : std::nullopt);
        header->setFlair(filterCount(current) > 0 ? QString::number(filterCount(current)) : QString());
        Filter group = current && current->kind != Filter::Kind::Expr ? *current : Filter::group(Filter::Kind::And, current ? QList<Filter>{*current}
                                                                                                                         : QList<Filter>());
        auto holder = std::make_shared<FilterGroupEditor*>(nullptr);
        auto* editor = new FilterGroupEditor(cc, group, false, [cc, holder, global, header] {
            if (!cc || !*holder)
                return;
            const std::optional<Filter> f = (*holder)->filter();
            header->setFlair(filterCount(f) > 0 ? QString::number(filterCount(f)) : QString());
            cc->editBase([&](BaseFile& b) {
                if (global) {
                    b.filters = f;
                } else if (BaseView* v = b.view(cc->viewName())) {
                    v->filters = f;
                }
            });
        }, {});
        *holder = editor;
        editor->setObjectName(global ? QStringLiteral("ocBasesGlobalFilters") : QStringLiteral("ocBasesViewFilters"));
        bl->addWidget(editor);
        body->setVisible(!collapsed);
        l->addWidget(body);
        QObject::connect(header, &SectionHeader::toggled, body, [body, pp](bool isCollapsed) {
            body->setVisible(!isCollapsed);
            if (pp)
                pp->reposition();
        });
    };
    section(tr("All views"), true, true);
    l->addWidget(divider(p));
    section(tr("This view"), false, false);
    panel->push(p);
    panel->popup();
    return panel;
}

// ---------------------------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------------------------
namespace {

// The "Edit property" page: display name, and the formula for formulas.
QWidget* editPropertyPage(BasesController* c, MenuPanel* panel, const QString& propertyId, bool isNew)
{
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 0);
    const PropertyRef ref = parsePropertyId(propertyId);
    auto* header = new PageHeader(isNew ? tr("Edit property") : tr("Edit %1").arg(ref.name), true, ref.type == u"formula", true, p);
    l->addWidget(header);
    QObject::connect(header, &PageHeader::backClicked, p, [pp] {
        if (pp)
            pp->back();
    });
    QObject::connect(header, &PageHeader::closeClicked, p, [pp] {
        if (pp)
            pp->close();
    });
    QObject::connect(header, &PageHeader::moreClicked, p, [cc, pp, ref, header] {
        QMenu* m = ownedMenu(header);
        QAction* del = m->addAction(IconProvider::instance().icon(QStringLiteral("trash-2"), 16), tr("Delete formula"));
        QObject::connect(del, &QAction::triggered, cc, [cc, pp, ref] {
            if (cc)
                cc->editBase([&](BaseFile& b) { b.removeFormula(ref.name); });
            if (pp)
                pp->back();
        });
        m->popup(header->moreButton()->mapToGlobal(QPoint(0, header->moreButton()->height())));
    });
    auto* form = new QWidget(p);
    auto* fl = new QVBoxLayout(form);
    fl->setContentsMargins(0, 4, 0, 8);
    fl->setSpacing(6);
    l->addWidget(form);
    auto* display = textInput(c->base().displayName(propertyId), tr("Display name"));
    display->setObjectName(QStringLiteral("ocBasesDisplayName"));
    fl->addWidget(inputRow(tr("Display name"), display, form));
    QObject::connect(display, &QLineEdit::editingFinished, p, [cc, propertyId, display] {
        if (!cc)
            return;
        const QString n = display->text().trimmed();
        const QString def = [&] {
            const QString label = filePropertyLabel(propertyId);
            return label.isEmpty() ? parsePropertyId(propertyId).name : label;
        }();
        cc->editBase([&](BaseFile& b) { b.setDisplayName(propertyId, n == def ? QString() : n); });
    });
    if (ref.type == u"formula") {
        const FormulaPtr f = c->base().formula(ref.name);
        auto* editor = textInput(f ? f->text() : QString(), QStringLiteral("x + y"));
        editor->setObjectName(QStringLiteral("ocBasesFormula"));
        editor->setFont(currentTheme()->monospaceFont(basesStyle().uiSmall.pixelSize()));
        fl->addWidget(inputRow(tr("Formula"), editor, form));
        auto* status = new QLabel(form);
        status->setFont(basesStyle().uiSmaller);
        status->setContentsMargins(10, 0, 8, 0);
        auto updateStatus = [status, editor] {
            const Formula formula(editor->text());
            const bool bad = formula.isInvalid();
            QPalette sp = status->palette();
            sp.setColor(QPalette::WindowText, editor->text().isEmpty() ? basesStyle().faint : bad ? basesStyle().error : basesStyle().success);
            status->setPalette(sp);
            status->setText(bad ? tr("Invalid formula") : tr("Valid formula"));
            status->setToolTip(bad ? formula.errorMessage() : QString());
        };
        updateStatus();
        fl->addWidget(status);
        QObject::connect(editor, &QLineEdit::textChanged, p, updateStatus);
        QObject::connect(editor, &QLineEdit::editingFinished, p, [cc, ref, editor] {
            if (cc)
                cc->editBase([&](BaseFile& b) { b.setFormula(ref.name, editor->text()); });
        });
    }
    return p;
}

} // namespace

MenuPanel* openEditPropertyMenu(BasesController* c, const QString& propertyId)
{
    auto* panel = new MenuPanel(c->propertiesButton(), 300);
    panel->push(editPropertyPage(c, panel, propertyId, false));
    panel->popup();
    return panel;
}

MenuPanel* openPropertiesMenu(BasesController* c, QWidget* anchor)
{
    auto* panel = new MenuPanel(anchor, 280);
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 4);
    auto* search = new SearchInput(p);
    search->setPlaceholderText(tr("Find or create..."));
    l->addWidget(search);
    l->addSpacing(4);
    auto* list = new QWidget(p);
    auto* ll = new QVBoxLayout(list);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    l->addWidget(list);
    l->addWidget(divider(p));
    auto* actions = new QWidget(p);
    auto* al = new QVBoxLayout(actions);
    al->setContentsMargins(0, 4, 0, 0);
    al->setSpacing(0);
    l->addWidget(actions);
    auto render = std::make_shared<std::function<void()>>();
    *render = [cc, pp, search, ll, al, list, actions, render] {
        if (!cc)
            return;
        for (QVBoxLayout* layout : {ll, al})
            while (QLayoutItem* it = layout->takeAt(0)) {
                if (QWidget* w = it->widget())
                    w->deleteLater();
                delete it;
            }
        const BaseView* v = cc->currentView();
        const QStringList order = v ? v->columns() : QStringList();
        QStringList ids;
        for (const QString& id : cc->data().allProperties)
            if (id != u"file.file")
                ids << id;
        if (!search->text().isEmpty())
            for (const auto& [id, name] : propertyOptions(cc))
                if (!ids.contains(id) && id != u"file.file")
                    ids << id;
        for (const QString& id : order)
            if (!ids.contains(id))
                ids << id;
        QCollator col;
        col.setNumericMode(true);
        col.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(ids.begin(), ids.end(), [&](const QString& a, const QString& b) { return col.compare(a, b) < 0; });
        std::stable_sort(ids.begin(), ids.end(), [&](const QString& a, const QString& b) {
            const qsizetype ia = order.indexOf(a), ib = order.indexOf(b);
            return (ia < 0 ? 1e9 : double(ia)) < (ib < 0 ? 1e9 : double(ib));
        });
        const FuzzyQuery q(search->text());
        for (const QString& id : std::as_const(ids)) {
            const QString display = cc->base().displayName(id);
            const QString name = parsePropertyId(id).name;
            if (!search->text().isEmpty() && !q.match(display) && !q.match(name))
                continue;
            const PropertyRef ref = parsePropertyId(id);
            QString icon = ref.type == u"formula" ? QStringLiteral("square-function") : ref.type == u"file" ? QStringLiteral("info")
                                                                                                             : cc->propertyIcon(ref.name);
            auto* row = new MenuRow(icon, display, list);
            row->setObjectName(QStringLiteral("ocBasesProperty:") + id);
            const bool visible = order.contains(id);
            row->setCheckable(true);
            row->setChecked(visible);
            row->setFaint(!visible);
            row->setChevron(true);
            if (ref.type == u"formula") {
                const FormulaPtr f = cc->base().formula(ref.name);
                if (f && f->isInvalid())
                    row->setIconColor(basesStyle().error);
            }
            auto toggle = [cc, id, render] {
                if (!cc)
                    return;
                cc->editView([&](BaseView& view) {
                    QStringList o = view.columns();
                    if (o.contains(id))
                        o.removeAll(id);
                    else
                        o << id;
                    view.order = o;
                });
                QTimer::singleShot(0, cc, *render);
            };
            QObject::connect(row, &MenuRow::clicked, row, toggle);
            QObject::connect(row, &MenuRow::checkToggled, row, toggle);
            QObject::connect(row, &MenuRow::chevronClicked, row, [cc, pp, id] {
                if (cc && pp)
                    pp->push(editPropertyPage(cc, pp, id, false));
            });
            ll->addWidget(row);
        }
        const QString query = search->text().trimmed();
        if (!query.isEmpty()) {
            bool exists = false;
            if (PropertyTypeManager* t = cc->session() ? PropertyTypeManager::of(cc->session()) : nullptr)
                exists = t->allProperties().contains(query.toLower());
            if (!exists) {
                auto* addProp = new MenuRow(QStringLiteral("plus"), tr("Add \"%1\" property").arg(query), actions);
                QObject::connect(addProp, &MenuRow::clicked, addProp, [cc, pp, query] {
                    if (!cc)
                        return;
                    const QString id = makePropertyId(QStringLiteral("note"), query);
                    cc->editView([&](BaseView& view) {
                        QStringList o = view.columns();
                        if (!o.contains(id))
                            o << id;
                        view.order = o;
                    });
                    if (pp)
                        pp->push(editPropertyPage(cc, pp, id, true));
                });
                al->addWidget(addProp);
            }
        }
        auto* addFormula = new MenuRow(QStringLiteral("square-function"), tr("Add formula"), actions);
        addFormula->setObjectName(QStringLiteral("ocBasesAddFormula"));
        QObject::connect(addFormula, &MenuRow::clicked, addFormula, [cc, pp, query] {
            if (!cc)
                return;
            QString name = query.isEmpty() ? tr("Untitled") : query;
            const QStringList existing = cc->base().formulaNames();
            const QString base = name;
            for (int i = 2; existing.contains(name); ++i)
                name = base + u' ' + QString::number(i);
            const QString id = makePropertyId(QStringLiteral("formula"), name);
            cc->editBase([&](BaseFile& b) {
                b.setFormula(name, QString());
                if (BaseView* v = b.view(cc->viewName())) {
                    QStringList o = v->columns();
                    o << id;
                    v->order = o;
                }
            });
            if (pp)
                pp->push(editPropertyPage(cc, pp, id, true));
        });
        al->addWidget(addFormula);
        auto* hideAll = new MenuRow(QStringLiteral("eye-off"), tr("Hide all"), actions);
        QObject::connect(hideAll, &MenuRow::clicked, hideAll, [cc, render] {
            if (cc)
                cc->editView([](BaseView& view) { view.order = QStringList(); });
            QTimer::singleShot(0, cc, *render);
        });
        al->addWidget(hideAll);
        if (pp)
            QTimer::singleShot(0, pp, [pp] {
                if (pp)
                    pp->reposition();
            });
    };
    (*render)();
    QObject::connect(search, &QLineEdit::textChanged, panel, [render] { (*render)(); });
    panel->push(p);
    panel->popup();
    search->setFocus();
    return panel;
}

// ---------------------------------------------------------------------------------------------
// Summaries
// ---------------------------------------------------------------------------------------------
MenuPanel* openSummaryMenu(BasesController* c, const QString& propertyId, const QList<Value>& values, QWidget* anchor)
{
    auto* panel = new MenuPanel(anchor, 260);
    QPointer<BasesController> cc = c;
    QPointer<MenuPanel> pp = panel;
    QVBoxLayout* l = nullptr;
    QWidget* p = page(&l, 4);
    const std::optional<ValueType> type = firstValueType(values);
    QStringList names = BaseFile::builtinSummaries(type);
    for (const QString& n : c->base().customSummaryNames())
        if (!names.contains(n))
            names << n;
    QCollator col;
    col.setNumericMode(true);
    col.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&](const QString& a, const QString& b) { return col.compare(a, b) < 0; });
    const QString current = c->currentView() ? c->currentView()->summaryOf(propertyId) : QString();
    for (const QString& n : std::as_const(names)) {
        const Value r = summaryValue(c->base(), c->runner()->dataSource(), values, n);
        auto* row = new MenuRow(r.isNull() ? QStringLiteral("square-function") : valueIcon(r), n, p);
        row->setObjectName(QStringLiteral("ocBasesSummary:") + n);
        row->setActive(n == current);
        if (!r.isNull())
            row->setValueText(r.isError() ? tr("Error") : r.type() == ValueType::Number ? numberText(r.number()) : r.toString());
        QObject::connect(row, &MenuRow::clicked, row, [cc, pp, propertyId, n] {
            if (cc)
                cc->editView([&](BaseView& v) { v.setSummary(propertyId, n); });
            if (pp)
                pp->close();
        });
        l->addWidget(row);
    }
    l->addWidget(divider(p));
    if (!current.isEmpty()) {
        auto* hide = new MenuRow(QStringLiteral("trash-2"), tr("Hide summary"), p);
        QObject::connect(hide, &MenuRow::clicked, hide, [cc, pp, propertyId] {
            if (cc)
                cc->editView([&](BaseView& v) { v.setSummary(propertyId, QString()); });
            if (pp)
                pp->close();
        });
        l->addWidget(hide);
    }
    panel->push(p);
    panel->popup();
    return panel;
}

} // namespace oc::basesui
