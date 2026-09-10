# SPEC-003: 解決 Everything 搜尋初始無資料但重選後正常之問題

## Problem Statement

使用者回報在 qBittorrent-Enhanced-Edition 中：
雖然開啟多個 Torrent/磁力連結時程式已不再崩潰，但 Everything 預覽搜尋出現新問題：**一開始開啟 Torrent 視窗時常常顯示「查詢不到」（找到 0 個相符項目），但若手動對 Torrent 檔案樹中的項目取消勾選、再重新勾選一次，Everything 就能正確查到資料**。

### 核心根因診斷 (Root Causes)

1. **`selectMaxMp4()` 僅比對 `.mp4` 副檔名，導致大量 `.mkv` / 4K / Remux 種子所有檔案全數被設為 `Ignored`**：
   - 於 `TorrentContentModel::selectMaxMp4()` 中：
     `if (file->name().endsWith(u".mp4", Qt::CaseInsensitive))`
   - 當種子主要影音為 `.mkv`（現代 4K / 1080p Web-DL 與壓制版的主流封裝）或 `.avi`, `.wmv`, `.ts`, `.m2ts`, `.iso` 時，`maxMp4File` 判定為 `nullptr`。
   - 迴圈接續將所有檔案均設定為 `BitTorrent::DownloadPriority::Ignored`！
   - 此時 `m_contentAdaptor->filePriorities()` 所有檔案全為 `Ignored`。
   - `AddNewTorrentDialog::doEverythingSearch()` 遍歷所有檔案皆為 `Ignored` 而直接跳過，`keywordsSet` 呈現全空。
   - 退回使用 `torrentDescr.name()`，若種子標題包含站點前綴標籤（如 `[ThZu.Cc]`），Everything 將中括號視為字元集合/Regex 語法而導致查詢失敗，顯示「找到 0 個相符項目」。
   - 當使用者在介面上手動點擊取消並重新勾選該檔案時，`setItemPriority()` 將其明確設為 `Normal`，重新觸發 `doEverythingSearch()`，成功擷取番號並搜出資料。

2. **`selectMaxMp4()` 僅刷新 `index(0, 0)` 單一節點，引發 UI 勾選框狀態不同步**：
   - 於 `TorrentContentModel::selectMaxMp4()` 結尾：
     `notifySubtreeUpdated(index(0, 0), columns);`
   - 若種子為多檔案且無單一根目錄封裝（Flat 結構或 `NoSubfolder` 佈局），`index(0, 0)` 僅代表第 0 列檔案。第 1 列、第 2 列等兄弟節點從未收到 `dataChanged` 訊號！
   - 導致樹狀視圖（QTreeView）介面上仍然繪製為「已勾選（綠色打勾）」的假象，但內部實質優先權已為 `Ignored`。
   - 使用者看到檔案「明明有勾選」卻搜不到，因此手動取消再勾選，剛好修正了底層模型與 Adaptor 的優先權。

3. **`doEverythingSearch()` 將非媒體附屬檔案（`.url`, `.txt`, `.jpg`）無差別納入關鍵字與語法衝突**：
   - 在 `doEverythingSearch()` 中：
     ```cpp
     const QString code = Utils::Misc::extractReleaseCode(fileName);
     if (!code.isEmpty())
         keywordsSet.insert(code);
     else if (!fileName.trimmed().isEmpty())
         keywordsSet.insert(fileName.trimmed());
     ```
   - 若種子內勾選了宣傳網址（如 `[ThZu.Cc] 66.url`）、說明檔（`readme.txt`）或封面（`cover.jpg`），因無法提煉番號，完整檔名會被直接塞進 `keywordsSet`。
   - 導致生成的 Everything 查詢變成：
     `<ADKL 363> | [ThZu.Cc] 66.url | readme.txt`
   - Everything 搜尋語法中 `[`、`]`、`!`、`|` 具有特殊運算符意義，未逸出的字元極易破壞搜尋語法或被大量無關檔案擠滿 100 筆上限，導致使用者真正想要的番號資料無法呈現。
   - **關鍵原則**：只要有任一勾選檔案能提取出標準番號代碼（如 `ADKL 363`），就**絕不可**將其他非代碼附屬檔名混入搜尋字串中。

