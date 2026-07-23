#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

struct EverythingItem
{
    QString name;
    QString path;
    qulonglong size = 0;
    QDateTime dateModified;
};

class EverythingSearch final : public QWidget
{
    Q_OBJECT

public:
    explicit EverythingSearch(QWidget *parent = nullptr);
    ~EverythingSearch() override;

    bool isAvailable() const;
#ifdef Q_OS_WIN
    void search(const QString &query, HWND receiverHwnd);
    bool processWmCopyData(void *message);
#else
    void search(const QString &query);
#endif

signals:
    void searchCompleted(const QString &query, const QList<EverythingItem> &results);

private:
    QString m_currentQuery;
};
