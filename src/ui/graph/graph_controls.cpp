#include "ui/graph/graph_controls.h"

#include "render/theme/theme.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/widgets/value_slider.h"

#include <QAbstractButton>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <cmath>

namespace oc {

namespace {

// .graph-controls .setting-item: padding 6px 0 + margin-bottom 4px.
constexpr int kItemPad = 6;
constexpr int kItemGap = 4;
// .graph-control-section: padding 6px 12px.
constexpr int kSectionPadX = 12;
constexpr int kSectionPadY = 6;
// .graph-controls.is-close: padding 6px (--size-2-3) inside a 1px transparent border.
constexpr int kClosedPad = 6;
constexpr int kBorder = 1;
constexpr int kSectionCount = 4;

QFont uiFont(double px, int weight = QFont::Normal)
{
    QFont f = currentTheme()->interfaceFont(px);
    f.setWeight(QFont::Weight(weight));
    return f;
}

QColor token(const char* name, const QColor& fallback = Qt::gray)
{
    return currentTheme()->color(QLatin1String(name), fallback);
}

// .setting-item-name: 13px, --text-normal, line-height 1.3.
class SettingName : public QLabel {
public:
    SettingName(const QString& text, QWidget* parent)
        : QLabel(text, parent)
    {
        restyle();
        if (auto* tm = ThemeManager::instance())
            QObject::connect(tm, &ThemeManager::themeChanged, this, [this] { restyle(); });
    }
    void restyle()
    {
        setFont(uiFont(currentTheme()->px(QStringLiteral("--font-ui-small"), -1, 13)));
        QPalette pal = palette();
        pal.setColor(QPalette::WindowText, token("--text-normal"));
        setPalette(pal);
        setMinimumHeight(int(std::ceil(font().pixelSize() * 1.3)));
    }
};

// A colour-group swatch (input[type=color] in .graph-color-group: 18 px round swatch).
class ColorSwatch : public QAbstractButton {
public:
    explicit ColorSwatch(QWidget* parent)
        : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setToolTip(QObject::tr("Click to change color\nDrag to reorder groups"));
    }
    void setColor(const QColor& c)
    {
        m_color = c;
        update();
    }
    QColor color() const { return m_color; }
    QSize sizeHint() const override { return {22, 18}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(QPointF((width() - 18) / 2.0, (height() - 18) / 2.0), QSizeF(18, 18));
        p.setPen(Qt::NoPen);
        p.setBrush(m_color);
        p.drawEllipse(r);
        // --swatch-shadow: inset 0 0 0 1px mono-100 at 15%.
        QColor ring = token("--text-normal", Qt::white);
        ring.setAlphaF(0.15f);
        p.setPen(QPen(ring, 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(r.adjusted(0.5, 0.5, -0.5, -0.5));
        if (hasFocus()) {
            p.setPen(QPen(token("--background-modifier-border-focus"), 2));
            p.drawEllipse(r.adjusted(-1, -1, 1, 1));
        }
    }

private:
    QColor m_color;
};

QWidget* settingRow(QWidget* parent, bool column, QBoxLayout** layout)
{
    auto* row = new QWidget(parent);
    QBoxLayout* l = column ? static_cast<QBoxLayout*>(new QVBoxLayout(row)) : static_cast<QBoxLayout*>(new QHBoxLayout(row));
    l->setContentsMargins(0, kItemPad, 0, kItemPad + kItemGap);
    l->setSpacing(column ? 0 : 8);
    *layout = l;
    return row;
}

} // namespace

namespace graph_controls_detail {

// The clickable header of a section (".tree-item-self.mod-collapsible"): collapse chevron in the
// icon gutter, 13px medium title.
class SectionHeader : public QWidget {
public:
    SectionHeader(const QString& title, QWidget* parent)
        : QWidget(parent)
        , m_title(title)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(25); // 4 + 13·1.3 + 4
    }
    void setCollapsed(bool c)
    {
        m_collapsed = c;
        update();
    }
    std::function<void()> onClick;

protected:
    void enterEvent(QEnterEvent*) override
    {
        m_hover = true;
        update();
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = false;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && onClick)
            onClick();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(token("--nav-item-background-hover", QColor(255, 255, 255, 17)));
            p.drawRoundedRect(QRectF(rect()), 4, 4);
        }
        // Chevron (Obsidian's right-triangle, 10 px, stroke 4/24), centred in a 16 px gutter that
        // starts 20 px before the title (padding-inline-start 16px → gutter at -4 .. 12).
        const QPointF c(4, height() / 2.0);
        p.save();
        p.translate(c);
        if (m_collapsed)
            p.rotate(-90);
        QPen pen(token("--nav-collapse-icon-color", token("--text-faint")), 10.0 * 4 / 24);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        const double k = 10.0 / 24;
        const QPointF pts[3] = {QPointF(-9 * k, -4.5 * k), QPointF(0, 4.5 * k), QPointF(9 * k, -4.5 * k)};
        p.drawPolyline(pts, 3);
        p.restore();
        p.setFont(uiFont(currentTheme()->px(QStringLiteral("--font-ui-small"), -1, 13), 500));
        p.setPen(token(m_hover ? "--nav-item-color-hover" : "--text-normal"));
        p.drawText(QRectF(16, 0, width() - 16 - 8, height()), Qt::AlignLeft | Qt::AlignVCenter, m_title);
    }

private:
    QString m_title;
    bool m_collapsed = true;
    bool m_hover = false;
};

