#include "everythingsearch.h"

#include <QtGlobal>
#include <QThreadPool>
#include <QTimer>
#include <QMetaObject>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>

#define EVERYTHING_IPC_COPYDATA_QUERYW 2
#define EVERYTHING_IPC_COPYDATA_LISTW 2

#define EVERYTHING_IPC_COPYDATA_QUERY2W 18
#define EVERYTHING_IPC_COPYDATA_LIST2W 18

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
    QThreadPool *everythingThreadPool()
    {
        static QThreadPool pool;
        static const bool initialized = []() {
            pool.setMaxThreadCount(1);
            return true;
        }();
        return &pool;
    }

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
    if (m_activeSearchState)
        m_activeSearchState->cancelled.store(true);

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
    if (dwData == EVERYTHING_IPC_COPYDATA_LISTW || dwData == EVERYTHING_IPC_COPYDATA_LIST2W)
    {
        if (cbData < sizeof(EVERYTHING_IPC_LISTW))
            return results;

        const auto *list = static_cast<const EVERYTHING_IPC_LISTW *>(lpData);
        totalMatches = static_cast<int>(list->totitems);

        const char *basePtr = reinterpret_cast<const char *>(list);
        const char *baseEnd = basePtr + cbData;
        const char *itemsStart = ((list->offset > 0) && (list->offset < cbData))
            ? (basePtr + list->offset)
            : (basePtr + 28);

        const DWORD numFolders = list->numfolders;
        const DWORD numFiles = list->numfiles;
        const DWORD totalItems = numFolders + numFiles;

        for (DWORD i = 0; i < totalItems; ++i)
        {
            const char *itemPtr = itemsStart + (i * sizeof(EVERYTHING_IPC_ITEMW));
            if (itemPtr + sizeof(EVERYTHING_IPC_ITEMW) > baseEnd)
                break;

            const auto *item1 = reinterpret_cast<const EVERYTHING_IPC_ITEMW *>(itemPtr);
            EverythingItem item;
            item.isFolder = (i < numFolders);

            if (item1->name_offset > 0 && item1->name_offset < cbData)
            {
                const char *namePtr = basePtr + item1->name_offset;
                if (namePtr + sizeof(wchar_t) <= baseEnd)
                {
                    const auto *wstr = reinterpret_cast<const wchar_t *>(namePtr);
                    const size_t maxWChars = (baseEnd - namePtr) / sizeof(wchar_t);
                    const size_t len = wcsnlen(wstr, maxWChars);
                    item.name = QString::fromWCharArray(wstr, static_cast<int>(len));
                }
            }

            if (item1->path_offset > 0 && item1->path_offset < cbData)
            {
                const char *pathPtr = basePtr + item1->path_offset;
                if (pathPtr + sizeof(wchar_t) <= baseEnd)
                {
                    const auto *wstr = reinterpret_cast<const wchar_t *>(pathPtr);
                    const size_t maxWChars = (baseEnd - pathPtr) / sizeof(wchar_t);
                    const size_t len = wcsnlen(wstr, maxWChars);
                    item.path = QString::fromWCharArray(wstr, static_cast<int>(len));
                }
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
                try
                {
                    int totalMatches = 0;
                    QList<EverythingItem> results = parseResponseBuffer(
                        static_cast<quintptr>(cds->dwData),
                        cds->lpData,
                        static_cast<quint32>(cds->cbData),
                        totalMatches
                    );

                    const QString query = self->m_currentQuery;
                    const quint64 currentId = self->m_searchId;
                    const auto taskState = self->m_activeSearchState;
                    QPointer<EverythingSearch> safeThis(self);

                    // Offload file size & timestamp checks to dedicated thread pool
                    // to prevent blocking the GUI thread when hundreds of files exist (especially on network drives)
                    everythingThreadPool()->start([safeThis, taskState, query, results, totalMatches, currentId]() mutable
                    {
                        if (taskState && taskState->cancelled.load())
                            return;

                        for (auto &item : results)
                        {
                            if (taskState && taskState->cancelled.load())
                                return;

                            const QString fullPath = item.path.isEmpty() ? item.name : QDir(item.path).filePath(item.name);
                            const QFileInfo fi(fullPath);
                            if (fi.exists())
                            {
                                if (fi.isFile())
                                    item.size = static_cast<qulonglong>(fi.size());
                                item.dateModified = fi.lastModified();
                            }
                        }

                        if (taskState && taskState->cancelled.load())
                            return;

                        QMetaObject::invokeMethod(qApp, [safeThis, taskState, query, results, totalMatches, currentId]()
                        {
                            if (!safeThis || (taskState && taskState->cancelled.load()))
                                return;

                            if (safeThis->m_searchId == currentId)
                            {
                                emit safeThis->searchCompleted(query, results, totalMatches);
                            }
                        });
                    });
                }
                catch (...)
                {
                }
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
    if (m_activeSearchState)
        m_activeSearchState->cancelled.store(true);
    m_activeSearchState = std::make_shared<SearchTaskState>();
    const auto taskState = m_activeSearchState;

    const quint64 searchId = ++m_searchId;
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

    if (!m_hwnd)
    {
        emit searchCompleted(query, {}, 0);
        return;
    }

    const HWND receiverHwnd = m_hwnd;
    QPointer<EverythingSearch> safeThis(this);

    // Watchdog timer: If Everything does not reply within 5 seconds,
    // emit empty results so UI does not stay stuck at "Searching..."
    QTimer::singleShot(5000, this, [safeThis, taskState, searchId, query]()
    {
        if (safeThis && !taskState->cancelled.load() && (safeThis->m_searchId == searchId))
        {
            emit safeThis->searchCompleted(query, {}, 0);
        }
    });

    everythingThreadPool()->start([safeThis, taskState, query, hwnd, receiverHwnd, searchId]()
    {
        if (taskState->cancelled.load())
            return;

        const std::wstring wquery = query.toStdWString();
        const size_t querySize = (wquery.length() + 1) * sizeof(wchar_t);
        const size_t allocSize = sizeof(EVERYTHING_IPC_QUERYW) + querySize;

        auto *queryStruct = static_cast<EVERYTHING_IPC_QUERYW *>(malloc(allocSize));
        if (!queryStruct)
        {
            QMetaObject::invokeMethod(qApp, [safeThis, taskState, searchId, query]()
            {
                if (safeThis && !taskState->cancelled.load() && (safeThis->m_searchId == searchId))
                    emit safeThis->searchCompleted(query, {}, 0);
            });
            return;
        }

        ZeroMemory(queryStruct, allocSize);
        queryStruct->reply_hwnd = static_cast<DWORD>(reinterpret_cast<uintptr_t>(receiverHwnd));
        queryStruct->reply_copydata_message = EVERYTHING_IPC_COPYDATA_LISTW;
        queryStruct->search_flags = 0;
        queryStruct->offset = 0;
        queryStruct->max_results = 100;
        memcpy(queryStruct->search_string, wquery.c_str(), querySize);

        COPYDATASTRUCT cds;
        cds.dwData = EVERYTHING_IPC_COPYDATA_QUERYW;
        cds.cbData = static_cast<DWORD>(allocSize);
        cds.lpData = queryStruct;

        DWORD_PTR sendResult = 0;
        const LRESULT lres = SendMessageTimeoutW(hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(receiverHwnd), reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000, &sendResult);
        free(queryStruct);

        if (lres == 0)
        {
            // SendMessageTimeoutW failed or timed out
            QMetaObject::invokeMethod(qApp, [safeThis, taskState, searchId, query]()
            {
                if (safeThis && !taskState->cancelled.load() && (safeThis->m_searchId == searchId))
                    emit safeThis->searchCompleted(query, {}, 0);
            });
        }
    });
#else
    emit searchCompleted(query, {}, 0);
#endif
}

