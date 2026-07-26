#include "everythingsearch.h"

#include <QtGlobal>
#include <QThreadPool>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>

#define EVERYTHING_IPC_COPYDATA_QUERYW 2
#define EVERYTHING_IPC_COPYDATA_LISTW 2

#define EVERYTHING_IPC_COPYDATA_QUERY2W 2
#define EVERYTHING_IPC_COPYDATA_LIST2W 2

#define EVERYTHING_IPC_QUERY2_REQUEST_NAME          0x00000001
#define EVERYTHING_IPC_QUERY2_REQUEST_PATH          0x00000002
#define EVERYTHING_IPC_QUERY2_REQUEST_SIZE          0x00000010
#define EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED 0x00000040

#pragma pack(push, 1)
struct EVERYTHING_IPC_QUERYW
{
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD offset;
    DWORD max_results;
    wchar_t search_string[1];
};

struct EVERYTHING_IPC_ITEMW
{
    DWORD flags;
    DWORD name_offset;
    DWORD path_offset;
};

struct EVERYTHING_IPC_LISTW
{
    DWORD totfolders;
    DWORD totfiles;
    DWORD totitems;
    DWORD numfolders;
    DWORD numfiles;
    DWORD numitems;
    DWORD offset;
    EVERYTHING_IPC_ITEMW items[1];
};

struct EVERYTHING_IPC_QUERY2W
{
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD offset;
    DWORD max_results;
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
    DWORD totfolders;
    DWORD totfiles;
    DWORD totitems;
    DWORD numfolders;
    DWORD numfiles;
    DWORD numitems;
    DWORD offset;
    DWORD request_flags;
    DWORD sort_type;
    EVERYTHING_IPC_ITEM2W items[1];
};
#pragma pack(pop)

namespace
{
    HWND getEverythingHwnd()
    {
        HWND hwnd = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
        if (!hwnd)
            hwnd = FindWindowW(L"EVERYTHING_IPC_WNDCLASS", nullptr);
        if (!hwnd)
            hwnd = FindWindowW(L"EVERYTHING", nullptr);
        return hwnd;
    }
}
#endif

EverythingSearch::EverythingSearch(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    createNativeWindow();
#endif
}

EverythingSearch::~EverythingSearch()
{
#ifdef Q_OS_WIN
    destroyNativeWindow();
#endif
}

QList<EverythingItem> EverythingSearch::parseResponseBuffer(quintptr dwData, const void *lpData, quint32 cbData, int &totalMatches)
{
    QList<EverythingItem> results;
    totalMatches = 0;
    if (!lpData || (cbData == 0))
        return results;

#ifdef Q_OS_WIN
    if (dwData == EVERYTHING_IPC_COPYDATA_LIST2W)
    {
        if (cbData < sizeof(EVERYTHING_IPC_LIST2W))
            return results;

        const auto *list = static_cast<const EVERYTHING_IPC_LIST2W *>(lpData);
        totalMatches = static_cast<int>(list->totitems);

        const char *basePtr = reinterpret_cast<const char *>(list);
        const char *itemsStart = ((list->offset > 0) && (list->offset < cbData))
            ? (basePtr + list->offset)
            : reinterpret_cast<const char *>(list->items);

        for (DWORD i = 0; i < list->numitems; ++i)
        {
            const char *itemPtr = itemsStart + (i * sizeof(EVERYTHING_IPC_ITEM2W));
            if (itemPtr + sizeof(EVERYTHING_IPC_ITEM2W) > basePtr + cbData)
                break;

            const auto *item2 = reinterpret_cast<const EVERYTHING_IPC_ITEM2W *>(itemPtr);
            const char *dataPtr = itemPtr + item2->data_offset;
            if ((dataPtr < basePtr) || (dataPtr >= basePtr + cbData))
                continue;

            EverythingItem item;
            const DWORD req = list->request_flags;

            if (req & EVERYTHING_IPC_QUERY2_REQUEST_NAME)
            {
                const auto *wstr = reinterpret_cast<const wchar_t *>(dataPtr);
                item.name = QString::fromWCharArray(wstr);
                dataPtr += (wcslen(wstr) + 1) * sizeof(wchar_t);
            }
            if (req & EVERYTHING_IPC_QUERY2_REQUEST_PATH)
            {
                const auto *wstr = reinterpret_cast<const wchar_t *>(dataPtr);
                item.path = QString::fromWCharArray(wstr);
                dataPtr += (wcslen(wstr) + 1) * sizeof(wchar_t);
            }
            if (req & EVERYTHING_IPC_QUERY2_REQUEST_SIZE)
            {
                if (dataPtr + sizeof(LARGE_INTEGER) <= basePtr + cbData)
                {
                    LARGE_INTEGER sz;
                    memcpy(&sz, dataPtr, sizeof(LARGE_INTEGER));
                    item.size = static_cast<qulonglong>(sz.QuadPart);
                }
                dataPtr += sizeof(LARGE_INTEGER);
            }
            if (req & EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED)
            {
                if (dataPtr + sizeof(FILETIME) <= basePtr + cbData)
                {
                    FILETIME ft;
                    memcpy(&ft, dataPtr, sizeof(FILETIME));
                    const ULONGLONG ftVal = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                    if (ftVal >= 116444736000000000ULL)
                    {
                        const qint64 msecs = static_cast<qint64>((ftVal - 116444736000000000ULL) / 10000ULL);
                        item.dateModified = QDateTime::fromMSecsSinceEpoch(msecs, Qt::UTC).toLocalTime();
                    }
                }
                dataPtr += sizeof(FILETIME);
            }

            // Fallback to local disk if item size is 0 and path exists
            if ((item.size == 0) && !item.name.isEmpty())
            {
                const QString fullPath = item.path.isEmpty() ? item.name : QDir(item.path).filePath(item.name);
                QFileInfo fi(fullPath);
                if (fi.exists() && fi.isFile())
                {
                    item.size = static_cast<qulonglong>(fi.size());
                    if (!item.dateModified.isValid())
                        item.dateModified = fi.lastModified();
                }
            }

            results.append(item);
        }
    }
    else if (dwData == EVERYTHING_IPC_COPYDATA_LISTW)
    {
        if (cbData < sizeof(EVERYTHING_IPC_LISTW))
            return results;

        const auto *list = static_cast<const EVERYTHING_IPC_LISTW *>(lpData);
        totalMatches = static_cast<int>(list->totitems);

        const char *basePtr = reinterpret_cast<const char *>(list);
        for (DWORD i = 0; i < list->numitems; ++i)
        {
            EverythingItem item;
            const auto *namePtr = reinterpret_cast<const wchar_t *>(basePtr + list->items[i].name_offset);
            const auto *pathPtr = reinterpret_cast<const wchar_t *>(basePtr + list->items[i].path_offset);

            item.name = QString::fromWCharArray(namePtr);
            item.path = QString::fromWCharArray(pathPtr);

            const QString fullPath = item.path.isEmpty() ? item.name : QDir(item.path).filePath(item.name);
            QFileInfo fi(fullPath);
            if (fi.exists() && fi.isFile())
            {
                item.size = static_cast<qulonglong>(fi.size());
                item.dateModified = fi.lastModified();
            }

            results.append(item);
        }
    }
#endif
    return results;
}

