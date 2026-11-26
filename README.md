# 大富翁 4（Richman 4）：原生 C++ 移植

把 1998 年的《大富翁4》重写成不依赖 Windows 的原生 C++20 引擎，在 **Mac（Apple Silicon）、Android 手机与
iPhone** 上执行原版的规则、画面、动画、音乐与语音。原版的工具列、分页面板、小地图、对话气泡、银行、股市、
卡片道具、神明、新闻、每月结算、三个小游戏与结局画面都已按原版程序与素材复刻，并针对触控做了少量调整
（例如“点一下选择、再点一下确认”，以及原版只能用鼠标悬停或右键的地方）。

> **⚠️ 本仓库不含游戏素材文件**（资源包、图片、音乐、语音、执行档）；只为重现原版界面而收录了对白与名称等文字摘录，
> 其权利仍属大宇资讯。你必须自己合法拥有《大富翁4》才能执行本程序。
> 请到 Steam 购买大宇资讯官方发行的正版：
> **<https://store.steampowered.com/app/2059810/Richman_4/?l=tchinese>**
>
> 本项目与大宇资讯（SOFTSTAR ENTERTAINMENT INC.）无关。由本项目建置、内含你自己游戏素材的 APK / App
> 只限你本人在自己的装置上使用，不得散布或上架。完整说明见 **[法律声明 NOTICE.md](NOTICE.md)**；
> 第三方元件授权见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)；引擎代码为 GPL-3.0-or-later（[LICENSE](LICENSE)）。

## 致谢 / Acknowledgements

