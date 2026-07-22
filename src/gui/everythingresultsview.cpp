#include "everythingresultsview.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QLineEdit>
#include <QLabel>
#include <QHeaderView>
#include "base/utils/misc.h"

EverythingResultsView::EverythingResultsView(QWidget *parent)
    : QWidget(parent)
{
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
    m_treeWidget->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_treeWidget->header()->resizeSection(0, 180);
    m_treeWidget->header()->resizeSection(1, 200);
    m_treeWidget->header()->resizeSection(2, 80);
    mainLayout->addWidget(m_treeWidget);

    m_everythingSearch = new EverythingSearch(this);
    connect(m_everythingSearch, &EverythingSearch::searchCompleted, this, &EverythingResultsView::onSearchCompleted);
    connect(m_queryEdit, &QLineEdit::returnPressed, this, [this]()
    {
        updateSearchQuery(m_queryEdit->text());
    });

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

void EverythingResultsView::onSearchCompleted(const QString &query, const QList<EverythingItem> &results)
{
    Q_UNUSED(query);
    m_treeWidget->clear();
    m_statusLabel->setText(tr("找到 %1 個相符項目").arg(results.size()));

    for (const EverythingItem &item : results)
    {
        auto *treeItem = new QTreeWidgetItem(m_treeWidget);
        treeItem->setText(0, item.name);
        treeItem->setText(1, item.path);
        treeItem->setText(2, Utils::Misc::friendlyUnit(item.size));
        treeItem->setText(3, item.dateModified.isValid() ? item.dateModified.toString(u"yyyy/MM/dd hh:mm"_s) : QString{});
    }
}
