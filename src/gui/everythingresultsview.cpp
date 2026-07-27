#include "everythingresultsview.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QLineEdit>
#include <QLabel>
#include <QHeaderView>
#include <QDir>
#include <QMenu>
#include <QAction>
#include <QGuiApplication>
#include <QClipboard>

#include "base/preferences.h"
#include "base/utils/misc.h"
#include "base/path.h"
#include "gui/utils.h"

namespace
{
    class EverythingTreeItem final : public QTreeWidgetItem
    {
    public:
        explicit EverythingTreeItem(const EverythingItem &item)
            : m_size(item.size)
            , m_dateModified(item.dateModified)
            , m_isFolder(item.isFolder)
        {
            setText(0, item.name);
            setText(1, item.path);
            setText(2, (item.size > 0) ? Utils::Misc::friendlyUnit(item.size) : QString{});
            setText(3, item.dateModified.isValid() ? item.dateModified.toString(QStringLiteral("yyyy/MM/dd hh:mm")) : QString{});

            const QString fullPath = item.path.isEmpty() ? item.name : QDir::toNativeSeparators(QDir(item.path).filePath(item.name));
            setData(0, Qt::UserRole, fullPath);
        }

        bool operator<(const QTreeWidgetItem &other) const override
        {
            const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
            const auto *otherItem = dynamic_cast<const EverythingTreeItem *>(&other);

            if (col == 2) // Size column
            {
                if (otherItem && (m_isFolder != otherItem->m_isFolder))
                {
                    const Qt::SortOrder order = treeWidget() ? treeWidget()->header()->sortIndicatorOrder() : Qt::AscendingOrder;
                    return (order == Qt::AscendingOrder) ? (!m_isFolder) : (m_isFolder);
                }
                if (otherItem)
                    return m_size < otherItem->m_size;
            }
            else if (col == 3) // Date column
            {
                if (otherItem)
                    return m_dateModified < otherItem->m_dateModified;
            }

            return QTreeWidgetItem::operator<(other);
        }

    private:
        qulonglong m_size = 0;
        QDateTime m_dateModified;
        bool m_isFolder = false;
    };
}

EverythingResultsView::EverythingResultsView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(350);
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    auto *headerLayout = new QHBoxLayout();
    auto *label = new QLabel(tr("Everything 搜尋:"), this);
    m_queryEdit = new QLineEdit(this);
    m_queryEdit->setPlaceholderText(tr("輸入檔名或關鍵字..."));

    headerLayout->addWidget(label);
    headerLayout->addWidget(m_queryEdit);
    mainLayout->addLayout(headerLayout);

    m_statusLabel = new QLabel(this);
    mainLayout->addWidget(m_statusLabel);

    m_treeWidget = new QTreeWidget(this);
    m_treeWidget->setHeaderLabels({tr("名稱"), tr("路徑"), tr("大小"), tr("修改日期")});
    m_treeWidget->setSortingEnabled(true);
    m_treeWidget->header()->resizeSection(0, 220);
    m_treeWidget->header()->resizeSection(1, 240);
    m_treeWidget->header()->resizeSection(2, 70);
    m_treeWidget->header()->resizeSection(3, 120);
    m_treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    mainLayout->addWidget(m_treeWidget);

    // Restore sort settings from Preferences
    Preferences *pref = Preferences::instance();
    const int sortCol = pref->value(QStringLiteral("EverythingResultsView/sortColumn"), 2).toInt();
    const Qt::SortOrder sortOrder = static_cast<Qt::SortOrder>(pref->value(QStringLiteral("EverythingResultsView/sortOrder"), static_cast<int>(Qt::DescendingOrder)).toInt());
    m_treeWidget->header()->setSortIndicator(sortCol, sortOrder);

    connect(m_treeWidget->header(), &QHeaderView::sortIndicatorChanged, this, [](int logicalIndex, Qt::SortOrder order)
    {
        Preferences *pref = Preferences::instance();
        pref->setValue(QStringLiteral("EverythingResultsView/sortColumn"), logicalIndex);
        pref->setValue(QStringLiteral("EverythingResultsView/sortOrder"), static_cast<int>(order));
    });

    m_everythingSearch = new EverythingSearch(this);
    connect(m_everythingSearch, &EverythingSearch::searchCompleted, this, &EverythingResultsView::onSearchCompleted);
    connect(m_queryEdit, &QLineEdit::returnPressed, this, [this]()
    {
        updateSearchQuery(m_queryEdit->text());
    });
    connect(m_treeWidget, &QTreeWidget::itemDoubleClicked, this, &EverythingResultsView::onItemDoubleClicked);
    connect(m_treeWidget, &QTreeWidget::customContextMenuRequested, this, &EverythingResultsView::onTreeContextMenuRequested);

    if (!m_everythingSearch->isAvailable())
    {
        m_statusLabel->setText(tr("未偵測到正在運行的 Everything 服務。"));
    }
}

