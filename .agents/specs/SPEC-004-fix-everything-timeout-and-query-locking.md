# SPEC-004: 徹底修復 Everything 搜尋 5 秒逾時誤殺、相同查詢鎖死與單一關鍵字語法問題

## Problem Statement

使用者在新增 Torrent / 磁力連結對話框（AddNewTorrentDialog）中反饋：
**「新增 torrent 視窗又找不到 everything 的資料了」**。

從使用者上傳的實際運作截圖中清晰可見：
1. **qBittorrent 對話框端**：
   - 搜尋關鍵字欄顯示：`Everything 搜尋: <SNOS 374>`
   - 搜尋結果狀態標籤顯示：`找到 0 個相符項目 (共 0 個)`
   - 左側檔案樹中最大影音檔案 `SNOS-374-UC.mp4` (2.11 GiB) 已正確被自動勾選。
2. **Everything 原生搜尋視窗端（由使用者並列開啟比對）**：
   - 使用者在 Everything 視窗中搜尋 `snos 374`
   - 搜尋結果清晰列出 6 筆相符資料（包含 `SNOS-374.mp4`、`SNOS-374-UC.torrent`、`SNOS-374` 資料夾等，均位於網路磁碟槽 `S:\` 即 `\\n100pro\BT` 上）。
3. **重試失敗**：
   - 使用者即使在搜尋框中重新按下 Enter，或在檔案樹中取消勾選後再重新勾選，介面依然卡死在「找到 0 個相符項目」，無法重新刷新出結果。

### 核心根因深層診斷 (Root Cause Analysis)

1. **未取消的 5 秒 Watchdog 單次計時器誤殺在線結果 (The 5-Second Watchdog Timebomb)**：
   - 在 `EverythingSearch::search()` 中，先前設置了一個單次計時器：
     `QTimer::singleShot(5000, this, [safeThis, searchId, query]() { ... emit safeThis->searchCompleted(query, {}, 0); });`
   - **致命設計缺陷**：`QTimer::singleShot` 屬於發射後無法撤銷（Fire-and-forget）的計時器。當 Everything IPC 在數十毫秒內成功回傳結果給視窗接收端時，這個 5 秒定時器**並未被停止**。
   - 5 秒一到，計時器無條件被觸發，檢查到 `safeThis->m_searchId == searchId` 仍然成立，強行調用 `emit safeThis->searchCompleted(query, {}, 0)`，直接呼叫 `m_treeWidget->clear()` 清空所有已查到的清單，並將狀態標籤覆寫為「找到 0 個相符項目 (共 0 個)」。
2. **相同查詢永久鎖死 (Permanent Search Suppression via Stale Current Query)**：
   - 先前在防護邏輯中加入了 `if ((query == m_currentQuery) && (m_searchId > 0)) return;`。
   - 由於 `m_currentQuery` 在搜尋完成或被計時器誤殺後**從未被重置**，且未區分「正在搜尋中 (In-Flight)」與「搜尋已結束」。
   - 當 5 秒計時器將畫面強制洗成 0 筆後，使用者手動在搜尋框敲擊 Enter、或點擊取消/重新勾選檔案時，重新生成的查詢依然是 `<SNOS 374>`。
   - `search()` 判定查詢字串與 `m_currentQuery` 相同且 `m_searchId > 0`，直接 `return` 忽略，導致後續所有重新整理操作全數失效，永久定格在查無資料。
3. **單一關鍵字被誤套用 `< >` 群組運算子**：
   - 在查詢字串產生邏輯中，只要關鍵字含有空格，就一律包裹為 `<SNOS 374>`。
   - 角括號在 Everything 語法中主要是用於多組 `OR` 運算的邏輯分組（如 `<term1> | <term2>`）。對於單一搜尋詞，包裹 `<>` 既冗餘，又與使用者在原生 Everything 視窗中搜尋的純文字 `snos 374` 不一致。
4. **網路磁碟 (SMB / NAS) 的 Win32 檔案屬性延遲**：
   - 舊邏輯在背景執行緒中針對每筆搜尋結果使用 Qt 的 `QFileInfo` 進行 `exists()`、`size()`、`lastModified()` 查詢。
   - 使用者的儲存路徑為 NAS 網路共享磁碟槽（`S:\` 即 `\\n100pro\BT`）。當機械硬碟剛休眠或網路有 SMB 協議延遲時，逐一建立 `QFileInfo` 耗時較長，更容易超過舊的 5 秒時限。

---

## Solution

1. **徹底拆除 5 秒 Watchdog 定時炸彈**：
   - 移除無條件觸發的 `QTimer::singleShot(5000)`。
   - Windows API `SendMessageTimeoutW` 本身已經內建 5000ms 的逾時控制機制。若 Everything 服務逾時未回應，`SendMessageTimeoutW` 自動返回 0，此時才安全回報空結果，絕不在正常回應後二次覆寫。
2. **引入即時「搜尋中狀態」管理 (`m_isSearching`)**：
   - 精確追蹤搜尋生命週期：僅在「上一次完全相同的查詢**正在背景執行緒中等待回傳**」時，才抑制重複發送。
   - 一旦 Everything 回傳結果、或搜尋逾時/失敗結束，立即標記 `m_isSearching = false`。
   - 使用者在任何時候於搜尋框按下 Enter、或在檔案樹更動勾選狀態，均能立即重新發起搜尋並刷新畫面。
3. **修復 `SendMessageTimeoutW` 呼叫旗標**：
   - 將旗標由 `SMTO_ABORTIFHUNG` 改為 `SMTO_NORMAL`（0），防止 Windows 系統因 Everything 在系統托盤處於閒置狀態而誤判為 Not Responding 提前中止。
4. **採用 Win32 原生 `GetFileAttributesExW` 輕量化查詢**：
   - 在背景檢查結果檔案時，直接呼叫 Win32 原生系統呼叫 `GetFileAttributesExW`，繞過 Qt 繁重的檔案引擎快取與解析，將 NAS 網路磁碟上的大小與修改時間讀取耗時縮減至微秒等級。
5. **純淨化單一關鍵字查詢語法**：
   - 當有效關鍵字僅有 1 個時，直接輸出原始純淨字串（例如 `SNOS 374`），不加 `<>` 角括號，與原生 Everything 介面 100% 一致。
   - 僅當有 2 個以上不同關鍵字需要以 ` | `（OR 邏輯）聯合查詢時，才對帶有空格的分項包裹 `<>`。

---

## User Stories

1. 作為種子下載者，當我開啟一個剛下載的新種子對話框時，Everything 預覽區塊能立即顯示出相符的本機或 NAS 檔案，且結果會一直穩定顯示在畫面上，絕不會在開啟 5 秒後突然被清空為「找到 0 個相符項目」。
2. 作為種子下載者，當我將種子視窗開著超過數分鐘（甚至數小時）去處理其他工作時，視窗內的 Everything 搜尋結果依然保持完整，不會因逾時計時器而在背景被洗掉。
3. 作為種子下載者，如果初次載入時 NAS 硬碟正在休眠喚醒而導致搜尋結果有短暫延遲，當我手動在 Everything 搜尋框按下 Enter 鍵時，系統能立即重新發動查詢並顯示最新結果，不會因為「字串相同」而被當作無效操作忽略。
4. 作為種子下載者，當我在種子檔案樹中取消勾選某個檔案、再重新勾選它時，Everything 搜尋結果能即時跟隨重新檢索並更新列表。
5. 作為種子下載者，當自動選取的檔案為單一影音（例如 `SNOS-374-UC.mp4`）時，搜尋框中顯示的文字為簡潔乾淨的 `SNOS 374`，而不是帶有角括號的 `<SNOS 374>`，讓搜尋語法與 Everything 原生介面完全一致。
6. 作為種子下載者，當多個檔案包含不同番號（例如合集種子）時，搜尋字串能自動組合成 `<KEYWORD 1> | <KEYWORD 2>` 的正確 OR 群組語法，精確查詢所有相關檔案。
7. 作為種子下載者，當搜尋結果位於網路磁碟槽（如 `\\n100pro\BT`）時，程式能以極低系統開銷迅速取得檔案大小與修改日期，介面不會感到卡頓。
8. 作為種子下載者，當 Everything 應用程式未開啟或未安裝時，介面能精確顯示「未偵測到正在運行的 Everything 服務」，而不是顯示找到 0 個項目。
9. 作為種子下載者，當我關閉正在搜尋中的對話框時，背景工作執行緒安全終止，絕不會發生記憶體洩漏或野指標（UAF）崩潰。
10. 作為種子下載者，當我連續快速點擊「全選」、「全不選」、「Max MP4」按鈕時，搜尋請求能被合理防抖合併，不會引起執行緒風暴或程式當機。

---

## Implementation Decisions

### 1. 縫合點設計 (Testing & Architectural Seam)
- **單一核心縫合點**：`EverythingSearch` 物件及其發射的 `searchCompleted(const QString &query, const QList<EverythingItem> &results, int totalMatches)` 訊號。
- 該介面是 UI 視圖層（`EverythingResultsView`）與 Windows 原生 IPC 引擎之間的唯一合約，外部行為完全由 `search(query)` 輸入與 `searchCompleted(...)` 輸出定義。

### 2. 狀態機與生命週期模型
- 移除拋出後不可控的 `QTimer::singleShot`。
- 在 `EverythingSearch` 內部維護原子/執行緒安全的 `m_isSearching` 標誌：
  - 進入 `search()` 且通過合法性檢查後，設定 `m_isSearching = true`。
  - 當收到 IPC 回應並解析完成、或 `SendMessageTimeoutW` 報告失敗/逾時返回時，設定 `m_isSearching = false`。
  - 抑制條件精確化：僅當 `m_isSearching && (query == m_currentQuery)` 時略過；搜尋完成後，即使 `query == m_currentQuery` 也允許重新觸發查詢。

### 3. IPC 訊息傳送最佳化
- `SendMessageTimeoutW` 採用 `SMTO_NORMAL`（0），逾時時間維持 5000ms。
- Windows 會在 Everything 處理完成後才返回；若 Everything 真正當機或無回應，5 秒後函式返回 0，此時由工作執行緒排程通知 UI 顯示逾時（0 筆），邏輯清晰可控。

### 4. 檔案屬性獲取輕量化
- 背景執行緒遍歷搜尋結果時，使用 Win32 API：
  ```cpp
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
      if (!(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
          item.size = (static_cast<qulonglong>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
      ULARGE_INTEGER ull;
      ull.LowPart = fad.ftLastWriteTime.dwLowDateTime;
      ull.HighPart = fad.ftLastWriteTime.dwHighDateTime;
      item.dateModified = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>((ull.QuadPart - 116444736000000000ULL) / 10000));
  }
  ```
- 避免建立 Qt 物件樹與檔案引擎實例，大幅降低網路磁碟延遲。

### 5. 查詢字串產生規則
- 單一關鍵字：`query = keyword`（無角括號）。
- 多關鍵字：僅對包含空格的項目包裹 `<>`，並以 ` | ` 拼接。

---

## Testing Decisions

### 1. 什麼是良好的測試 (Good Test Criteria)
- 測試必須**僅檢驗外部可觀察行為**（搜尋訊號是否正確發射、結果數量是否如預期、相同查詢在完成後能否被再次接受），嚴禁依賴或測試私有內部細節。
- 測試必須覆蓋：
  1. 單一關鍵字產生規則（不帶 `<>`）。
  2. 多關鍵字產生規則（帶 `<>` 與 ` | `）。
  3. 搜尋完成後，允許再次發起同名搜尋。
  4. 快速連續發起同名搜尋時，不引起重入或中斷在線任務。
  5. 物件快速析構與併發安全（零 UAF 回歸驗證）。

### 2. 受測試模組
- `EverythingSearch`（IPC 核心與結果解析模組）。
- `AddNewTorrentDialog::doEverythingSearch` / 相關查詢構建邏輯。

### 3. 先前測試資產 (Prior Art)
- `test/testeverythingsearch.cpp`：已具備 `testParseQuery1Response`、`testRapidCreationAndDestruction`、`testMultipleConcurrentSearchesAndDestruction`。在此基礎上擴充重複查詢與重試生命週期測試。
- `test/testutilsmisc.cpp`：已具備 `extractReleaseCode` 的單元測試。

---

## Out of Scope

1. Everything 搜尋結果的直接刪除或重新命名功能（本規格書僅涵蓋搜尋預覽與雙擊開啟功能）。
2. 對非 Windows 系統（如 Linux / macOS）上的 Everything IPC 支援（Everything 本身僅運作於 Windows）。
3. 修改 Everything 伺服器本身的資料庫索引配置。

---

## Further Notes

- 此修正完全向下相容 Everything 1.4 與 1.5 系列。
- 解決此問題後，預覽搜尋在任何機械硬碟休眠喚醒、網路延遲或視窗常開情境下均能保持 100% 穩定呈現。
