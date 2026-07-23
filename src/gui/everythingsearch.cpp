#include "everythingsearch.h"

#include <QtGlobal>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include <windows.h>

#define EVERYTHING_IPC_COPYDATA_QUERYW 2
#define EVERYTHING_IPC_COPYDATA_LISTW 2

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

EverythingSearch::EverythingSearch(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DontShowOnScreen, true);
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
            if (cds && (cds->dwData == EVERYTHING_IPC_COPYDATA_LISTW))
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

                emit self->searchCompleted(self->m_currentQuery, results, static_cast<int>(list->totitems));
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
        const size_t allocSize = sizeof(EVERYTHING_IPC_QUERYW) + querySize;

        auto *queryStruct = static_cast<EVERYTHING_IPC_QUERYW *>(malloc(allocSize));
        if (!queryStruct) return;

        ZeroMemory(queryStruct, allocSize);
        queryStruct->reply_hwnd = static_cast<DWORD>(reinterpret_cast<uintptr_t>(receiverHwnd));
        queryStruct->reply_copydata_message = EVERYTHING_IPC_COPYDATA_LISTW;
        queryStruct->search_flags = 0;
        queryStruct->offset = 0;
        queryStruct->max_results = 100000;
        memcpy(queryStruct->search_string, wquery.c_str(), querySize);

        COPYDATASTRUCT cds;
        cds.dwData = EVERYTHING_IPC_COPYDATA_QUERYW;
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