// ".graph-control-section": header + children, with a bottom border except for the last one.
class Section : public QWidget {
public:
    Section(const QString& title, QWidget* parent)
        : QWidget(parent)
    {
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(kSectionPadX, kSectionPadY, kSectionPadX, kSectionPadY);
        l->setSpacing(2); // --nav-item-margin-bottom
        header = new SectionHeader(title, this);
        l->addWidget(header);
        body = new QWidget(this);
        bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 4, 0, 4);
        bodyLayout->setSpacing(0);
        l->addWidget(body);
    }
    void setCollapsed(bool c)
    {
        collapsed = c;
        header->setCollapsed(c);
        body->setVisible(!c);
        updateGeometry();
    }
    void setLast(bool last)
    {
        m_last = last;
        bodyLayout->setContentsMargins(0, 4, 0, last ? 16 : 4);
        update();
    }

    SectionHeader* header;
    QWidget* body;
    QVBoxLayout* bodyLayout;
    bool collapsed = true;

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_last)
            return;
        QPainter p(this);
        p.fillRect(QRect(0, height() - 1, width(), 1), token("--background-modifier-border"));
    }

private:
    bool m_last = false;
};

// ".graph-color-group": query input, colour swatch, delete button.
class GroupRow : public QWidget {
public:
    GroupRow(GraphControls* owner, const graph::ColorGroupSetting& g, QWidget* parent)
        : QWidget(parent)
    {
        auto* l = new QHBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 6);
        l->setSpacing(0);
        query = new QLineEdit(this);
        query->setPlaceholderText(QObject::tr("Enter query..."));
        query->setText(g.query);
        query->setFont(uiFont(13));
        l->addWidget(query, 1);
        l->addSpacing(6);
        swatch = new ColorSwatch(this);
        swatch->setColor(QColor::fromRgb(g.color.rgb));
        l->addWidget(swatch);
        l->addSpacing(2);
        remove = new IconButton(QStringLiteral("x"), QObject::tr("Delete group"), this);
        l->addWidget(remove);
        QPointer<GraphControls> o = owner;
        QObject::connect(query, &QLineEdit::textEdited, this, [this, o] {
            if (o)
                o->setGroupQuery(o->m_groupRows.indexOf(this), query->text());
        });
        QObject::connect(swatch, &QAbstractButton::clicked, this, [this, o] {
            auto* dlg = new QColorDialog(swatch->color(), this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            QObject::connect(dlg, &QColorDialog::colorSelected, this, [this, o](const QColor& c) {
                if (o)
                    o->setGroupColor(o->m_groupRows.indexOf(this), c);
            });
            dlg->open();
        });
        QObject::connect(remove, &QAbstractButton::clicked, this, [this, o] {
            if (o)
                o->removeGroup(o->m_groupRows.indexOf(this));
        });
    }
    QLineEdit* query;
    ColorSwatch* swatch;
    IconButton* remove;
};

} // namespace graph_controls_detail

