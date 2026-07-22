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

void EverythingSearch::search(const QString &query)
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
    queryStruct->reply_hwnd = static_cast<DWORD>(static_cast<uintptr_t>(winId()));
    queryStruct->reply_copydata_message = 0;
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

    SendMessageW(hwnd, WM_COPYDATA, static_cast<WPARAM>(static_cast<uintptr_t>(winId())), reinterpret_cast<LPARAM>(&cds));
    free(queryStruct);
#else
    emit searchCompleted(query, {});
#endif
}

#ifdef Q_OS_WIN
bool EverythingSearch::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(eventType);
    Q_UNUSED(result);

    const MSG *msg = static_cast<MSG *>(message);
    if (msg->message == WM_COPYDATA)
    {
        const COPYDATASTRUCT *cds = reinterpret_cast<COPYDATASTRUCT *>(msg->lParam);
        if (cds && cds->dwData == EVERYTHING_IPC_COPYDATA_LIST2W)
        {
            const auto *list = static_cast<const EVERYTHING_IPC_LIST2W *>(cds->lpData);
            QList<EverythingItem> results;

            if (list)
            {
                const char *basePtr = reinterpret_cast<const char *>(list);
                for (DWORD i = 0; i < list->numitems; ++i)
                {
                    const char *itemDataPtr = basePtr + list->items[i].data_offset;
                    EverythingItem item;

                    // Parse request fields in order: Name (string), Path (string), Size (qulonglong), DateModified (FILETIME)
                    const wchar_t *wname = reinterpret_cast<const wchar_t *>(itemDataPtr);
                    item.name = QString::fromWCharArray(wname);
                    itemDataPtr += (wcslen(wname) + 1) * sizeof(wchar_t);

                    const wchar_t *wpath = reinterpret_cast<const wchar_t *>(itemDataPtr);
                    item.path = QString::fromWCharArray(wpath);
                    itemDataPtr += (wcslen(wpath) + 1) * sizeof(wchar_t);

                    // Align pointer to 8 bytes for size and filetime if needed
                    uintptr_t ptrVal = reinterpret_cast<uintptr_t>(itemDataPtr);
                    if (ptrVal % sizeof(qulonglong) != 0)
                        ptrVal += (sizeof(qulonglong) - (ptrVal % sizeof(qulonglong)));
                    itemDataPtr = reinterpret_cast<const char *>(ptrVal);

                    const auto *sizePtr = reinterpret_cast<const qulonglong *>(itemDataPtr);
                    item.size = *sizePtr;
                    itemDataPtr += sizeof(qulonglong);

                    const auto *ftPtr = reinterpret_cast<const FILETIME *>(itemDataPtr);
                    ULARGE_INTEGER ull;
                    ull.LowPart = ftPtr->dwLowDateTime;
                    ull.HighPart = ftPtr->dwHighDateTime;
                    // FILETIME to QDateTime (Windows epoch to Unix epoch offset: 11644473600 seconds)
                    qint64 seconds = (ull.QuadPart / 10000000ULL) - 11644473600ULL;
                    item.dateModified = QDateTime::fromSecsSinceEpoch(seconds);

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
