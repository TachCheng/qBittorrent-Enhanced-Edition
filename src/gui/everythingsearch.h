#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>

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

Q_DECLARE_METATYPE(EverythingItem)

class EverythingSearch final : public QObject
{
    Q_OBJECT

public:
    explicit EverythingSearch(QObject *parent = nullptr);
    ~EverythingSearch() override;

    bool isAvailable() const;
    void search(const QString &query);

    static QList<EverythingItem> parseResponseBuffer(quintptr dwData, const void *lpData, quint32 cbData, int &totalMatches);

signals:
    void searchCompleted(const QString &query, const QList<EverythingItem> &results, int totalMatches);

private:
    QString m_currentQuery;
#ifdef Q_OS_WIN
    HWND m_hwnd = nullptr;
    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void createNativeWindow();
    void destroyNativeWindow();
#endif
};