using namespace graph_controls_detail;

// ---------------------------------------------------------------------------------------------
// GraphControls
// ---------------------------------------------------------------------------------------------
GraphControls::GraphControls(Variant variant, QWidget* parent)
    : QWidget(parent)
    , m_variant(variant)
{
    setAttribute(Qt::WA_NoMousePropagation); // clicks on the panel never reach the graph
    build();
    syncWidgets();
    updateLayout();
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

GraphControls::~GraphControls() = default;

double GraphControls::panelRadius() const { return currentTheme()->px(QStringLiteral("--menu-radius"), -1, 8); }

void GraphControls::build()
{
    const bool local = m_variant == Variant::Local;

    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // --scrollbar-native-width: none
    m_scroll->setWidgetResizable(true);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    m_content = new QWidget;
    m_content->setAutoFillBackground(false);
    auto* contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    m_scroll->setWidget(m_content);

    auto addSection = [&](const QString& title) {
        auto* s = new Section(title, m_content);
        const int index = int(m_sections.size());
        s->header->onClick = [this, index] { setSectionCollapsed(index, !m_sections[index]->collapsed); };
        contentLayout->addWidget(s);
        m_sections << s;
        return s;
    };
    auto addToggle = [&](Section* s, const QString& key, const QString& name, const QString& tip) {
        QBoxLayout* l;
        QWidget* row = settingRow(s->body, false, &l);
        row->setToolTip(tip);
        l->addWidget(new SettingName(name, row), 1, Qt::AlignVCenter);
        auto* t = new ToggleSwitch(row);
        l->addWidget(t, 0, Qt::AlignVCenter);
        s->bodyLayout->addWidget(row);
        m_toggles.insert(key, t);
        connect(t, &QAbstractButton::toggled, this, [this, key](bool on) {
            if (m_syncing)
                return;
            graph::GraphSettings& st = m_settings;
            Changes change = Change::Filters;
            if (key == u"showTags")
                st.showTags = on;
            else if (key == u"showAttachments")
                st.showAttachments = on;
            else if (key == u"hideUnresolved")
                st.hideUnresolved = on;
            else if (key == u"showOrphans")
                st.showOrphans = on;
            else if (key == u"localBacklinks")
                st.localBacklinks = on;
            else if (key == u"localForelinks")
                st.localForelinks = on;
            else if (key == u"localInterlinks")
                st.localInterlinks = on;
            else if (key == u"showArrow") {
                st.showArrow = on;
                change = Change::Display;
            }
            emitEdited(change);
        });
    };
    auto addSlider = [&](Section* s, const QString& key, const QString& name, double lo, double hi, double step, const QString& tip = {}) {
        QBoxLayout* l;
        QWidget* row = settingRow(s->body, true, &l);
        row->setToolTip(tip);
        l->addWidget(new SettingName(name, row));
        l->addSpacing(12 + 12); // .setting-item row-gap + .mod-slider .setting-item-control padding-top
        auto* v = new ValueSlider(row);
        v->setRange(lo, hi);
        v->setStep(step);
        l->addWidget(v);
        s->bodyLayout->addWidget(row);
        m_sliders.insert(key, v);
        connect(v, &ValueSlider::valueChanged, this, [this, key](double value) {
            if (m_syncing)
                return;
            graph::GraphSettings& st = m_settings;
            Changes change = Change::Forces;
            if (key == u"localJumps") {
                st.localJumps = int(std::lround(value));
                change = Change::Filters;
            } else if (key == u"textFadeMultiplier" || key == u"nodeSizeMultiplier" || key == u"lineSizeMultiplier") {
                change = Change::Display;
                (key == u"textFadeMultiplier" ? st.textFadeMultiplier : key == u"nodeSizeMultiplier" ? st.nodeSizeMultiplier : st.lineSizeMultiplier) = value;
            } else if (key == u"centerStrength") {
                st.centerStrength = value;
            } else if (key == u"repelStrength") {
                st.repelStrength = value;
            } else if (key == u"linkStrength") {
                st.linkStrength = value;
            } else if (key == u"linkDistance") {
                st.linkDistance = value;
            }
            emitEdited(change);
        });
    };

    // ---- Filters ----
    Section* filters = addSection(tr("Filters"));
    {
        QBoxLayout* l;
        m_searchRow = settingRow(filters->body, false, &l);
        m_search = new SearchInput(m_searchRow);
        m_search->setPlaceholderText(tr("Search files..."));
        m_search->setFont(uiFont(13));
        l->addWidget(m_search, 1);
        filters->bodyLayout->addWidget(m_searchRow);
        connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (m_syncing)
                return;
            m_settings.search = text;
            emitEdited(Change::Search);
        });
        connect(m_search, &QLineEdit::returnPressed, this, [this] { emitEdited(Change::SearchSubmitted); });
    }
    if (local) {
        addSlider(filters, QStringLiteral("localJumps"), tr("Depth"), 1, 5, 1, tr("Show nodes this number of links away"));
        addToggle(filters, QStringLiteral("localBacklinks"), tr("Incoming links"), tr("Show links from other files"));
        addToggle(filters, QStringLiteral("localForelinks"), tr("Outgoing links"), tr("Show links to other files"));
        addToggle(filters, QStringLiteral("localInterlinks"), tr("Neighbor links"), tr("Show links between neighbors"));
    }
    addToggle(filters, QStringLiteral("showTags"), tr("Tags"), tr("Tags are linked to the files that contain them"));
    addToggle(filters, QStringLiteral("showAttachments"), tr("Attachments"), tr("Show attachments"));
    addToggle(filters, QStringLiteral("hideUnresolved"), tr("Existing files only"), tr("When enabled, links to nonexistent files are not shown"));
    if (!local)
        addToggle(filters, QStringLiteral("showOrphans"), tr("Orphans"), tr("Show files that are not linked to any other file"));

    // ---- Groups ----
    Section* groups = addSection(tr("Groups"));
    m_groupsContainer = new QWidget(groups->body);
    m_groupsLayout = new QVBoxLayout(m_groupsContainer);
    m_groupsLayout->setContentsMargins(0, 0, 0, 0);
    m_groupsLayout->setSpacing(0);
    groups->bodyLayout->addWidget(m_groupsContainer);
    {
        auto* box = new QWidget(groups->body);
        auto* l = new QVBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 10); // .graph-color-button-container margin-bottom
        m_newGroup = new QPushButton(tr("New group"), box);
        m_newGroup->setFont(uiFont(13));
        m_newGroup->setMinimumHeight(30);
        l->addWidget(m_newGroup);
        groups->bodyLayout->addWidget(box);
        connect(m_newGroup, &QPushButton::clicked, this, [this] {
            // Obsidian: hsl(40·n, dark ? 70% : 60%, 60%).
            const int n = int(m_settings.colorGroups.size());
            const bool dark = currentTheme()->isDark();
            const QColor c = QColor::fromHslF(float(std::fmod(40.0 * n, 360.0) / 360.0), dark ? 0.7f : 0.6f, 0.6f);
            graph::ColorGroupSetting g;
            g.color.rgb = c.rgb() & 0xFFFFFFu;
            g.color.a = 1;
            m_settings.colorGroups.append(g);
            addGroupRow(g);
            m_groupRows.last()->query->setFocus();
            emitEdited(Change::Groups);
            updateLayout();
        });
    }

    // ---- Display ----
    Section* display = addSection(tr("Display"));
    addToggle(display, QStringLiteral("showArrow"), tr("Arrows"), tr("Show arrows when zoomed in"));
    addSlider(display, QStringLiteral("textFadeMultiplier"), tr("Text fade threshold"), -3, 3, 0.1);
    addSlider(display, QStringLiteral("nodeSizeMultiplier"), tr("Node size"), 0.1, 5, 0);
    addSlider(display, QStringLiteral("lineSizeMultiplier"), tr("Link thickness"), 0.1, 5, 0);
    if (!local) {
        QBoxLayout* l;
        QWidget* row = settingRow(display->body, false, &l);
        m_animateText = new QPushButton(tr("Animate"), row);
        m_animateText->setFont(uiFont(13));
        m_animateText->setMinimumHeight(30);
        l->addWidget(m_animateText, 1);
        display->bodyLayout->addWidget(row);
        connect(m_animateText, &QPushButton::clicked, this, &GraphControls::animateRequested);
    }

    // ---- Forces ----
    Section* forces = addSection(tr("Forces"));
    addSlider(forces, QStringLiteral("centerStrength"), tr("Center force"), 0, 1, 0);
    addSlider(forces, QStringLiteral("repelStrength"), tr("Repel force"), 0, 20, 0);
    addSlider(forces, QStringLiteral("linkStrength"), tr("Link force"), 0, 1, 0);
    addSlider(forces, QStringLiteral("linkDistance"), tr("Link distance"), 30, 500, 1);
    m_sections.last()->setLast(true);
    contentLayout->addStretch(1);

    // ---- buttons ----
    m_resetBtn = new IconButton(QStringLiteral("rotate-ccw"), tr("Restore default settings"), this);
    m_closeBtn = new IconButton(QStringLiteral("x"), tr("Close"), this);
    m_openBtn = new IconButton(QStringLiteral("settings"), tr("Open graph settings"), this);
    m_openBtn->setButtonSize(QSize(30, 26)); // .clickable-icon padding 4px 6px
    connect(m_resetBtn, &QAbstractButton::clicked, this, &GraphControls::restoreDefaults);
    connect(m_closeBtn, &QAbstractButton::clicked, this, [this] { setClosed(true); });
    connect(m_openBtn, &QAbstractButton::clicked, this, [this] { setClosed(false); });
    if (!local) {
        m_animateBtn = new IconButton(QStringLiteral("wand-2"), tr("Start timelapse animation"), this);
        m_animateBtn->setButtonSize(QSize(30, 26));
        connect(m_animateBtn, &QAbstractButton::clicked, this, &GraphControls::animateRequested);
    }
}

