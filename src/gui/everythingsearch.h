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
    void search(const QString &query);

signals:
    void searchCompleted(const QString &query, const QList<EverythingItem> &results);

private:
    QString m_currentQuery;
#ifdef Q_OS_WIN
    void *m_hwnd = nullptr;
    static int64_t __stdcall staticWndProc(void *hwnd, uint32_t msg, uint64_t wParam, int64_t lParam);
    void createNativeWindow();
    void destroyNativeWindow();
#endif
};
