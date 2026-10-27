# BGIplayer

使用 C++20 和 SDL2 实现的 BGI（Buriko General Interpreter）跨平台播放器，附带 Flutter 游戏库界面。引擎行为依据原程序的反编译、反汇编和运行结果逐步实现。

本仓库仅发布程序源码、必要的界面资源与构建配置，不包含游戏资源、汉化文本、原版可执行文件、存档、测试或逆向工程资料。

## 功能

- 读取 PackFile、BURIKO ARC20 等游戏资源包。
- 支持已适配的 32 位和 64 位 BGI VM、线程调度、画面合成和文字排版。
- 通过 SDL2 处理窗口、鼠标、键盘和音频输出，通过 FFmpeg 解码音视频。
- macOS 支持 Metal 图像合成，并提供 CPU 精确、GPU 精确和 GPU 快速渲染模式。
- 支持存档、压缩调试快照、鼠标事件记录与重放。
- 自动加载游戏目录中的 `translation_patch` 文本补丁。
- Flutter 界面支持游戏目录导入、游戏库、运行日志和默认字体设置。

不同游戏及引擎版本的兼容程度仍有差异。当前主要验证平台是 macOS；iOS 工程和嵌入式引擎接口也包含在源码中。

## 构建命令行引擎

依赖 C++20 编译器、Make、SDL2、FFmpeg、FreeType、zlib 和 pkg-config。macOS 构建还使用系统提供的 Metal、Cocoa、CoreText 等框架及 iconv。

在 macOS 上准备依赖后编译：

```sh
brew install sdl2 ffmpeg freetype pkg-config
make -j4
./build/bgiplayer --game "/path/to/game" --backend sdl2
```

`--game` 指向游戏根目录，资源可以位于根目录或其 `Archive/` 子目录。程序不包含游戏本身。

```sh
# 指定默认对话字体
./build/bgiplayer --game "/path/to/game" --backend sdl2 \
  --default-font "/path/to/font.ttf"

# 渲染模式
BGI_GPU=0 ./build/bgiplayer --game "/path/to/game" --backend sdl2 --render-mode exact
BGI_GPU=1 ./build/bgiplayer --game "/path/to/game" --backend sdl2 --render-mode exact
BGI_GPU=1 ./build/bgiplayer --game "/path/to/game" --backend sdl2 --render-mode fast

# 查看命令行选项
./build/bgiplayer --help
```

字体需要覆盖游戏及译文所用的字符。`BGI_CHECK_LEVEL=warn` 为默认内容校验级别，`strict` 可用于定位不一致；结构错误和越界访问不会因此被放行。

## 构建 macOS 界面

需要 Flutter SDK、Xcode 和上述原生依赖。在仓库根目录执行：

```sh
make -j4
cd src/UI
bash tool/build_macos.sh
```

输出位于 `src/UI/build/macos/Build/Products/Release/BGIplayer.app`，包含引擎及运行需要的动态库。界面通过独立 SDL 进程运行游戏，游戏库和日志使用系统 Application Support 目录。

## iOS 工程

需要 Flutter、Xcode 和 iOS 原生依赖。构建入口：

```sh
cd src/UI
bash tool/build_ios.sh simulator
# 未签名设备包
bash tool/build_ios.sh device
```

设备安装需要自行签名。移动端使用应用文档目录下的 `Games`，每个子目录作为一个游戏目录。

## 补丁与调试工具

- `tools/translation_patch.py`：资源替换补丁的生成与检查。
- `tools/compiled_text_patch.py`：已验证格式的剧情文本提取与重建。
- `tools/windows_translation_build.py` 与 `tools/windows_translation_loader.c`：为匹配的原版 PE64 程序构建 Windows 补丁加载器。需要本地原始游戏、补丁及 MinGW 工具链，不包含生成后的游戏文本。

```sh
# 记录与重放鼠标输入
./build/bgiplayer --game "/path/to/game" --backend sdl2 --record-clicks clicks.csv
./build/bgiplayer --game "/path/to/game" --backend sdl2 --replay-clicks clicks.csv
```

## 目录

- `include/`：引擎公共接口。
- `src/`：引擎和 Flutter 界面。
- `tools/`：补丁处理与 Windows 加载器源码。
- `scripts/`：默认图标嵌入脚本。
- `Makefile`：原生命令行程序构建入口。
