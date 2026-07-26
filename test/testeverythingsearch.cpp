#include <QTest>
#include <QDateTime>
#include "gui/everythingsearch.h"

#ifdef Q_OS_WIN
#include <windows.h>

#define EVERYTHING_IPC_COPYDATA_QUERY2W 2
#define EVERYTHING_IPC_COPYDATA_LIST2W 2

#define EVERYTHING_IPC_QUERY2_REQUEST_NAME          0x00000001
#define EVERYTHING_IPC_QUERY2_REQUEST_PATH          0x00000002
#define EVERYTHING_IPC_QUERY2_REQUEST_SIZE          0x00000010
#define EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED 0x00000040

#pragma pack(push, 1)
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
#endif

class TestEverythingSearch final : public QObject
{
    Q_OBJECT

private slots:
    void testParseQuery2Response()
    {
#ifdef Q_OS_WIN
        const wchar_t *sampleName = L"test_file.iso";
        const wchar_t *samplePath = L"C:\\Downloads";
        const qulonglong sampleSize = 1073741824ULL; // 1 GB

        const size_t nameBytes = (wcslen(sampleName) + 1) * sizeof(wchar_t);
        const size_t pathBytes = (wcslen(samplePath) + 1) * sizeof(wchar_t);
        const size_t dataPayloadSize = nameBytes + pathBytes + sizeof(LARGE_INTEGER) + sizeof(FILETIME);

        const size_t totalBufferSize = sizeof(EVERYTHING_IPC_LIST2W) + dataPayloadSize;
        QByteArray buffer(static_cast<int>(totalBufferSize), 0);

        auto *list = reinterpret_cast<EVERYTHING_IPC_LIST2W *>(buffer.data());
        list->totfolders = 0;
        list->totfiles = 1;
        list->totitems = 1;
        list->numfolders = 0;
        list->numfiles = 1;
        list->numitems = 1;
        list->offset = offsetof(EVERYTHING_IPC_LIST2W, items);
        list->request_flags = EVERYTHING_IPC_QUERY2_REQUEST_NAME
                            | EVERYTHING_IPC_QUERY2_REQUEST_PATH
                            | EVERYTHING_IPC_QUERY2_REQUEST_SIZE
                            | EVERYTHING_IPC_QUERY2_REQUEST_DATE_MODIFIED;
        list->sort_type = 0;

        list->items[0].flags = 0;
        list->items[0].data_offset = sizeof(EVERYTHING_IPC_ITEM2W);

        char *dataPtr = buffer.data() + sizeof(EVERYTHING_IPC_LIST2W);
        memcpy(dataPtr, sampleName, nameBytes);
        dataPtr += nameBytes;

        memcpy(dataPtr, samplePath, pathBytes);
        dataPtr += pathBytes;

        LARGE_INTEGER sz;
        sz.QuadPart = static_cast<LONGLONG>(sampleSize);
        memcpy(dataPtr, &sz, sizeof(LARGE_INTEGER));
        dataPtr += sizeof(LARGE_INTEGER);

        FILETIME ft;
        ft.dwLowDateTime = 0xD53E8000;
        ft.dwHighDateTime = 0x01DA8320;
        memcpy(dataPtr, &ft, sizeof(FILETIME));

        int totalMatches = 0;
        const QList<EverythingItem> results = EverythingSearch::parseResponseBuffer(
            EVERYTHING_IPC_COPYDATA_LIST2W,
            buffer.constData(),
            static_cast<quint32>(buffer.size()),
            totalMatches
        );

        QCOMPARE(totalMatches, 1);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].name, QStringLiteral("test_file.iso"));
        QCOMPARE(results[0].path, QStringLiteral("C:\\Downloads"));
        QCOMPARE(results[0].size, sampleSize);
        QVERIFY(results[0].size > 0);
#endif
    }
};

QTEST_MAIN(TestEverythingSearch)
#include "testeverythingsearch.moc"