EverythingResultsView::~EverythingResultsView() = default;

void EverythingResultsView::updateSearchQuery(const QString &query)
{
    if (m_queryEdit->text() != query)
        m_queryEdit->setText(query);

    if (!m_everythingSearch->isAvailable())
    {
        m_statusLabel->setText(tr("未偵測到 Everything 服務。"));
        return;
    }

    m_statusLabel->setText(tr("正在搜尋: %1 ...").arg(query));
    m_everythingSearch->search(query);
}

void EverythingResultsView::onSearchCompleted(const QString &query, const QList<EverythingItem> &results, int totalMatches)
{
    Q_UNUSED(query);
    m_treeWidget->setUpdatesEnabled(false);
    m_treeWidget->setSortingEnabled(false);
    m_treeWidget->clear();
    m_statusLabel->setText(tr("找到 %1 個相符項目 (共 %2 個)").arg(results.size()).arg(totalMatches));

    QList<QTreeWidgetItem *> treeItems;
    treeItems.reserve(results.size());

    for (const EverythingItem &item : results)
    {
        treeItems.append(new EverythingTreeItem(item));
    }

    m_treeWidget->addTopLevelItems(treeItems);

    const int sortCol = m_treeWidget->header()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = m_treeWidget->header()->sortIndicatorOrder();

    m_treeWidget->setSortingEnabled(true);
    m_treeWidget->sortByColumn(sortCol, sortOrder);
    m_treeWidget->setUpdatesEnabled(true);
}

void EverythingResultsView::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item) return;

    const QString fullPath = item->data(0, Qt::UserRole).toString();
    if (!fullPath.isEmpty())
    {
        Utils::Gui::openPath(Path(fullPath));
    }
}

void EverythingResultsView::onTreeContextMenuRequested(const QPoint &pos)
{
    QTreeWidgetItem *item = m_treeWidget->itemAt(pos);
    if (!item) return;

    const QString name = item->text(0);
    const QString folderPath = item->text(1);
    const QString fullPath = item->data(0, Qt::UserRole).toString();

    if (fullPath.isEmpty()) return;

    const QPoint globalPos = m_treeWidget->viewport()->mapToGlobal(pos);

    QMenu menu(this);
    QAction *actOpen = menu.addAction(tr("開啟"));
    QAction *actOpenFolder = menu.addAction(tr("開啟所在資料夾"));
    menu.addSeparator();
    QAction *actCopyPath = menu.addAction(tr("複製完整路徑"));
    QAction *actCopyName = menu.addAction(tr("複製檔名"));
    QAction *actCopyFolderPath = menu.addAction(tr("複製資料夾路徑"));
    menu.addSeparator();
    QAction *actDelete = menu.addAction(tr("刪除"));

    QAction *selectedAction = menu.exec(globalPos);
    if (!selectedAction) return;

    if (selectedAction == actOpen)
    {
        Utils::Gui::openPath(Path(fullPath));
    }
    else if (selectedAction == actOpenFolder)
    {
        Utils::Gui::openFolderSelect(Path(fullPath));
    }
    else if (selectedAction == actCopyPath)
    {
        QGuiApplication::clipboard()->setText(fullPath);
    }
    else if (selectedAction == actCopyName)
    {
        QGuiApplication::clipboard()->setText(name);
    }
    else if (selectedAction == actCopyFolderPath)
    {
        QGuiApplication::clipboard()->setText(folderPath);
    }
    else if (selectedAction == actDelete)
    {
        if (QFile::moveToTrash(fullPath))
        {
            delete item;
        }
    }
}
