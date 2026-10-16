# OBS Relay Buffer — 1.0

OBS Studio 外掛：將 OBS 主畫面編碼為 H.264，快取最近 60 秒，並在獨立視窗中以播放器式 UI 回放。

目標環境：Windows + OBS Studio **32.2.2**（x64）。

## 功能

1. **選單注入**：`Tools` 下新增 `Relay Buffer Preview`。
2. **獨立視窗**：標題 `Relay Preview`，可縮放，無 OBS 父視窗。
3. **單例**：視窗已開時再點選單 → `raise()` + `activateWindow()`，不建第二個實例。
4. **關閉視窗**：只銷毀該視窗，OBS 繼續運行。
5. **關閉 OBS**：監聽 `OBS_FRONTEND_EVENT_EXIT` / `SCRIPTING_SHUTDOWN`，一併關閉獨立視窗，不留殭屍行程。
6. **播放器式回放**：
   - 背景持續將 OBS 主畫面編碼為 H.264。
   - Ring buffer 快取最近 **60 秒** encoded packets。
   - Slider（-60s ~ 0s）、Play / Pause、邊解邊播、拖曳 seek。
   - 解碼目標：**1640×920** YUV420P。

## 系統需求

- OBS Studio **32.2.2**（Windows x64）
- Windows 10 / 11 x64
- 建議記憶體：**16 GB 以上**（回放時 FrameStore 約需 7.8 GB）
- 建置需要：CMake 3.28+、Visual Studio 2022 Build Tools（含 MSVC）、Windows SDK

> **注意**：本外掛使用 OBS 內建的 FFmpeg（avcodec 62 / avutil 60 / swscale 9）。
> 若使用舊版 OBS（FFmpeg 7.x），外掛將無法載入。
> 安裝時**不需要**額外複製任何 FFmpeg DLL。

## 建置與安裝

本專案使用 OBS 官方 plugin template 的 PowerShell 腳本。

### 一鍵建置 + 安裝

```powershell
cd C:\Users\Pero\OBS-MVP-1

# 建置（第一次會下載 OBS 依賴並編譯，時間較長）
powershell -ExecutionPolicy Bypass -File .\scripts\Build-Windows.ps1

# 安裝到 OBS
powershell -ExecutionPolicy Bypass -File .\scripts\Install-To-OBS.ps1
```

安裝完成後，**完全重啟 OBS**。

### 手動等價指令（備用）

如果腳本不可用，可以手動執行：

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

產物：`build_x64\rundir\RelWithDebInfo\obs-relay-buffer.dll`

然後手動複製到：

```
%APPDATA%\obs-studio\plugins\obs-relay-buffer\bin\64bit\obs-relay-buffer.dll
```

> **只需要這一個檔案。** 不需要複製任何 FFmpeg DLL。

### 依賴版本

`buildspec.json` 使用：

- `obs-studio`: `32.2.2`
- `prebuilt` (obs-deps): `2026-06-25`
- `qt6`: `2026-06-25`

`CMakeLists.txt` 會從 `build_x64/.deps/obs-deps-2026-06-25-x64` 取得 FFmpeg，
與 OBS 執行時載入的版本一致，因此外掛不需要自帶 FFmpeg DLL。

> 若 OBS 版本更新，需同步更新 `buildspec.json` 的 `obs-studio` 版本與
> `prebuilt` / `qt6` 版本，以及 `CMakeLists.txt` 的 `OBS_DEPS_VERSION`。

## 安裝步驟（手動）

> 若你已執行 `Install-To-OBS.ps1`，可跳過本節。

### 1. 關閉 OBS

安裝前請先完全關閉 OBS Studio（包含系統匣中的常駐程序）。

### 2. 建立外掛資料夾

開啟檔案總管，在網址欄輸入：

```
%APPDATA%\obs-studio\plugins\
```

如果 `plugins` 資料夾不存在，請手動建立。

### 3. 複製外掛檔案

建立子資料夾 `obs-relay-buffer`，結構如下：

```
%APPDATA%\obs-studio\plugins\obs-relay-buffer\
└── bin\
    └── 64bit\
        └── obs-relay-buffer.dll
```

將 `obs-relay-buffer.dll` 複製到 `bin\64bit\` 底下。

> **只需要這一個檔案。** 不需要複製任何 FFmpeg DLL。

### 4. 啟動 OBS

重新開啟 OBS Studio。如果安裝成功，Tools 選單會出現 **Relay Buffer Preview** 按鈕。

### 5. 使用

點擊 Tools → Relay Buffer Preview，會開啟獨立視窗，自動回放最近 60 秒的畫面。

> **提示**：OBS 剛啟動的前 60 秒內，緩衝區可能還沒有足夠的可解碼畫面，
> 請等 OBS 運行滿 60 秒後再開視窗。

## 疑難排解

### Tools 選單沒有出現按鈕

1. 確認 DLL 路徑正確：
   `%APPDATA%\obs-studio\plugins\obs-relay-buffer\bin\64bit\obs-relay-buffer.dll`
2. 確認 OBS 版本為 32.2.2 x64。
3. 檢查 OBS 日誌：`%APPDATA%\obs-studio\logs\`，尋找 `[obs-relay-buffer]` 相關訊息。

### 出現「avcodec 62 not found」或類似載入錯誤

- 確認 OBS 版本為 32.2.2（自帶 FFmpeg 8.x）。
- 若使用舊版 OBS，請升級 OBS，或使用對應舊版 FFmpeg 重新編譯本外掛。

### 視窗開啟後沒有畫面

- OBS 剛啟動的前 60 秒內，緩衝區可能還沒有足夠的可解碼畫面，
  請等 OBS 運行滿 60 秒後再開視窗。
- 確認 OBS 主畫面輸出正常（解析度 3840×2160 @ 60fps）。

### 播放不流暢

- 目前使用 CPU 軟解 4K 畫面，邊解碼邊播放時可能受限於 CPU 效能。
- 等背景解碼完成後重播，流暢度會改善。
- 已知瓶頸：軟解 + swscale 約 8.3 ms/幀（解碼至 1640×920）。

### 畫面出現粗糙色塊

- 確認編碼位元率是否足夠。本外掛使用動態位元率公式：
  `bitrate_bps = width * height * fps * 0.08`，4K60 約 40 Mbps。

## 除錯日誌

日誌前綴：`[obs-relay-buffer]`（Help → Log Files）

## 已知限制

- 僅支援 Windows x64。
- 僅支援 OBS 32.2.2（FFmpeg 8.x）。
- 無音訊處理。
- 無播放速度控制（僅 1x）。
- 無循環播放。
- 記憶體用量：**60 秒 @ 1640×920 YUV420P ≈ 7.8 GB**（FrameStore 全解至記憶體）。

## 卸載

刪除 `%APPDATA%\obs-studio\plugins\obs-relay-buffer\` 整個資料夾即可。

## 授權

（請依實際情況填寫）
