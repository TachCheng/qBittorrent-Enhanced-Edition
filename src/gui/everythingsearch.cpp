#include "everythingsearch.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>

#define EVERYTHING_IPC_WNDCLASS L"EVERYTHING"
#define EVERYTHING_IPC_COPYDATA_QUERYW 2
#define EVERYTHING_IPC_COPYDATA_LISTW 2

#define EVERYTHING_IPC_COPYDATA_QUERY2W 18
#define EVERYTHING_IPC_COPYDATA_LIST2W 18

#define EVERYTHING_IPC_QUERY2_REQUEST_NAME 0x00000001
#define EVERYTHING_IPC_QUERY2_REQUEST_PATH 0x00000002
#define EVERYTHING_IPC_QUERY2_REQUEST_SIZE 0x00000010
#define EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED 0x00000020

#pragma pack(push, 1)
struct EVERYTHING_IPC_QUERY2W
{
    DWORD max_results;
    DWORD offset;
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD request_flags;
    DWORD sort_type;
    wchar_t search_string[1];
};

struct EVERYTHING_IPC_ITEM2W
{
    DWORD flags;
    DWORD data_offset;
};

struct EVERYTHING_IPC_LIST2W
{
    DWORD totitems;
    DWORD numitems;
    DWORD offset;
    EVERYTHING_IPC_ITEM2W items[1];
};
#pragma pack(pop)

#endif

EverythingSearch::EverythingSearch(QWidget *parent)
    : QWidget(parent)
{
    // Invisible helper widget to receive WM_COPYDATA
    setAttribute(Qt::WA_DontShowOnScreen, true);
}

EverythingSearch::~EverythingSearch() = default;

bool EverythingSearch::isAvailable() const
{
#ifdef Q_OS_WIN
    HWND hwnd = FindWindowW(EVERYTHING_IPC_WNDCLASS, nullptr);
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
    const size_t allocSize = sizeof(EVERYTHING_IPC_QUERY2W) + querySize;

    auto *queryStruct = static_cast<EVERYTHING_IPC_QUERY2W *>(malloc(allocSize));
    if (!queryStruct) return;

    ZeroMemory(queryStruct, allocSize);
    queryStruct->max_results = 200;
    queryStruct->offset = 0;
    queryStruct->reply_hwnd = static_cast<DWORD>(reinterpret_cast<uintptr_t>(receiverHwnd));
    queryStruct->reply_copydata_message = EVERYTHING_IPC_COPYDATA_LIST2W;
    queryStruct->search_flags = 0;
    queryStruct->request_flags = EVERYTHING_IPC_QUERY2_REQUEST_NAME
                                | EVERYTHING_IPC_QUERY2_REQUEST_PATH
                                | EVERYTHING_IPC_QUERY2_REQUEST_SIZE
                                | EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED;
    queryStruct->sort_type = 0;
    wcscpy_s(queryStruct->search_string, wquery.length() + 1, wquery.c_str());

    COPYDATASTRUCT cds;
    cds.dwData = EVERYTHING_IPC_COPYDATA_QUERY2W;
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
        if (cds && (cds->dwData == EVERYTHING_IPC_COPYDATA_LIST2W || cds->dwData == 2))
        {
            const auto *list = static_cast<const EVERYTHING_IPC_LIST2W *>(cds->lpData);
            QList<EverythingItem> results;

            if (list)
            {
                const char *basePtr = reinterpret_cast<const char *>(list);
                for (DWORD i = 0; i < list->numitems; ++i)
                {
                    const auto *namePtr = reinterpret_cast<const wchar_t *>(itemDataPtr + list->items[i].name_offset);
                    const auto *pathPtr = reinterpret_cast<const wchar_t *>(itemDataPtr + list->items[i].path_offset);

                    item.name = QString::fromWCharArray(namePtr);
                    item.path = QString::fromWCharArray(pathPtr);

                    if (list->items[i].request_flags & EVERYTHING_IPC_QUERY2_REQUEST_SIZE)
                        item.size = list->items[i].size;

                    if (list->items[i].request_flags & EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED)
                    {
                        ULARGE_INTEGER ull;
                        ull.LowPart = list->items[i].date_modified.dwLowDateTime;
                        ull.HighPart = list->items[i].date_modified.dwHighDateTime;
                        qint64 seconds = (ull.QuadPart / 10000000ULL) - 11644473600ULL;
                        item.dateModified = QDateTime::fromSecsSinceEpoch(seconds);
                    }
                    results.append(item);
                }
            }

            emit searchCompleted(m_currentQuery, results);
            return true;
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}
#endif
