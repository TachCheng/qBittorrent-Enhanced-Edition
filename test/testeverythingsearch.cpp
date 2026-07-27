#include <QTest>
#include <QDateTime>
#include "gui/everythingsearch.h"

#ifdef Q_OS_WIN
#include <windows.h>

#define EVERYTHING_IPC_COPYDATA_QUERYW 2
#define EVERYTHING_IPC_COPYDATA_LISTW 2

#pragma pack(push, 1)
struct EVERYTHING_IPC_ITEMW
{
    DWORD flags;
    DWORD filename_offset;
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
#endif

class TestEverythingSearch final : public QObject
{
    Q_OBJECT

private slots:
    void testParseQuery1Response()
    {
#ifdef Q_OS_WIN
        const wchar_t *sampleName = L"test_file.iso";
        const wchar_t *samplePath = L"C:\\Downloads";

        const size_t nameBytes = (wcslen(sampleName) + 1) * sizeof(wchar_t);
        const size_t pathBytes = (wcslen(samplePath) + 1) * sizeof(wchar_t);
        const size_t headerSize = offsetof(EVERYTHING_IPC_LISTW, items);
        const size_t itemSize = sizeof(EVERYTHING_IPC_ITEMW);

        const size_t nameOffset = headerSize + itemSize;
        const size_t pathOffset = nameOffset + nameBytes;
        const size_t totalBufferSize = pathOffset + pathBytes;

        QByteArray buffer(static_cast<int>(totalBufferSize), 0);

        auto *list = reinterpret_cast<EVERYTHING_IPC_LISTW *>(buffer.data());
        list->totfolders = 0;
        list->totfiles = 1;
        list->totitems = 1;
        list->numfolders = 0;
        list->numfiles = 1;
        list->numitems = 1;
        list->offset = static_cast<DWORD>(headerSize);

        list->items[0].flags = 0;
        list->items[0].filename_offset = static_cast<DWORD>(nameOffset);
        list->items[0].path_offset = static_cast<DWORD>(pathOffset);

        char *dataPtr = buffer.data() + nameOffset;
        memcpy(dataPtr, sampleName, nameBytes);
        dataPtr = buffer.data() + pathOffset;
        memcpy(dataPtr, samplePath, pathBytes);

        int totalMatches = 0;
        const QList<EverythingItem> results = EverythingSearch::parseResponseBuffer(
            EVERYTHING_IPC_COPYDATA_LISTW,
            buffer.constData(),
            static_cast<quint32>(buffer.size()),
            totalMatches
        );

        QCOMPARE(totalMatches, 1);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].name, QStringLiteral("test_file.iso"));
        QCOMPARE(results[0].path, QStringLiteral("C:\\Downloads"));
#endif
    }
};

QTEST_MAIN(TestEverythingSearch)
#include "testeverythingsearch.moc"
