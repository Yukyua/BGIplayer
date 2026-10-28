# BGIplayer

BGIplayer 是使用 C++20 和 SDL2 实现的 BGI（Buriko General Interpreter）引擎移植项目，提供命令行运行器和 Flutter 游戏库界面。引擎行为依据原程序反汇编、反编译与运行对照实现，兼容工作持续进行中。

仓库提供源码，不附带游戏资源、存档、字体或汉化文本。运行时需要自行准备完整的游戏目录。

## 功能与平台

- 识别多代 32 位及 64 位 BGI 程序，解析资源归档并执行 BP 脚本。
- 绘制背景、立绘、窗口和文本，处理音频、视频、输入及存读档。
- 通过 `translation_patch` 自动加载外置文本补丁。
- Flutter 界面管理游戏目录、渲染模式和默认字体，并显示最近一次运行的终端日志。
- 提供点击事件记录与回放、压缩 VM 调试快照及按需开启的性能诊断。

| 平台 | 运行方式 | GPU 后端 |
| --- | --- | --- |
| macOS | 命令行 / Flutter App | Metal |
| iOS 15+ | Flutter App 内嵌引擎，ARM64 | Metal |
| Android 9+ | Flutter App 内嵌引擎，ARM64 | OpenGL ES 3.1 Compute |

GPU 不可用或某个操作无法加速时，使用对应 CPU 路径。不同引擎版本的兼容程度有所差异；能够启动不代表所有脚本和绘制分支都已完成验证。网络与安装器功能不属于当前实现范围。

## macOS 构建与运行

需要支持 C++20 的编译器、Make、`pkg-config`，以及 SDL2、FFmpeg、FreeType 和 zlib 开发库。Makefile 通过 `sdl2-config` 和 `pkg-config` 查找依赖；macOS 的 GPU 与字体接口使用系统框架。

使用 Homebrew 准备依赖后，在仓库根目录构建：

```sh
brew install pkg-config sdl2 ffmpeg freetype
make -j3
./build/bgiplayer --game "/path/to/game" --backend sdl2
```

`--game` 指向包含原程序及资源归档的目录。运行移植后的引擎无需启动原 Windows 程序。

构建 Flutter 桌面应用需要 Flutter SDK 和 Xcode：

```sh
bash src/UI/tool/build_macos.sh
```

生成的应用位于：

```text
src/UI/build/macos/Build/Products/Release/BGIplayer.app
```

构建脚本将引擎及其动态库打包进 App。命令行生成的 `build/bgiplayer` 仍依赖本机动态库，单独复制该文件不等于完整分发包。App 内引擎架构取决于本机引擎的构建架构。

## 移动端

### Android

准备 Flutter SDK、JDK、Android SDK、NDK `28.2.13676358` 和 CMake `3.22.1`，然后运行：

```sh
export ANDROID_SDK_ROOT="/path/to/android-sdk"
bash src/UI/tool/build_android.sh
```

APK 输出至 `src/UI/build/app/outputs/flutter-apk/app-release.apk`。当前构建目标为 `arm64-v8a`，发布构建使用本地 debug 签名配置；正式发布前需配置自己的签名。

在应用中通过系统目录选择器授权并导入游戏。导入会将整个目录复制到应用私有 `Games` 目录，运行及存档均使用该副本。从库中移除游戏会删除私有副本及其中的存档，外部原始目录不受影响。导入时需预留完整副本所需的空间。

### iOS

在安装了 Flutter SDK 和 Xcode 的 macOS 上运行：

```sh
bash src/UI/tool/build_ios.sh device
```

输出为 `src/UI/build/ios/iphoneos/BGIplayer.app` 和 `src/UI/build/ios/BGIplayer-unsigned.ipa`。真机安装前需要有效签名；未签名包不能直接安装。

将游戏放入应用文稿目录下的 `Games`，每个游戏使用独立子目录；启动时自动扫描。可以通过“文件”应用访问对应目录。iOS 与 Android 均禁止脚本切换宿主分辨率或全屏；移动端双指触控映射为右键。

## 渲染与字体

Flutter 设置提供三种渲染模式：

| 模式 | 行为 |
| --- | --- |
| CPU（精确） | 使用 CPU 合成 |
| GPU（精确） | 加速已验证的精确像素分支，其余使用 CPU |
| GPU（快速） | 在精确分支之外，允许已实现的近似 GPU 分支 |

命令行默认使用快速模式，也可显式指定：

```sh
BGI_GPU=0 ./build/bgiplayer --game "/path/to/game" --backend sdl2
BGI_GPU=1 ./build/bgiplayer --game "/path/to/game" --backend sdl2 --render-mode exact
BGI_GPU=1 ./build/bgiplayer --game "/path/to/game" --backend sdl2 --render-mode fast
```

默认 GPU 模式将位图和已呈现画面的像素保存在 GPU，按需读回；部分合成操作仍需要同步传输，性能取决于设备和场景。调试时可用 `BGI_KEEP_CPU_PIXELS=1` 保留 CPU 像素副本。

未指定字体时从系统选择可用字体。Flutter 设置可选择默认字体，命令行可传入字体文件：

```sh
./build/bgiplayer --game "/path/to/game" --backend sdl2 --default-font "/path/to/font.ttf"
```

中文和日文能否显示取决于所选字体的字形覆盖。

## 文本补丁

在游戏目录中放置 `translation_patch`，支持以下两种形式，启动时自动加载：

```text
game/
  translation_patch                 # 一个 .bgitp 格式的文件，文件名不带扩展名
```

或：

```text
game/
  translation_patch/
    text.bgitp                      # 目录中只放一个 .bgitp 文件
```

也可通过 `--translation-patch "/path/to/text.bgitp"` 指定补丁，或通过 `--no-translation-patch` 禁用自动加载。补丁须匹配原资源版本；使用中文补丁时应选择覆盖中文字形的字体。

## 调试与性能记录

默认保留 warning，详细追踪按需开启。`BGI_CHECK_LEVEL=warn` 是默认内容检查级别；`strict` 将相应内容不匹配视为错误。结构损坏、越界及其他安全检查仍会拒绝操作。

记录和回放鼠标移动、左键及右键事件：

```sh
./build/bgiplayer --game "/path/to/game" --backend sdl2 --record-clicks clicks.csv
./build/bgiplayer --game "/path/to/game" --backend sdl2 --replay-clicks clicks.csv
```

回放坐标采用游戏逻辑像素，依赖相同的游戏版本、窗口状态和存档状态。

记录每帧 VM、合成、呈现等待及像素驻留耗时：

```sh
BGI_TRACE_GPU_FRAMES=1 ./build/bgiplayer --game "/path/to/game" --backend sdl2 > run.log 2>&1
python3 tools/analyze_frame_budget.py run.log --output frame-cost
```

分析结果输出为 CSV 和 JSON。60 FPS 的每帧预算约为 16.67 ms，判断瓶颈时应区分实际生成工作与垂直同步等待。其他命令行选项可通过 `./build/bgiplayer --help` 查看。
