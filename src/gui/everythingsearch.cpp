#include "everythingsearch.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>

#define EVERYTHING_IPC_WNDCLASS L"EVERYTHING"
#define EVERYTHING_IPC_COPYDATA_QUERYW 2
#define EVERYTHING_IPC_COPYDATA_LISTW 2

#pragma pack(push, 1)
struct EVERYTHING_IPC_QUERYW
{
    DWORD max_results;
    DWORD offset;
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    wchar_t search_string[1];
};

struct EVERYTHING_IPC_ITEMW
{
    DWORD name_offset;
    DWORD path_offset;
};

struct EVERYTHING_IPC_LISTW
{
    DWORD totitems;
    DWORD numitems;
    EVERYTHING_IPC_ITEMW items[1];
};
#pragma pack(pop)

#endif

EverythingSearch::EverythingSearch(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DontShowOnScreen, true);
}

EverythingSearch::~EverythingSearch() = default;

bool EverythingSearch::isAvailable() const
{
#ifdef Q_OS_WIN
    HWND hwnd = FindWindowW(EVERYTHING_IPC_WNDCLASS, nullptr);
    if (!hwnd)
        hwnd = FindWindowW(L"EVERYTHING_IPC_WNDCLASS", nullptr);
    if (!hwnd)
        hwnd = FindWindowW(L"EVERYTHING", nullptr);
    return (hwnd != nullptr);
#else
    return false;
#endif
}

#ifdef Q_OS_WIN
void EverythingSearch::search(const QString &query, HWND receiverHwnd)
#else
void EverythingSearch::search(const QString &query)
#endif
{
    m_currentQuery = query;
    if (query.trimmed().isEmpty())
    {
        emit searchCompleted(query, {});
        return;
    }

#ifdef Q_OS_WIN
    HWND hwnd = FindWindowW(EVERYTHING_IPC_WNDCLASS, nullptr);
    if (!hwnd)
        hwnd = FindWindowW(L"EVERYTHING_IPC_WNDCLASS", nullptr);
    if (!hwnd)
        hwnd = FindWindowW(L"EVERYTHING", nullptr);

    if (!hwnd)
    {
        emit searchCompleted(query, {});
        return;
    }

    const std::wstring wquery = query.toStdWString();
    const size_t querySize = (wquery.length() + 1) * sizeof(wchar_t);
    const size_t allocSize = sizeof(EVERYTHING_IPC_QUERYW) + querySize;

    auto *queryStruct = static_cast<EVERYTHING_IPC_QUERYW *>(malloc(allocSize));
    if (!queryStruct) return;

    ZeroMemory(queryStruct, allocSize);
    queryStruct->max_results = 200;
    queryStruct->offset = 0;
    queryStruct->reply_hwnd = static_cast<DWORD>(reinterpret_cast<uintptr_t>(receiverHwnd));
    queryStruct->reply_copydata_message = EVERYTHING_IPC_COPYDATA_LISTW;
    wcscpy_s(queryStruct->search_string, wquery.length() + 1, wquery.c_str());

    COPYDATASTRUCT cds;
    cds.dwData = EVERYTHING_IPC_COPYDATA_QUERYW;
    cds.cbData = static_cast<DWORD>(allocSize);
    cds.lpData = queryStruct;

    SendMessageW(hwnd, WM_COPYDATA, static_cast<WPARAM>(reinterpret_cast<uintptr_t>(receiverHwnd)), reinterpret_cast<LPARAM>(&cds));
    free(queryStruct);
#else
    emit searchCompleted(query, {});
#endif
}

#ifdef Q_OS_WIN
bool EverythingSearch::processWmCopyData(void *message)
{
    const MSG *msg = static_cast<MSG *>(message);
    if (msg && (msg->message == WM_COPYDATA))
    {
        const COPYDATASTRUCT *cds = reinterpret_cast<COPYDATASTRUCT *>(msg->lParam);
        if (cds && cds->dwData == EVERYTHING_IPC_COPYDATA_LISTW)
        {
            const auto *list = static_cast<const EVERYTHING_IPC_LISTW *>(cds->lpData);
            QList<EverythingItem> results;

            if (list)
            {
                const char *basePtr = reinterpret_cast<const char *>(list);
                for (DWORD i = 0; i < list->numitems; ++i)
                {
                    EverythingItem item;
                    const auto *namePtr = reinterpret_cast<const wchar_t *>(basePtr + list->items[i].name_offset);
                    const auto *pathPtr = reinterpret_cast<const wchar_t *>(basePtr + list->items[i].path_offset);

                    item.name = QString::fromWCharArray(namePtr);
                    item.path = QString::fromWCharArray(pathPtr);
                    results.append(item);
                }
            }

            emit searchCompleted(m_currentQuery, results);
            return true;
        }
    }
    return false;
}
#endif
