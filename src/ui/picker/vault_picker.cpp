#include "ui/picker/vault_picker.h"

#include "core/config/vault_registry.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

namespace oc {

namespace {
enum Roles { PathRole = Qt::UserRole + 1, IdRole, ExternalRole };

class VaultItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& idx) const override
    {
        return idx.data(ExternalRole).toInt() == 2 ? QSize(100, 30) : QSize(100, 52);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const ThemePtr t = currentTheme();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect.adjusted(6, 2, -6, -2);
        const int kind = idx.data(ExternalRole).toInt();
        if (kind == 2) { // section header
            QFont f = t->interfaceFont(12);
            f.setWeight(QFont::DemiBold);
            p->setFont(f);
            p->setPen(t->color(QStringLiteral("--text-faint")));
            p->drawText(r.adjusted(8, 0, 0, 0), Qt::AlignLeft | Qt::AlignBottom, idx.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }
        if (opt.state & (QStyle::State_Selected | QStyle::State_MouseOver)) {
            QPainterPath path;
            path.addRoundedRect(r, 6, 6);
            p->fillPath(path, t->color(opt.state & QStyle::State_Selected ? QStringLiteral("--background-modifier-active-hover")
                                                                           : QStringLiteral("--background-modifier-hover")));
        }
        QFont nf = t->interfaceFont(14);
        nf.setWeight(QFont::DemiBold);
        p->setFont(nf);
        p->setPen(t->color(QStringLiteral("--text-normal")));
        p->drawText(r.adjusted(12, 6, -8, -r.height() / 2), Qt::AlignLeft | Qt::AlignVCenter, idx.data(Qt::DisplayRole).toString());
        p->setFont(t->interfaceFont(12));
        p->setPen(t->color(QStringLiteral("--text-muted")));
        const QString path = QFontMetrics(p->font()).elidedText(idx.data(PathRole).toString(), Qt::ElideMiddle, r.width() - 24);
        p->drawText(r.adjusted(12, r.height() / 2, -8, -6), Qt::AlignLeft | Qt::AlignVCenter, path);
        p->restore();
    }
};
} // namespace

VaultPicker::VaultPicker(VaultRegistry* registry, QWidget* parent)
    : QWidget(parent)
    , m_registry(registry)
{
    setWindowTitle(QStringLiteral("Obsidian C++"));
    setFixedSize(800, 650);
    const ThemePtr t = currentTheme();

    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Left: recent vaults
    auto* left = new QWidget(this);
    left->setFixedWidth(280);
    left->setProperty("ocBg", QStringLiteral("secondary"));
    left->setAttribute(Qt::WA_StyledBackground);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(8, 12, 8, 12);
    m_list = new QListWidget(left);
    m_list->setItemDelegate(new VaultItemDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    lv->addWidget(m_list);
    h->addWidget(left);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        if (it->data(ExternalRole).toInt() != 2)
            emit openVault(it->data(PathRole).toString());
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (it->data(ExternalRole).toInt() != 2)
            emit openVault(it->data(PathRole).toString());
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, &VaultPicker::showItemMenu);

    // Right: branding + actions
    auto* right = new QWidget(this);
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(40, 40, 40, 30);
    rv->setSpacing(10);
    auto* logo = new QLabel(right);
    logo->setPixmap(IconProvider::instance().pixmap(QStringLiteral("obsidian-cpp-logo"), 72, t->color(QStringLiteral("--interactive-accent")), 1.5,
                                                   devicePixelRatioF()));
    logo->setAlignment(Qt::AlignCenter);
    rv->addWidget(logo);
    auto* name = new QLabel(QStringLiteral("Obsidian C++"), right);
    QFont nf = t->interfaceFont(26);
    nf.setWeight(QFont::Bold);
    name->setFont(nf);
    name->setAlignment(Qt::AlignCenter);
    rv->addWidget(name);
    auto* ver = new QLabel(tr("Version %1").arg(QCoreApplication::applicationVersion()), right);
    ver->setAlignment(Qt::AlignCenter);
    ver->setProperty("ocText", QStringLiteral("muted"));
    rv->addWidget(ver);
    rv->addSpacing(28);

    auto addOption = [&](const QString& title, const QString& desc, QWidget* extra, QPushButton* button) {
        auto* row = new QWidget(right);
        auto* rh = new QHBoxLayout(row);
        rh->setContentsMargins(0, 8, 0, 8);
        auto* texts = new QVBoxLayout;
        auto* tl = new QLabel(title, row);
        QFont tf = t->interfaceFont(15);
        tf.setWeight(QFont::DemiBold);
        tl->setFont(tf);
        auto* dl = new QLabel(desc, row);
        dl->setWordWrap(true);
        dl->setProperty("ocText", QStringLiteral("muted"));
        texts->addWidget(tl);
        texts->addWidget(dl);
        if (extra)
            texts->addWidget(extra);
        rh->addLayout(texts, 1);
        rh->addWidget(button, 0, Qt::AlignVCenter);
        rv->addWidget(row);
        auto* line = new QFrame(right);
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: %1;").arg(t->color(QStringLiteral("--background-modifier-border")).name()));
        rv->addWidget(line);
    };

    auto* createBox = new QWidget(right);
    auto* cv = new QVBoxLayout(createBox);
    cv->setContentsMargins(0, 6, 0, 0);
    m_newName = new QLineEdit(createBox);
    m_newName->setPlaceholderText(tr("Vault name"));
    auto* locRow = new QHBoxLayout;
    m_newLocation = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), createBox);
    auto* browse = new QPushButton(tr("Browse"), createBox);
    locRow->addWidget(m_newLocation, 1);
    locRow->addWidget(browse);
    cv->addWidget(m_newName);
    cv->addLayout(locRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose location"), m_newLocation->text());
        if (!dir.isEmpty())
            m_newLocation->setText(dir);
    });
    auto* createBtn = new QPushButton(tr("Create"), right);
    createBtn->setProperty("cta", true);
    connect(createBtn, &QPushButton::clicked, this, &VaultPicker::createVault);
    addOption(tr("Create new vault"), tr("Create a new vault under a folder."), createBox, createBtn);

    auto* openBtn = new QPushButton(tr("Open"), right);
    connect(openBtn, &QPushButton::clicked, this, &VaultPicker::openFolder);
    addOption(tr("Open folder as vault"), tr("Choose an existing folder of Markdown files."), nullptr, openBtn);

    rv->addStretch(1);
    h->addWidget(right, 1);

    connect(m_registry, &VaultRegistry::changed, this, &VaultPicker::rebuildList);
    rebuildList();
}

