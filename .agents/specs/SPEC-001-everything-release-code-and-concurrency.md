# SPEC-001: Everything 搜尋番號精準擷取與多對話框並發防卡死規範

## Problem Statement

當使用者在 qBittorrent-Enhanced-Edition 中使用 Everything 預覽搜尋功能時，面臨兩個嚴重的問題：

1. **番號擷取不準確**：
   當 Torrent 內部包含帶有網站網域、宣傳字樣浮水印（如 `dx5c.xyzAKDL-363CX.mp4`、`hhd800.com@AKDL-363CX.mp4`、`[ThZu.Cc]AKDL-363.mp4`）的檔案時，搜尋關鍵字抽取邏輯貪婪匹配了多餘的字元，導致擷取出的番號變成 `<zAKDL 363>` 而非正確的 `<AKDL 363>`。這導致 Everything 搜尋不到任何本地檔案，預覽功能失效。

2. **多個 Torrent 視窗同時開啟時搜尋卡死**：
   當使用者一次批次開啟多個 Torrent 新增對話框時，Everything 預覽視窗會永久停滯在 `正在搜尋: <WAAA 637> ...`，無法顯示任何搜尋結果，也不會恢復或超時。其原因是：
   - 檔案選取與對話框初始化期間連續發出大量未防抖（debounce）的查詢，擠爆 Everything 單執行緒訊息佇列。
   - 每筆搜尋結果返回時，程式在 Qt GUI 主執行緒上對數百筆檔案執行同步磁碟/網路 I/O（`QFileInfo` 檢查），造成 Qt 主事件循環與 Windows 訊息處理凍結，特別是在掛載網路磁碟機（UNC / SMB 如 `M:\`）時延遲更為嚴重。
   - `SendMessageTimeoutW` 逾時後未進行任何狀態復原或回呼，導致介面永久鎖死在搜尋中狀態。

---

## Solution

1. **建立獨立且健壯的番號提取機制**：
   在執行番號擷取前，先自動過濾清理常見網址、TLD 網域名稱字尾、方括號標籤，並處理駝峰式命名（小寫轉大寫字元邊界），最後使用邊界防護的正則表達式萃取正確的番號與數字組合，並統一轉為大寫標準格式（如 `AKDL 363`）。

2. **全面非同步化與並發保護**：
   - 將所有磁碟與網路檔案屬性查詢（`QFileInfo`）徹底從 GUI 主執行緒中抽離，移入背景執行緒池處理，GUI 主執行緒 0 延遲。
   - 在 Torrent 新增對話框引入成員防抖計時器（250ms），確保在多個檔案優先順序變動或大量視窗同時開啟時，僅在穩定後發起單次查詢。
   - 增加背景 Everything IPC 互斥鎖，序列化發往 Everything 的查詢請求。
   - 設置 5 秒看門狗計時器與逾時錯誤捕獲，確保即使 Everything 逾時未響應，也能正常重置狀態為「找到 0 個相符項目」，絕不卡死。

---

## User Stories

1. 作為使用者，當我新增一個檔案名為 `dx5c.xyzAKDL-363CX.mp4` 的種子時，我希望 Everything 自動搜尋 `<AKDL 363>`，以便我能夠立刻找到本地硬碟或 NAS 上的對應檔案。
2. 作為使用者，當種子檔案名稱帶有網址前綴（如 `hhd800.com@...` 或 `www.jav.com_...`）時，我希望程式能自動過濾這些網址雜訊，以便萃取出純淨的影片番號。
3. 作為使用者，當種子檔案名稱帶有發行標籤（如 `[ThZu.Cc]`、`【720P】`、`[FHD]`）時，我希望程式能自動移除標籤，不干擾番號識別。
4. 作為使用者，當種子內容為 FC2 系列檔案（如 `FC2-PPV-1234567.mp4` 或 `FC2-1234567.mp4`）時，我希望程式能辨識並格式化為 `FC2 PPV 1234567` 進行搜尋。
5. 作為使用者，當檔案名稱為小寫（如 `akdl-363.mp4`）時，我希望搜尋關鍵字能標準化為大寫 `AKDL 363`，以保持查詢格式一致。
6. 作為使用者，當我一次性拖曳加入 10 個種子時，我希望所有開啟的種子對話框都能正常搜尋，且介面順暢不凍結。
7. 作為使用者，當我在種子檔案樹中快速點擊勾選或取消多個檔案時，我希望搜尋請求能自動防抖（Debounce），不會對系統造成高頻發送與重複負載。
8. 作為使用者，當搜尋結果中的檔案位於較慢的網路磁碟機（如 `M:\` 或 UNC 路徑）時，我希望 qBittorrent 視窗不會因此無回應（Not Responding）。
9. 作為使用者，當 Everything 服務因負載過高或未響應超過 5 秒時，我希望介面能結束「正在搜尋」狀態並顯示結果提示，而不是永遠卡住。
10. 作為使用者，當某個檔案未能提取出任何番號特徵時，我希望系統能回退使用完整檔名作為搜尋字串，避免遺漏搜尋。
11. 作為使用者，當種子內部檔案均未勾選時，我希望系統能自動退回使用種子標題（Torrent Name）抽取關鍵字進行搜尋。

---

## Implementation Decisions

### 1. 模組職責劃分
- **通用字串與工具模組 (`Utils::Misc`)**：
  負責提供純粹、無 GUI 依賴的番號清洗與解析 API：
  `QString extractReleaseCode(const QString &text)`
  該介面作為最高層級的公開測試接縫，便於單元測試直接驗證各類邊界檔名。
- **種子對話框控制器 (`AddNewTorrentDialog`)**：
  負責管理檔案樹變更事件與防抖調度。內部配置 `QTimer m_everythingSearchTimer`（250ms 單次計時器），接收到檔案選取或資料變更時僅重置計時器，逾時後才發動搜尋。
- **Everything IPC 引擎 (`EverythingSearch`)**：
  - 核心搜尋發起：在背景執行緒中透過靜態互斥鎖保護 `SendMessageTimeoutW`，防止多執行緒對 Everything 訊息迴圈產生飢餓或碰撞。
  - 視窗訊息接收：Win32 訊息回呼（`staticWndProc`）僅快速解析記憶體指標中的名稱與路徑（< 0.05ms），立即返回 `TRUE` 釋放 Everything 進程。
  - 屬性非同步補完：解析後的項目列表丟入背景執行緒池進行 `QFileInfo` 檢查，完成後以 Qt Queued Connection 安全拋回主執行緒。
  - 雙重逾時看門狗：若發送失敗或 5 秒未收到回呼，強制觸發 `searchCompleted(query, {}, 0)` 重置 UI 狀態。

### 2. 番號清理管線演算法
依序套用四道清洗防護：
1. 截斷最後副檔名（若副檔名長度小於 6 字元）。
2. 剔除常見 TLD 網域名稱（匹配 `.xyz`, `.com`, `.net`, `.cc`, `.top` 等 30+ 種常見域名）。
3. 剔除中括號 `[]`、圓括號 `()`、中文括號 `【】` 中的內容。
4. 於小寫字母銜接大寫字母之處插入空白斷詞（例：`xyzAKDL` $\to$ `xyz AKDL`）。
5. 優先比對 FC2 PPV 模式；次之比對具有非英文字元邊界（`(?<![a-zA-Z])`）之 2~5 字母 + 3~5 數字標準番號。

---

## Testing Decisions

### 1. 良好測試原則
- **行為驅動而非實作耦合**：不針對私有方法進行 Mock，直接測試公開函式輸入各類極端檔名時的輸出是否符合預期。
- **避免同義反覆（Non-Tautological）**：期望值直接使用常數字串（如 `"AKDL 363"`），不使用運算推導。
- **垂直切片（Vertical Slice）**：先建立失敗的測試案例，再實現最小剛好通過的清理代碼。

### 2. 測試接縫與範圍
- **模組測試**：[test/testutilsmisc.cpp](file:///u:/Software/VSCode_AI/qBittorrent-Enhanced-Edition/test/testutilsmisc.cpp)
  涵蓋測試案例：
  - `dx5c.xyzAKDL-363CX.mp4` $\to$ `AKDL 363`
  - `hhd800.com@AKDL-363CX.mp4` $\to$ `AKDL 363`
  - `[ThZu.Cc]AKDL-363CX.mp4` $\to$ `AKDL 363`
  - `www.jav.com_AKDL-363.mp4` $\to$ `AKDL 363`
  - `FC2-PPV-1234567.mp4` $\to$ `FC2 PPV 1234567`
  - `WAAA-637.mp4` $\to$ `WAAA 637`
  - `MIDA-532.mp4` $\to$ `MIDA 532`

---

## Out of Scope

1. 支援第三方程式（如非官方 Everything 衍生版、自定義 Named Instance 實例名稱）。
2. 針對非標準番號（如純中文名稱電影、無代號之動漫番劇）進行自然語言實體識別（NER）。
3. 修改 Everything 本身索引與快取資料庫之行為。

---

## Further Notes

- 標籤分類：`ready-for-agent`
- 本技術規格書由 `/to-spec` 技能根據對話脈絡與代碼庫深入分析自動生成。