#ifdef Q_OS_WIN
void EverythingSearch::createNativeWindow()
{
    if (m_hwnd) return;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = staticWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"qBittorrent_Everything_Receiver";
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, L"qBittorrent_Everything_Receiver", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (m_hwnd)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        // Allow WM_COPYDATA through Windows UIPI filter
        typedef BOOL (WINAPI *pfnChangeWindowMessageFilterEx)(HWND, UINT, DWORD, PVOID);
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32)
        {
            auto pChangeFilter = reinterpret_cast<pfnChangeWindowMessageFilterEx>(GetProcAddress(hUser32, "ChangeWindowMessageFilterEx"));
            if (pChangeFilter)
            {
                pChangeFilter(m_hwnd, WM_COPYDATA, 1 /* MSGFLT_ALLOW */, nullptr);
            }
        }
    }
}

void EverythingSearch::destroyNativeWindow()
{
    if (m_hwnd)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK EverythingSearch::staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_COPYDATA)
    {
        auto *self = reinterpret_cast<EverythingSearch *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self)
        {
            const COPYDATASTRUCT *cds = reinterpret_cast<const COPYDATASTRUCT *>(lParam);
            if (cds && (cds->dwData == EVERYTHING_IPC_COPYDATA_LIST2W || cds->dwData == EVERYTHING_IPC_COPYDATA_LISTW))
            {
                int totalMatches = 0;
                const QList<EverythingItem> results = parseResponseBuffer(
                    static_cast<quintptr>(cds->dwData),
                    cds->lpData,
                    static_cast<quint32>(cds->cbData),
                    totalMatches
                );

                emit self->searchCompleted(self->m_currentQuery, results, totalMatches);
                return TRUE;
            }
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif

bool EverythingSearch::isAvailable() const
{
#ifdef Q_OS_WIN
    return (getEverythingHwnd() != nullptr);
#else
    return false;
#endif
}

void EverythingSearch::search(const QString &query)
{
    m_currentQuery = query;
    if (query.trimmed().isEmpty())
    {
        emit searchCompleted(query, {}, 0);
        return;
    }

#ifdef Q_OS_WIN
    HWND hwnd = getEverythingHwnd();
    if (!hwnd)
    {
        emit searchCompleted(query, {}, 0);
        return;
    }

    if (!m_hwnd)
        createNativeWindow();

    if (!m_hwnd) return;

    const HWND receiverHwnd = m_hwnd;
    QThreadPool::globalInstance()->start([query, hwnd, receiverHwnd]()
    {
        const std::wstring wquery = query.toStdWString();
        const size_t querySize = (wquery.length() + 1) * sizeof(wchar_t);
        const size_t allocSize = sizeof(EVERYTHING_IPC_QUERY2W) + querySize;

        auto *queryStruct = static_cast<EVERYTHING_IPC_QUERY2W *>(malloc(allocSize));
        if (!queryStruct) return;

        ZeroMemory(queryStruct, allocSize);
        queryStruct->reply_hwnd = static_cast<DWORD>(reinterpret_cast<uintptr_t>(receiverHwnd));
        queryStruct->reply_copydata_message = EVERYTHING_IPC_COPYDATA_LIST2W;
        queryStruct->search_flags = 0;
        queryStruct->offset = 0;
        queryStruct->max_results = 100000;
        queryStruct->request_flags = EVERYTHING_IPC_QUERY2_REQUEST_NAME
                                   | EVERYTHING_IPC_QUERY2_REQUEST_PATH
                                   | EVERYTHING_IPC_QUERY2_REQUEST_SIZE
                                   | EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED;
        queryStruct->sort_type = 0;
        memcpy(queryStruct->search_string, wquery.c_str(), querySize);

        COPYDATASTRUCT cds;
        cds.dwData = EVERYTHING_IPC_COPYDATA_QUERY2W;
        cds.cbData = static_cast<DWORD>(allocSize);
        cds.lpData = queryStruct;

        DWORD_PTR sendResult = 0;
        SendMessageTimeoutW(hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(receiverHwnd), reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 3000, &sendResult);
        free(queryStruct);
    });
#else
    emit searchCompleted(query, {}, 0);
#endif
}