**本项目建立在 [Iru Cai](https://github.com/mytbk)（mytbk / vimacs）的 [mytbk/rich4](https://github.com/mytbk/rich4)
逆向工程之上，没有他的工作就不可能有这个移植。** 他从 2018 年起逆向《大富翁4》v3.11：找出数据文件的解压算法
与存档格式，2021 年用 [r2dumpbin](https://github.com/mytbk/r2dumpbin) 把 `rich4.exe` 转成可修改、可重新链接的
汇编，2025 年在 [rich4_asm](https://codeberg.org/vimacs/rich4_asm) 中加注解，并把 Watcom 运行库剥离到能用 MinGW
重新生成可用的游戏。本项目的每一条规则、每个画面与动画时序，都是对照他公开的反汇编（GPL-3.0）逐一重写的。
欢迎到 Matrix 聊天室 `#rich4re:matrixim.cc` 交流。

**This port is built on [Iru Cai](https://github.com/mytbk)'s reverse-engineering work in
[mytbk/rich4](https://github.com/mytbk/rich4) — it would not exist without it.** Since 2018 he decoded
Richman 4 v3.11's archive compression and save format, turned `rich4.exe` into relinkable, modifiable
assembly with r2dumpbin, annotated it in [rich4_asm](https://codeberg.org/vimacs/rich4_asm), and
separated the Watcom runtime so the game rebuilds with MinGW. Every rule, screen and animation timing here
was reimplemented by reading his published (GPL-3.0) disassembly. Thank you!

## 状态与参与

游戏**已可完整游玩**，但仍可能缺少部分原版动画序列、对白或其他游戏内容，或与原版有细微差异。欢迎提交
Pull Request；合并前必须**实际试玩并附上可重现的结果**（地图、seed、测试存档与步骤、测试报告），规则见
**[CONTRIBUTING.md](CONTRIBUTING.md)**。

| 平台 | 测试状态 |
| --- | --- |
| macOS（Apple Silicon） | ✅ 已测试 |
| Android | ✅ 真机（OnePlus 8）与模拟器 |
| iOS | ✅ 模拟器（真机尚未测试） |
| Windows | ⚠️ 尚未测试 Windows 建置 |

## 移植规模 / Migration scale

约 **15.8 万行 x86 汇编**被重写成约 **3.2 万行 C++**（游戏本身约为原来的五分之一）。统计于 2026-10-06：

| 旧代码（上游 [mytbk/rich4](https://github.com/mytbk/rich4)） | 行数 |
| --- | --- |
| `rich4.exe` 的 x86 反汇编（101 个 `.asm`） | 157,977（去除空行与注释约 147,600） |
| 上游已部分反编译的 C | 11,452 |

| 新代码（本仓库） | 行数 |
| --- | --- |
| 规则核心 `src/` + `include/`（C++20，不依赖 Windows） | 18,174（含 1,553 行生成的表） |
| 游戏应用：画面、UI、动画、音频（`tools/`，不含开发工具） | 约 15,700 |
| 测试、开发工具与对照脚本（`tests/` 9,753 + `scripts/` 3,542 等） | 约 13,300 |
| Android / iOS 外壳 | 约 770 |
| **代码合计** | **约 51,600** |
| 文档（规格、审计、迁移记录） | 约 8,900 |

去除空行与注释后，游戏本身约 3.1 万行 C++，对照约 14.8 万行汇编，约 5:1，这是从编译后的机器码回到高阶语言常见的比例。
新代码约三分之一用于验证（单元测试、自动对局、在 Wine 下执行原版做对照），也加入了原版没有的部分：触控、三个平台、新的音频管线与手机外壳。

About **158k lines of x86 assembly** were rewritten as about **32k lines of C++** for the game itself (roughly 5:1
non-blank). Including tests, tooling and the mobile shells the new code is ~51.6k lines, plus ~8.9k lines of docs.

重现统计 / Reproduce: `find .local/upstream/asm -name '*.asm' | xargs cat | wc -l`（上游需先由
`scripts/build_upstream.sh` 下载）与 `git ls-files src include tools | grep -E '\.(cpp|hpp)$' | xargs cat | wc -l`。

目前状态、已知差异与验证证据见 **[项目状态](docs/STATUS.md)**；每个原版画面的规格见 [`docs/ui/`](docs/ui/)，
素材覆盖审计见 [`docs/audit/`](docs/audit/)，规则迁移记录见 [迁移记录](docs/migrations/README.md)。

## 取得游戏素材

**建置任何平台之前**，先把游戏文件放进 `game-assets/`。详细清单、目录示意与各平台指令见
**[`game-assets/README.md`](game-assets/README.md)**。

1. 在 Steam 购买并安装《大富翁4》（或使用你已拥有的原版光盘版）。
2. 把游戏安装文件夹里的文件直接复制到本仓库根目录的 `game-assets/`（除了其中的 README，此目录内容已被 gitignore，绝不会提交）：
   - 必需：所有 `*.mkf`（`Data`、`Panel`、`map`、`Effect`、`Speaking`、`jump`、`help`）、`*.mid` 音乐、`*.wav`；
   - 建议：`rich4.exe`（只在建置时读取它的图标作为 App 图标；不会被执行，也不会打包）。
   - `*.avi` 过场不需要。
3. 本项目以原版 v3.11 光盘版的文件验证（`scripts/audit_assets.py` 可检查）。
4. Steam 版：从安装文件夹的 `MultiverseJourney/` 复制 `*.mkf`，不要用 `Game/`（`Game/` 的素材布局与 v3.11 不同）。Steam 版没有 `*.mid`，改为把 `Media/Music/track02.ogg`…`track26.ogg` 放进 `game-assets/` 作为背景音乐（目前只有 Mac 支持）。详见 [`game-assets/README.md`](game-assets/README.md#steam-版--steam-release)。

## 在 Mac 上玩

```sh
brew install cmake pkgconf sdl3 freetype fluid-synth
```

双击 **`Play Rich4 Native.command`**（或执行 `./scripts/run_native_mac.sh`）。存档在
`~/Library/Application Support/Rich4Native/saves`。空格/Enter 掷骰，F5 存档，F9 读档。
Mac 会优先使用本机 Microsoft Office 的细明体，没有时使用 Noto Serif CJK TC。

## Android（仅侧载到自己的手机）

需要 Android SDK/NDK（Android Studio 自带）、Java 17。手机打开“开发者选项 → USB 调试”，用线连接后：

```sh
~/Library/Android/sdk/platform-tools/adb devices        # 取得序号
scripts/build_android.sh <序号>                          # 建置并安装；省略序号则只建置
```

第一次建置会下载 SDL3、FreeType、FluidSynth、libiconv、GeneralUser GS 音色库与 Noto 字体（校验 SHA-256，
放在 gitignored 的 `android/third_party/`）。APK 约 190 MB（含你的游戏素材），只能侧载，**绝不要上传
Google Play 或分享给他人**。App 名称“大富翁4”，图标取自你的 `rich4.exe`。切到后台会自动写入
`Autosave.DAT`；标题画面的 EXIT 会结束 App。

## iOS（仅安装到自己的 iPhone / iPad）

需要 Xcode。模拟器：`scripts/build_ios.sh --run "iPhone 16 Pro"`。真机：

```sh
xcrun devicectl list devices                                   # 取得设备 ID
RICH4_IOS_TEAM=<10 位团队 ID> scripts/build_ios.sh --device <设备 ID>
```

iPhone 需已“信任此电脑”并开启“开发者模式”（设置 › 隐私与安全性）。免费个人团队的签名 7 天后过期，
重新执行即可。**不要上传 App Store 或 TestFlight。** 详见下文“iOS”一节。

---

以下是开发者说明。

## 编译原生核心与 Mac 查看器

```sh
brew install cmake pkgconf sdl3 freetype fluid-synth
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Debug -DRICH4_SANITIZERS=ON
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

只编译核心的环境可加 `-DRICH4_BUILD_VIEWER=OFF -DRICH4_BUILD_GAME=OFF`，核心与测试仍可单独编译。
Mac 资源查看器使用 SDL3，实际程序为 ARM64：

```sh
build/native/rich4-resource-viewer.app/Contents/MacOS/rich4-resource-viewer \
  game-assets/Data.mkf 1 0
```

左右键切换帧，PageUp/PageDown 切换资源，Escape 退出。这是资源检查工具，不是游戏本身。

命令行工具可以检查所有图片、导出资源/帧、读取并无损重写存档。例如：

```sh
build/native/rich4-tool inspect game-assets/Data.mkf
build/native/rich4-tool frame game-assets/Data.mkf 1 0 artifacts/menu.ppm
build/native/rich4-tool terrain game-assets/map.mkf 0 artifacts/taiwan.ppm
```

地面导出是按 tile 索引拼接的俯视源纹理，不是游戏中的投影相机画面。

## 原版行为对照

先按下文准备 Wine 基准环境，再生成原版结果。脚本使用独立 prefix，不操作正在玩的游戏或存档。

```sh
python3 scripts/run_asset_oracle.py --native-tool build/native/rich4-tool
cmake -S . -B build/native -DRICH4_ORACLE_DIR="$PWD/.local/oracle/latest"
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

已经捕获过原版结果时，只重做全 MKF 字节比较：

```sh
python3 scripts/run_asset_oracle.py --native-tool build/native/rich4-tool --reuse-oracle
```

金标保存在本机 `.local/oracle/latest`，清单记录 EXE、资源、适配器源文件和输出的 SHA-256。
金标不进入 Git；没有商业素材时仍可运行合成输入的普通单元测试。
详细范围与局限见 [验收记录](docs/migrations/validation.md)。

## 不占用桌面的玩法验证

已经配置 Wine 基准环境并编译原生工具后，可以自动测试真实原版的读档、掷骰、走格、电脑回合和回到玩家操作状态：

```sh
python3 scripts/run_playability.py --target wine --slot 1 --turns 3 --timeout 300
```

测试在独立的游戏副本和无窗口 Wine 进程中运行，不操作桌面鼠标/键盘。它保留事件、游戏内存快照和原版存档，再用原生 C++ 逐字节回读并交叉核对状态。报告在 `artifacts/playability/`；原有游戏、存档和配置不会被覆盖。

原生版本使用同一SDL按钮输入路径，无需桌面自动化：

```sh
python3 scripts/run_native_playability.py --turns 30 --seed 42
# 或 python3 scripts/run_playability.py --target native --turns 30
```

它明确拒绝未实现事件、没有移动、存档/随机状态不一致等情况。前后画面、逐事件/输入记录和原文件保护校验位于 `artifacts/native-playability/`。详细范围见[原生应用记录](docs/migrations/018-native-app.md)；Wine检查见[原版玩法验证](docs/migrations/009-playability-hooks.md)。

## Android ARM64 核心验证

使用本机已有 Android SDK/NDK 和已启动的 ARM64 模拟器。以下命令交叉编译同一 C++ 核心，实际在 Android 上运行测试及全部资源比较：

```sh
python3 scripts/test_android_core.py --serial emulator-5554 \
  --oracle .local/oracle/latest --game game-assets
```

这是原生库/命令行测试，不会安装 APK；完整 App 的建置见上文“Android”。报告位于 `artifacts/android-core/`。

## iOS（iPhone / iPad，仅本机安装）

`ios/` 是 CMake（Xcode 生成器，`CMAKE_SYSTEM_NAME=iOS`）工程，直接编译仓库根目录的源码，加上 SDL3 3.4.16、FreeType、FluidSynth（无 glib、无后端）；Big5 用 iOS 系统 libiconv。第三方源码由脚本下载到 `ios/third_party/`（gitignored，SHA-256 校验，版本同 Android）。`game-assets/` 的 mkf/mid/wav、GeneralUser GS 音色库与 Noto Serif CJK TC 字体在编译时复制进 app 包（不进 Git），游戏直接从包内读取；存档在 app 的 `Library/Application Support/divby0/rich4/saves`，切到后台写入 `Autosave.DAT`。图标由 `scripts/make_ios_icon.py` 从 rich4.exe 生成。Bundle id `io.divby0.rich4`，仅横屏、全屏、隐藏状态栏。**不要上传 App Store / TestFlight。**

```sh
scripts/build_ios.sh                                   # 模拟器版（ad-hoc 签名）
scripts/build_ios.sh --run "iPhone 16 Pro"             # 并在该模拟器（名称或 UDID）安装、启动
RICH4_IOS_TEAM=<团队ID> scripts/build_ios.sh --device <设备ID>   # 真机：自动签名、安装并启动
```

真机需要：10 位 Apple 团队 ID（Xcode 已登录该账号）、iPhone 用线连接或同一网络配对且已“信任此电脑”、iPhone 上开启“开发者模式”（设置 › 隐私与安全性）；设备 ID 见 `xcrun devicectl list devices`。免费个人团队签名 7 天后过期，需要重新运行。产物在 `build/ios-simulator/` 或 `build/ios-device/`，约 300 MB。

开发用真实触控测试（菜单 → 新游戏 → 棋盘 → GO、后台存档）：

```sh
cmake -G Xcode -S ios -B build/ios-uitests -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DRICH4_ASSET_CATALOG="$PWD/build/ios-generated/Assets.xcassets" \
  -DRICH4_IOS_UITESTS=ON
xcodebuild test -project build/ios-uitests/rich4_ios.xcodeproj -scheme rich4 \
  -destination 'platform=iOS Simulator,name=iPhone 16 Pro' -resultBundlePath /tmp/rich4-touch.xcresult
```

## 原版参考（Wine）

原版参考程序可双击 **`Play Rich4.command`**。也可以在终端执行：

```sh
./scripts/run_mac.sh rich4.exe
```

从源码编译的版本：

```sh
./scripts/run_mac.sh rich4_mscrt.exe
```

一次只开一个游戏进程。画面保持 640×480，使用最近邻缩放，关闭字体平滑。
Big5 保持不变。已从本机 Microsoft Word 的字体目录安装 `mingliu.ttc`，让 Wine 使用游戏要求的细明体。
该字体只复制到本机 Wine 目录，没有纳入 Git。

## 重建环境

在 macOS 上安装编译依赖后，按顺序执行。运行安装脚本前关闭 Boxedwine。

```sh
brew install mingw-w64 nasm
./scripts/build_upstream.sh
python3 scripts/setup_mac.py game-assets
./scripts/run_mac.sh rich4.exe
```

脚本固定上游源码版本及运行时校验和。默认查找已安装的 Word、Excel、PowerPoint 中的细明体，
也可使用 `--font /path/to/mingliu.ttc` 指定本机字体。找不到时采用开源 Noto Serif CJK TC；替代字体的字形不与原版完全相同。

Boxedwine 官方 Mac 应用启用了沙盒，因此游戏、字体、Wine 文件系统均放在其容器内；不需要关闭系统或应用沙盒。
原始素材文件夹保持不变。安装脚本不覆盖已有游戏素材或存档。

## 原版（Wine）存档位置

```text
~/Library/Containers/org.boxedwine/Data/Library/Application Support/Boxedwine/Rich4/game/
```

`SAVE0.DAT` 是自动存档，`SAVE1.DAT` 等是手动存档。两种 EXE 共用该目录。
当前槽位 1 是验证用对局：台湾地图，小丹尼，已买下台中土地，现金 98,000。

日志在 `artifacts/mac-runtime.log`；重新运行会覆盖上次日志。
只读素材检查：`python3 scripts/audit_assets.py`。

## 原版 Wine 基准实测（2026-10-05）

| 项目 | 结果 |
| --- | --- |
| 原版 EXE 版本匹配 | SHA-256 与上游针对的 v3.11 完全一致 |
| 上游源码构建 | MinGW + NASM 成功生成 `rich4_mscrt.exe` |
| 两种 EXE 启动 | 均显示主菜单 |
| 重编译版本基础玩法 | 选人、台湾地图、掷骰走格、银行、买地、电脑回合通过 |
| 中文字体 | Big5 + 本机细明体显示正常，恢复接近原版的细笔画 |
| 存取档 | 手动存档成功；重启后原版成功读取重编译版本存档，现金与地图状态一致 |
| 原版资源包 | 7 个 MKF，共 2,673 项；原生与原版解码结果逐字节一致，包含 462 个压缩条目 |
| 声音 | 用户实际试玩确认背景音乐和角色语音正常；日志显示音频设备与 MIDI 播放调用 |
| 原版过场 | **素材不完整**：23 个 AVI 均为相同的 5,112 字节、1 秒视频 |

这一组 Wine 基准只验证列出的基础流程，没有覆盖其他地图、全部卡片道具、小游戏或完整结局。Mac 原生版本的独立验收范围见上方和[验收记录](docs/migrations/validation.md)，不能把两套程序的证据混用。

## 素材覆盖与工具

`RICH4_ASSET_TRACE=FILE` 会记录游戏读取的每个 MKF 条目，用于找出尚未使用的原版素材（见 `docs/audit/`）。
`build/native/rich4-contact-sheet ARCHIVE FIRST LAST OUT.ppm` 把 MKF 条目画成带编号的总览图；
`build/native/rich4-tool flc` 可导出 FLC 动画帧；`rich4-native` 另有 `--gallery`、`--items-gallery`、
`--overlay-gallery`、`--monthly-gallery` 等开发用画廊参数，可一次输出各画面截图。

## 来源

- [mytbk/rich4](https://github.com/mytbk/rich4)，固定版本 `ec7d70e693059d1e781d9d1cc2ad7f36d360d39e`，GPL-3.0。
- [Boxedwine 26R1.0](https://github.com/danoon2/Boxedwine/releases/tag/26R1.0)，官方 Mac 应用；Wine 11.0 的 32 位文件系统。
- [Noto CJK](https://github.com/notofonts/noto-cjk)，手机版与缺少本机细明体时使用，SIL Open Font License 1.1。
- 其余第三方元件见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

《大富翁4》© 1998–2022 SOFTSTAR
ENTERTAINMENT INC.，请购买正版：<https://store.steampowered.com/app/2059810/Richman_4/?l=tchinese>