void GraphControls::addGroupRow(const graph::ColorGroupSetting& group)
{
    auto* row = new GroupRow(this, group, m_groupsContainer);
    m_groupsLayout->addWidget(row);
    m_groupRows << row;
}

void GraphControls::rebuildGroups()
{
    for (GroupRow* r : std::as_const(m_groupRows))
        delete r;
    m_groupRows.clear();
    for (const graph::ColorGroupSetting& g : std::as_const(m_settings.colorGroups))
        addGroupRow(g);
}

void GraphControls::setGroupQuery(int row, const QString& query)
{
    if (row < 0 || row >= m_settings.colorGroups.size())
        return;
    m_settings.colorGroups[row].query = query;
    if (m_groupRows[row]->query->text() != query)
        m_groupRows[row]->query->setText(query);
    emitEdited(Change::GroupQuery);
}

void GraphControls::removeGroup(int row)
{
    if (row < 0 || row >= m_settings.colorGroups.size())
        return;
    m_settings.colorGroups.removeAt(row);
    GroupRow* r = m_groupRows.takeAt(row);
    r->hide();
    r->deleteLater();
    emitEdited(Change::Groups);
    updateLayout();
}

void GraphControls::setGroupColor(int row, const QColor& color)
{
    if (row < 0 || row >= m_settings.colorGroups.size() || !color.isValid())
        return;
    m_settings.colorGroups[row].color.rgb = color.rgb() & 0xFFFFFFu;
    m_settings.colorGroups[row].color.a = 1;
    m_groupRows[row]->swatch->setColor(color);
    emitEdited(Change::Groups);
}

