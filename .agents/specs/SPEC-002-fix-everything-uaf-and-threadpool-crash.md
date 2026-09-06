# SPEC-002: Everything IPC 異步生命週期與執行緒池防崩潰規範

## Problem Statement

使用者回報當在 qBittorrent-Enhanced-Edition 中**開啟多個磁力連結（Magnet Links）或新 Torrent 對話框（AddNewTorrentDialog），或是對話框開啟過久時，整個程式會直接崩潰**。

### 核心根因診斷 (Root Causes)

1. **生命週期懸空指標與 Use-After-Free 崩潰**：
   - 在 `EverythingSearch::search` 與 `staticWndProc` 中，異步執行緒直接捕獲了未受保護的 raw 指標（`this` 與 `self`）：
     `QThreadPool::globalInstance()->start([self, query, results, totalMatches, currentId]() mutable { ... });`
   - 當使用者開啟多個對話框並隨後關閉（或點擊確定/取消），`AddNewTorrentDialog` 會被銷毀，連帶釋放其子物件 `EverythingResultsView` 與 `EverythingSearch`。
   - 然而，`QThreadPool::globalInstance()` 中的背景執行緒仍在執行耗時的網路/磁碟檔案屬性查詢或在互斥鎖上排隊。當該執行緒完成後呼叫：
     `QMetaObject::invokeMethod(self, [self, ...]() { ... });`
     `self` 已經是釋放後的記憶體（Dangling Pointer），Qt 於 `self->thread()` 嘗試解引用時觸發 `0xC0000005 Access Violation`，導致程式立即崩潰。

2. **全局執行緒池飢餓與跨行程 Win32 IPC 阻塞**：
   - 搜尋請求使用全局執行緒池 `QThreadPool::globalInstance()` 並以靜態互斥鎖 `s_everythingIpcMutex` 序列化。
   - 在低核心數設備（如 Intel N100 僅 4 核心 4 執行緒，`maxThreadCount == 4`）上，若開啟 4 個以上對話框，所有執行緒會被 `s_everythingIpcMutex` 或 `SendMessageTimeoutW` (5000ms) 佔滿。
   - `staticWndProc` 解析完 Everything 回覆後，需要向 `QThreadPool::globalInstance()` 排入檔案屬性查詢任務，但此時執行緒池已經被其他等待互斥鎖的搜尋請求塞滿，導致任務排隊積壓。