void VaultPicker::rebuildList()
{
    m_list->clear();
    auto addHeader = [&](const QString& text) {
        auto* it = new QListWidgetItem(text, m_list);
        it->setData(ExternalRole, 2);
        it->setFlags(Qt::NoItemFlags);
    };
    const QList<VaultEntry> own = m_registry->vaults();
    for (const VaultEntry& e : own) {
        auto* it = new QListWidgetItem(e.name(), m_list);
        it->setData(PathRole, e.path);
        it->setData(IdRole, e.id);
        it->setData(ExternalRole, 0);
        it->setToolTip(e.path);
    }
    // Suggestions from the real Obsidian's registry (read-only).
    QList<VaultEntry> ext;
    for (const VaultEntry& e : VaultRegistry::obsidianVaults())
        if (!m_registry->byPath(e.path))
            ext.push_back(e);
    if (!ext.isEmpty()) {
        addHeader(tr("From Obsidian"));
        for (const VaultEntry& e : ext) {
            auto* it = new QListWidgetItem(e.name(), m_list);
            it->setData(PathRole, e.path);
            it->setData(ExternalRole, 1);
            it->setToolTip(e.path);
        }
    }
}

void VaultPicker::createVault()
{
    const QString name = m_newName->text().trimmed();
    if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
        m_newName->setFocus();
        return;
    }
    const QString path = QDir(m_newLocation->text()).filePath(name);
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("Create vault"), tr("A folder named \"%1\" already exists there.").arg(name));
        return;
    }
    if (!QDir().mkpath(path)) {
        QMessageBox::warning(this, tr("Create vault"), tr("Couldn't create the folder."));
        return;
    }
    emit openVault(path);
}

void VaultPicker::openFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open folder as vault"), QDir::homePath());
    if (!dir.isEmpty())
        emit openVault(dir);
}

void VaultPicker::showItemMenu(const QPoint& pos)
{
    QListWidgetItem* it = m_list->itemAt(pos);
    if (!it || it->data(ExternalRole).toInt() == 2)
        return;
    const QString path = it->data(PathRole).toString();
    QMenu menu(this);
    menu.addAction(tr("Reveal in file manager"), this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    if (it->data(ExternalRole).toInt() == 0) {
        const QString id = it->data(IdRole).toString();
        menu.addAction(tr("Remove from list"), this, [this, id] {
            m_registry->remove(id);
            m_registry->save();
        });
    }
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void VaultPicker::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-primary")));
}

} // namespace oc