ToggleSwitch* GraphControls::toggle(const QString& key) const { return m_toggles.value(key); }
ValueSlider* GraphControls::slider(const QString& key) const { return m_sliders.value(key); }

void GraphControls::setSettings(const graph::GraphSettings& settings)
{
    const bool groupsChanged = settings.colorGroups != m_settings.colorGroups;
    m_settings = settings;
    if (groupsChanged)
        rebuildGroups();
    syncWidgets();
    updateLayout();
}

void GraphControls::syncWidgets()
{
    m_syncing = true;
    const graph::GraphSettings& s = m_settings;
    if (m_search->text() != s.search)
        m_search->setText(s.search);
    auto setToggle = [&](const char* key, bool on) {
        if (ToggleSwitch* t = m_toggles.value(QLatin1String(key)))
            t->setChecked(on);
    };
    setToggle("showTags", s.showTags);
    setToggle("showAttachments", s.showAttachments);
    setToggle("hideUnresolved", s.hideUnresolved);
    setToggle("showOrphans", s.showOrphans);
    setToggle("localBacklinks", s.localBacklinks);
    setToggle("localForelinks", s.localForelinks);
    setToggle("localInterlinks", s.localInterlinks);
    setToggle("showArrow", s.showArrow);
    auto setSlider = [&](const char* key, double v) {
        if (ValueSlider* sl = m_sliders.value(QLatin1String(key)))
            sl->setValue(v);
    };
    setSlider("localJumps", s.localJumps);
    setSlider("textFadeMultiplier", s.textFadeMultiplier);
    setSlider("nodeSizeMultiplier", s.nodeSizeMultiplier);
    setSlider("lineSizeMultiplier", s.lineSizeMultiplier);
    setSlider("centerStrength", s.centerStrength);
    setSlider("repelStrength", s.repelStrength);
    setSlider("linkStrength", s.linkStrength);
    setSlider("linkDistance", s.linkDistance);
    for (int i = 0; i < m_groupRows.size() && i < s.colorGroups.size(); ++i) {
        if (m_groupRows[i]->query->text() != s.colorGroups[i].query)
            m_groupRows[i]->query->setText(s.colorGroups[i].query);
        m_groupRows[i]->swatch->setColor(QColor::fromRgb(s.colorGroups[i].color.rgb));
    }
    const bool collapsed[kSectionCount] = {s.collapseFilter, s.collapseColorGroups, s.collapseDisplay, s.collapseForces};
    for (int i = 0; i < kSectionCount && i < m_sections.size(); ++i)
        m_sections[i]->setCollapsed(collapsed[i]);
    m_syncing = false;
}