3. **大量檔案批次網路磁碟 SMB I/O 阻塞**：
   - Everything 回傳最多 1,000 筆結果，每筆結果均執行 `QFileInfo fi(fullPath)`。
   - 當路徑位於網路磁碟機（UNC / SMB 如 `\\n100pro` 或 `M:\`）時，單次查詢可達數千次網路 SMB 往返，若多個對話框同時開啟，會產生數萬次網路磁碟查詢，導致 Windows 網路重定向驅動程式逾時、執行緒累積、句柄耗盡並卡死崩潰。

4. **磁力連結（Magnet）非同步生命週期競爭**：
   - 磁力連結初啟動時無 metadata（`!hasMetadata`），待 libtorrent 在背景下載完成後，`SessionImpl` 發送 `metadataDownloaded` 訊號，觸發 `AddNewTorrentDialog::updateMetadata`，進而觸發 `selectMaxMp4()` 與 `triggerEverythingSearch()`。
   - 若使用者長時間保持視窗開啟，當有多個磁力連結在隨機時間點陸續完成 metadata 下載時，會突然並發發起多次 Everything 搜尋與背景任務，若使用者在此時關閉對話框，極易觸發上述第 1 點的 Use-After-Free 崩潰。

---

## Solution

1. **零 UAF 異步生命週期防護 (Safe Lifetime with QPointer & Cancellation Token)**：
   - 在 `EverythingSearch` 引入基於 `std::shared_ptr<std::atomic<bool>>` 的取消標記（Cancellation Token）。
   - 在 `EverythingSearch` 解構式（Destructor）中立即將取消標記設為 `true`，並安全註銷 Win32 訊息視窗 `m_hwnd`。
   - 所有異步任務執行前後均檢查取消標記，若物件已銷毀或查詢已被新查詢取代，立即終止並丟棄任務。
   - 拋回 Qt 主事件循環時，**統一以 `qApp` 作為目標 Context Object**（保證在應用程式生命週期內始終有效），並搭配 `QPointer<EverythingSearch>` 弱引用安全守護，徹底杜絕存取野指標與 Use-After-Free。

2. **專屬 Everything 異步執行緒池 (Dedicated Single-Worker ThreadPool)**：
   - 將 Everything IPC 查詢從共享的 `QThreadPool::globalInstance()` 隔離至專屬的執行緒池 `s_everythingThreadPool`，並將最大執行緒數限制為 1。
   - 單執行緒池自然實現請求先進先出（FIFO）序列化，完全免除靜態互斥鎖競爭，絕不消耗或阻塞 libtorrent 與 Qt 核心元件的全局執行緒。
   - 當前一項查詢已被取消時，工作執行緒自隊列取出後直接在 0 微秒內略過，優先處理最新的搜尋需求。

3. **限制查詢數量與安全屬性補完**：
   - 將預設搜尋上限自 1,000 筆縮減至實用合理的 100 筆（`max_results = 100`），避免對網路磁碟機進行無意義的超大量 SMB 遍歷。
   - 在屬性遍歷循環中每一步檢查 `cancelled` 標記，若對話框關閉或發起新搜尋則立刻中斷。

---

## User Stories

1. 作為使用者，當我同時開啟 10 個或更多磁力連結視窗時，程式能穩定運行，不會因為背景 IPC 查詢或多視窗競爭而崩潰。
2. 作為使用者，當磁力連結視窗開啟數分鐘以上、並在背景陸續獲取到 metadata 時，介面能正常更新並搜尋，程式保持穩定。
3. 作為使用者，當我在搜尋進行中或檔案屬性查詢期間隨時關閉、接受或取消任何種子對話框時，程式保證安全釋放資源，絕不發生記憶體存取違規崩潰。
4. 作為使用者，在低核心數電腦（如 4 核心無超執行緒的 N100）上使用時，開啟多個種子視窗不會耗盡全局執行緒池，亦不影響種子下載與 libtorrent 運作。

---

## Implementation Decisions

### 1. `EverythingSearch` 內部生命週期設計
```cpp
struct EverythingSearchState
{
    std::atomic<bool> cancelled {false};
};

class EverythingSearch : public QObject
{
    // ...
private:
    std::shared_ptr<EverythingSearchState> m_activeSearchState;
    // ...
};
```
在發起新搜尋或解構時：
```cpp
if (m_activeSearchState)
    m_activeSearchState->cancelled.store(true);
m_activeSearchState = std::make_shared<EverythingSearchState>();
```
在執行緒回呼時：
```cpp
QPointer<EverythingSearch> safeThis(self);
std::shared_ptr<EverythingSearchState> state = ...;

QMetaObject::invokeMethod(qApp, [safeThis, state, query, results, totalMatches, currentId]()
{
    if (!safeThis || state->cancelled.load() || safeThis->m_searchId != currentId)
        return;
    emit safeThis->searchCompleted(query, results, totalMatches);
});
```

### 2. 獨立執行緒池
```cpp
QThreadPool *everythingThreadPool()
{
    static QThreadPool pool;
    static const bool initialized = []() {
        pool.setMaxThreadCount(1);
        return true;
    }();
    return &pool;
}
```

---

## Testing Decisions

1. **單元測試驗證生命週期安全性**：
   在 `test/testeverythingsearch.cpp` 中建立並發建立與銷毀測試：
   連續快速建立並銷毀 20 個 `EverythingSearch` 實例，同時觸發搜尋，驗證在物件消亡後背景任務不會導致非法記憶體讀取或崩潰。
2. **零延遲與取消驗證**：
   驗證標記取消後，任務會立即返回，不觸發 `searchCompleted` 訊號。