4. **`commit aa73b5633` 引入的單執行緒池 (`maxThreadCount = 1`) 隊列阻塞與激進 `cancelled` 機制（關鍵迴歸根因）**：
   - 在 `aa73b5633` 中，為解決多視窗關閉時的野指標 UAF 崩潰，引入了全域靜態單執行緒池 `everythingThreadPool(maxThreadCount = 1)` 與 `m_activeSearchState->cancelled`。
   - 該單一執行緒池同時執行 Everything IPC 與結果檔案屬性檢查（`QFileInfo` 讀取 UNC/網路磁碟檔案大小與修改時間）。
   - 當初次開啟視窗時，若存在網路磁碟延遲，查詢任務排在隊列中無法執行，觸發了 5 秒 Watchdog 計時器回傳 0 筆資料。
   - 同時，視窗載入與佈局（`sectionResized`）期間若觸發重複搜尋，`search()` 會立即將前一次查詢標記為 `cancelled.store(true)`，導致剛回傳的初次結果在回呼中直接被丟棄。
   - 手動取消再勾選之所以正常，是因為此時視窗已穩定靜止，且隊列已空，單一搜尋得以順利返回。

---

## Solution

1. **擴充 `selectMaxMp4()` 為全影音格式智慧選取（支援 `.mkv`, `.mp4`, `.avi`, `.wmv` 等）並提供安全兜底**：
   - 擴展副檔名比對清單至常見媒體格式：`.mp4`, `.mkv`, `.avi`, `.wmv`, `.mov`, `.flv`, `.ts`, `.m2ts`, `.m4v`, `.webm`, `.iso`, `.rmvb`, `.vob`。
   - 優先選取容量最大的影音檔案為 `Normal`，其餘設為 `Ignored`。
   - **安全兜底 (Fallback)**：若種子中完全不存在任何影音格式（如壓縮包、純音樂或軟體），**嚴禁將全部檔案設為 `Ignored`**，應保留最大單檔或維持預設勾選，避免 0 檔案被選取的異常狀態。

2. **修正 `TorrentContentModel` 頂層節點完整廣播**：
   - 在 `selectMaxMp4()`, `selectGreaterThanSize()`, `updateFilesPriorities()` 中，遍歷所有頂層列（`for (int r = 0; r < rowCount(); ++r)`），確保所有頂層項目與其子項目均完整收到 `dataChanged` 訊號，保證 UI 呈現與後端優先權 100% 同步。

3. **淨化 `doEverythingSearch()` 查詢構建邏輯**：
   - **第一優先級**：只要任何勾選的檔案中提取出了 Release Code（如 `AKDL 363`），查詢字串**僅保留 Release Code**，徹底過濾並忽略 `url`, `txt`, `jpg`, `nfo` 等附屬檔案。
   - **第二優先級**：若勾選檔案均無 Release Code，嘗試從種子標題提取 Release Code。
   - **第三優先級**：若仍無 Release Code，過濾掉非媒體雜訊副檔名，選取容量最大的主檔案檔名（去除副檔名），並對 Everything 敏感字元（`[`, `]`, `!`, `|`, `<`, `>`）進行安全清理後再發起搜尋。

4. **健全 EverythingSearch 併發與生命週期（消除迴歸與零 UAF）**：
   - 移除單執行緒限制 `everythingThreadPool(maxThreadCount = 1)`，檔案屬性檢查回歸 `QThreadPool::globalInstance()` 平行執行，避免佇列阻塞。
   - 移除過激的 `cancelled` 機制，改用「相同查詢抑制（`if (query == m_currentQuery && m_searchId > 0) return;`）」，確保在線合法查詢不被中斷或誤殺。
   - 全程保留 `QPointer<EverythingSearch> safeThis` 與 Win32 視窗 `GWLP_USERDATA` 清零機制，視窗關閉時自動安全退出，徹底保證零 UAF。
   - 恢復結果上限 `max_results = 1000`。

---

## User Stories

1. 作為使用者，當我打開包含 `.mkv` 格式（如 4K Remux / Web-DL）的種子時，視窗會自動勾選該最大影音檔，且 Everything 預覽視窗能立即自動搜出對應的番號資料，無須手動重選。
2. 作為使用者，當種子內包含宣傳網址（.url）、文字說明（.txt）或封面圖片時，Everything 搜尋不會被這些雜訊檔案干擾，只會精準搜尋番號。
3. 作為使用者，無論種子是否有單一資料夾層級，介面上的檔案勾選狀態與底層下載優先權隨時保持精確同步。
