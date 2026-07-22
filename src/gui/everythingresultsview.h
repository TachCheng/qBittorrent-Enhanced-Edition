#pragma once

#include <QWidget>
#include <QList>
#include "everythingsearch.h"

class QTreeWidget;
class QLabel;
class QLineEdit;

class EverythingResultsView final : public QWidget
{
    Q_OBJECT

public:
    explicit EverythingResultsView(QWidget *parent = nullptr);
    ~EverythingResultsView() override;

    void updateSearchQuery(const QString &query);

private slots:
    void onSearchCompleted(const QString &query, const QList<EverythingItem> &results);

private:
    EverythingSearch *m_everythingSearch = nullptr;
    QLineEdit *m_queryEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTreeWidget *m_treeWidget = nullptr;
};
