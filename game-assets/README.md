# game-assets/ — 放你自己的《大富翁4》遊戲檔案 / Put your own Richman 4 game files here

**這個資料夾除了本說明外不會進入 Git**（見根目錄 `.gitignore`）。請勿把遊戲檔案提交、上傳或分享。
**Everything in this folder except this README is gitignored.** Never commit, upload or share game files.

本專案不提供遊戲檔案。請購買正版 / This project does not provide game files. Please buy the game:
<https://store.steampowered.com/app/2059810/Richman_4/?l=tchinese>
（法律聲明 / legal notice: [`../NOTICE.md`](../NOTICE.md)）

## 要放哪些檔案 / Which files

從你的遊戲安裝資料夾（Steam：在遊戲庫對《大富翁4》按右鍵 → 管理 → 瀏覽本機檔案）複製到這裡，
直接放在 `game-assets/` 底下，不要再包一層資料夾。檔名大小寫不拘。
Copy these from your installation folder (Steam: right-click Richman 4 in the Library → Manage → Browse
local files) directly into `game-assets/`, not into a subfolder. File name case does not matter.

| 檔案 / Files | 必要性 / Needed | 用途 / Purpose |
| --- | --- | --- |
| `Data.mkf`、`Panel.mkf`、`map.mkf`、`Effect.mkf`、`Speaking.mkf`、`jump.mkf`、`help.mkf` | **必要 / required** | 圖片、動畫、地圖、音效、語音、說明 / graphics, movies, maps, sound effects, voices, help |
| `*.mid`（`midi01.mid`…、`Rich08.mid`…`Rich22.mid`） | **必要 / required** | 背景音樂 / music |
| `track02.ogg`…`track26.ogg`（Steam 版 `Media/Music/`） | Steam 版代替 `*.mid` / Steam release instead of `*.mid` | Steam 版沒有 MIDI，背景音樂改用這些 Ogg；只有 Mac 建置支援 / the Steam release has no MIDIs; its Ogg tracks are used instead (Mac build only) |
| `*.wav` | 建議 / recommended | 安裝程式音效（目前未使用）/ installer sounds (not used yet) |
| `rich4.exe` | 建議 / recommended | 只在建置 Android / iOS 時讀取它的圖示做 App 圖示；不會執行、不會打包 / read at build time for the app icon only; never run or packaged |
| `*.avi` | 不需要 / not needed | 原版過場（本專案未使用）/ cutscenes (not used) |

完成後應該像這樣 / When done it should look like:

```text
game-assets/
├── README.md        ← 本檔 / this file (tracked)
├── Data.mkf
├── Effect.mkf
├── help.mkf
├── jump.mkf
├── map.mkf
├── Panel.mkf
├── Speaking.mkf
├── midi01.mid … midi16.mid, Rich08.mid … Rich22.mid
└── rich4.exe        ← 選用 / optional (app icon)
```

檢查 / Check: `python3 scripts/audit_assets.py`

本專案以原版 v3.11 光碟版的檔案驗證；Steam 版的檔案尚未逐一比對，如有差異請回報。
Verified with the original v3.11 CD release; the Steam release's files have not been compared yet —
please report any differences.

## 接著建置 / Then build

所有建置腳本預設從這個資料夾讀取（也可用 `RICH4_ASSETS` / `RICH4_GAME_ASSETS` 指向別處）。
All build scripts read from this folder by default (override with `RICH4_ASSETS` / `RICH4_GAME_ASSETS`).

| 平台 / Platform | 指令 / Command | 測試狀態 / Tested |
| --- | --- | --- |
| macOS (Apple Silicon) | 雙擊 `Play Rich4 Native.command`，或 `./scripts/run_native_mac.sh` | ✅ 已測試 / tested |
| Android | `scripts/build_android.sh <adb 序號 / serial>` | ✅ 真機（OnePlus 8）與模擬器 / device (OnePlus 8) and emulator |
| iOS | `scripts/build_ios.sh --run "iPhone 16 Pro"`（模擬器 / simulator）；`RICH4_IOS_TEAM=<team> scripts/build_ios.sh --device <id>`（真機 / device） | ✅ 模擬器 / simulator；真機尚未測試 / device not yet tested |
| Windows | — | ⚠️ 尚未測試 Windows 建置 / Windows builds have not been tested |

詳細步驟見根目錄 [`README.md`](../README.md)。 / Full steps: root [`README.md`](../README.md).