void GraphControls::setClosed(bool closed)
{
    if (m_settings.close == closed)
        return;
    m_settings.close = closed;
    updateLayout();
    emitEdited(Change::Layout);
}

void GraphControls::setSectionCollapsed(int section, bool collapsed)
{
    if (section < 0 || section >= m_sections.size())
        return;
    bool* flags[kSectionCount] = {&m_settings.collapseFilter, &m_settings.collapseColorGroups, &m_settings.collapseDisplay,
                                  &m_settings.collapseForces};
    *flags[section] = collapsed;
    m_sections[section]->setCollapsed(collapsed);
    updateLayout();
    emitEdited(Change::Layout);
}

void GraphControls::focusSearch()
{
    setClosed(false);
    setSectionCollapsed(0, false);
    m_search->setFocus();
    m_search->selectAll();
}

void GraphControls::setSearching(bool searching)
{
    m_searchRow->setProperty("ocLoading", searching);
    m_searchRow->update();
}

void GraphControls::setInSidebar(bool inSidebar)
{
    m_inSidebar = inSidebar;
    update();
}

void GraphControls::restoreDefaults()
{
    m_settings.restoreDefaults();
    rebuildGroups();
    syncWidgets();
    updateLayout();
    emitEdited(Changes(Change::Filters) | Change::Groups | Change::Display | Change::Forces);
}

