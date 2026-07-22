#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>
#include <QWidget>

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
    void search(const QString &query);

signals:
    void searchCompleted(const QString &query, const QList<EverythingItem> &results);

protected:
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
    QString m_currentQuery;
};