void GraphControls::emitEdited(Changes changes)
{
    if (!m_syncing)
        emit edited(changes);
}

QSize GraphControls::sizeHint() const
{
    if (m_settings.close) {
        const int w = 2 * kBorder + 2 * kClosedPad + m_openBtn->sizeHint().width();
        int h = 2 * kBorder + 2 * kClosedPad + m_openBtn->sizeHint().height();
        if (m_animateBtn)
            h += 8 + m_animateBtn->sizeHint().height(); // .mod-animate margin-top 8px
        return {w, h};
    }
    const int w = int(std::lround(currentTheme()->px(QStringLiteral("--graph-controls-width"), -1, 240)));
    // Layouts cache their items' hints until the (posted) LayoutRequest: sections may have been
    // expanded or groups added since.
    for (Section* s : m_sections) {
        s->body->updateGeometry();
        s->updateGeometry();
    }
    return {w, m_content->layout()->sizeHint().height() + 2 * kBorder};
}

void GraphControls::updateLayout()
{
    const bool closed = m_settings.close;
    m_scroll->setVisible(!closed);
    m_closeBtn->setVisible(!closed);
    m_resetBtn->setVisible(!closed);
    m_openBtn->setVisible(closed);
    if (m_animateBtn)
        m_animateBtn->setVisible(closed);
    updateGeometry();
    resizeEvent(nullptr);
    update();
    emit sizeChanged();
}

void GraphControls::resizeEvent(QResizeEvent*)
{
    m_scroll->setGeometry(rect().adjusted(kBorder, kBorder, -kBorder, -kBorder));
    // .graph-controls-button.mod-close: top 6px, right 8px; .mod-reset: right 36px.
    m_closeBtn->setGeometry(width() - 8 - m_closeBtn->sizeHint().width(), 6, m_closeBtn->sizeHint().width(), m_closeBtn->sizeHint().height());
    m_resetBtn->setGeometry(width() - 36 - m_resetBtn->sizeHint().width(), 6, m_resetBtn->sizeHint().width(), m_resetBtn->sizeHint().height());
    m_closeBtn->raise();
    m_resetBtn->raise();
    const int x = kBorder + kClosedPad;
    m_openBtn->setGeometry(QRect(QPoint(x, x), m_openBtn->sizeHint()));
    if (m_animateBtn)
        m_animateBtn->setGeometry(QRect(QPoint(x, x + m_openBtn->sizeHint().height() + 8), m_animateBtn->sizeHint()));
}

void GraphControls::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double r = panelRadius();
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_settings.close) {
        p.setPen(Qt::NoPen);
        p.setBrush(token(m_inSidebar ? "--background-secondary" : "--background-primary"));
        p.drawRoundedRect(box, r, r);
        return;
    }
    p.setPen(QPen(token("--menu-border-color", token("--background-modifier-border-hover")), kBorder));
    p.setBrush(token("--menu-background", token("--background-secondary")));
    p.drawRoundedRect(box, r, r);
}

} // namespace oc
