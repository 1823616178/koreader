# AI Quota — Kindle 原生移植

## 1. 项目简介

这是 KOReader 插件 `readingquota.koplugin` 的 Kindle 原生 C 移植：不依赖 KOReader，用 FBInk 直接写 framebuffer 渲染界面，提供 KUAL 菜单与书库 Scriptlet 双入口。完整的设计依据、技术选型和风险分析见仓库根目录的 [`kindle-port.zh-CN.md`](../kindle-port.zh-CN.md)。

目标设备：Kindle，固件 5.16.2.1.1（softfloat ABI），已越狱。

## 2. 目录结构

```
kindle/
  src/            C 源码：main / ui / keypad / quota / http / input / config / net
  third_party/    vendored 依赖：cJSON（JSON 解析）、fbink.h + libfbink.a（静态链接）
  fonts/          NotoSansSC.ttf（完整字体，不入库）+ 构建时生成的子集
  extension/      KUAL 扩展，部署到 /mnt/us/extensions/aiquota/
    config.xml    KUAL 扩展声明（id 必须与文件夹名一致）
    menu.json     KUAL 菜单项
    config        运行时配置文件（key=value）
    icon.png      书库 Scriptlet 的图标
    bin/aiquota.sh  framework 接管 / 恢复 + 启动二进制
    bin/aiquota     构建产物（ARM 单二进制）
  scriptlet/      书库入口 ai-quota.sh，部署到 /mnt/us/documents/
  Makefile        交叉编译入口
tools/
  build.sh        一键：编译 + 裁字体子集 + 校验 + 打包 dist/aiquota-kindle.tar.gz
```

## 3. 构建

全程在 WSL（Ubuntu 24.04）里进行，仓库路径 `/mnt/g/code/koread_ext`。

**第一步：交叉工具链（一次性，约 30–60 分钟）。** 拉 [koreader/koxtoolchain](https://github.com/koreader/koxtoolchain)，跑 `./gen-tc.sh kindlepw2`，产物在 `~/x-tools/arm-kindlepw2-linux-gnueabi`。注意：koxtoolchain 在 Ubuntu 24.04 上需要 texinfo ≥ 7.2——系统自带的 7.1 会在 glibc 手册构建阶段崩溃（见 koreader/koxtoolchain issue #47），先用更新版本的 texinfo 再跑。

**第二步：FBInk 静态库（一次性）。** 拉 [NiLuJe/FBInk](https://github.com/NiLuJe/FBInk)，**必须用 master commit 886f25f1**——release v1.25.0 还没有 `fbink_fill_rect_gray()`。构建命令：

```sh
make staticlib KINDLE=true MINIMAL=true DRAW=true OPENTYPE=true CROSS_TC=arm-kindlepw2-linux-gnueabi
```

产物 `Release/libfbink.a` 和 `fbink.h` 拷到 `kindle/third_party/fbink/`。

**第三步：字体与 fonttools。** 完整字体 `NotoSansSC.ttf` 放到 `kindle/fonts/`（8.3 MB，不入库）；准备一个装了 fonttools 的 python venv 在 `~/fontenv`。

**第四步：一键构建打包。**

```sh
./tools/build.sh
```

它会依次：交叉编译 `kindle/extension/bin/aiquota` → 从 `kindle/src/*.c` 提取出现的非 ASCII 字符 → `pyftsubset` 裁出 `NotoSansSC-subset.ttf`（约 24 KB）→ 校验二进制是 ARM 且没有动态链接 libfbink → 组装 `dist/aiquota/` → 打出 `dist/aiquota-kindle.tar.gz`。

## 4. 部署

前提：Kindle 已越狱，装好 KUAL 和 SH_Integration（Scriptlet 支持）。

1. USB 连接，把 `dist/aiquota-kindle.tar.gz` 拷到 Kindle 的 USB 根目录。
2. 在设备上（SSH 进去）解压到 `/mnt/us`：

```sh
tar -xzf /mnt/us/aiquota-kindle.tar.gz -C /mnt/us
```

解压后得到 `extensions/aiquota/`（KUAL 扩展本体）和 `documents/ai-quota.sh`（书库 Scriptlet）。安全弹出 USB，KUAL 菜单和书库里就会出现入口。

## 5. 使用

两个入口任选：

- **KUAL**：菜单「AI Quota → AI 额度」。
- **书库**：首页里的「AI 额度」图标（SH_Integration 把 `documents/ai-quota.sh` 索引成一本书）。

启动后脚本会冻结 Amazon framework（SIGSTOP），应用独占屏幕。界面三个按钮：

- **刷新** —— 重新向配置的接口拉取数据。
- **接口地址** —— 弹出数字键盘修改 API 地址。键盘只有 `0-9 . :`（外加退格 / 确定 / 取消），面向 `IP:端口` 形式的地址；要用域名或带路径的地址，请通过 USB 直接改配置文件（见下节）。
- **关闭** —— 退出应用，脚本自动恢复 framework。

## 6. 配置

配置文件是 `/mnt/us/extensions/aiquota/config`，位于 FAT32 分区，插 USB 就能在电脑上直接编辑，改完重新打开应用生效。四个键：

| 键 | 含义 |
| --- | --- |
| `api_url` | 数据接口地址。可以写 `host`、`host:port` 或完整 URL |
| `touch_swap_xy` | 触摸坐标交换 X/Y 轴（0/1），由 `--calibrate` 确定 |
| `touch_mirror_x` | 触摸 X 轴镜像（0/1） |
| `touch_mirror_y` | 触摸 Y 轴镜像（0/1） |

`api_url` 会做归一化（与 KOReader 版 `quotadata.lua` 同逻辑）：没有 scheme 就补 `http://`、去掉尾部斜杠、结尾不是 `/api/usage` 就补上。例如写 `123.161.179.32:46927` 等价于 `http://123.161.179.32:46927/api/usage`。

## 7. 调试开关

三个开关都在 SSH 里直接跑二进制使用（`/mnt/us/extensions/aiquota/bin/aiquota --xxx`）：

- `--dump-state` —— 打印 `fbink_get_state()` 的全部字段（分辨率、DPI、旋转、bpp 等），用来核对设备实际参数。
- `--demo` —— 用内置的示例 JSON（`usage-api.md` 里的完整示例）渲染界面，不联网，用来验证布局和字体。
- `--calibrate` —— 触摸标定。按提示依次点屏幕四角，程序根据落点推导坐标变换，最后直接输出建议的配置行：

```
suggested config lines:
touch_swap_xy=0
touch_mirror_x=0
touch_mirror_y=0
```

把这三行抄进 `config` 即可。这几个调试开关都是独立运行的，不经过包装脚本，因此不会冻结 framework——屏幕上可能残留系统界面的重绘，属正常现象。正常模式请通过 KUAL / 书库入口启动，由包装脚本负责 framework 的冻结与恢复。

## 8. 机上验证清单

来自设计文档 §8，逐条操作：

1. **二进制验证**：WSL 里 `file kindle/extension/bin/aiquota` 应输出 `ARM ... EABI5 ... soft-float`；`readelf -d` 的 NEEDED 里不应出现 libfbink（`tools/build.sh` 已自动校验这两项）。
2. **dump-state**：SSH 里跑 `aiquota --dump-state`，输出的分辨率 / 旋转应与设备实际一致。
3. **calibrate**：SSH 里跑 `aiquota --calibrate`，依次点四角，确认十字线落点准确；把输出的三行建议参数写进 `config`。
4. **demo 对照**：SSH 里跑 `aiquota --demo`，对照仓库根目录的 `preview-partial.png` 检查布局；再改 demo JSON 造出「某提供商为 null」和「超过 alertPercent」两种情况，确认「暂无数据」和 `!` 告警标记都对。
5. **真实拉取与错误路径**：配好真实 `api_url` 跑一次联网拉取；再把 URL 改成不可达地址，确认界面走错误卡片而不是崩溃。
6. **KUAL 入口完整交互**：从 KUAL 启动 → 完成「改地址 → 刷新 → 关闭」一轮 → **确认 framework 完全恢复**（能翻书、能进设置、能息屏唤醒）。这是整个移植的关键验收点。
7. **书库 Scriptlet 入口**：从书库图标启动，重复第 6 步。
8. **断电重启**：长按电源完整重启一次，确认没有残留的冻结进程或被改动的 lipc 状态。

## 9. 风险提示

最高危的一条（设计文档 §7）：**冻结 framework 之后如果程序崩溃，系统将呈现卡死状态**。缓解措施已经内置——包装脚本 `bin/aiquota.sh` 用 `trap restore_framework EXIT INT HUP` 保证无论以何种方式退出都会恢复 framework，二进制内部也装了 SIGSEGV / SIGABRT handler 先恢复再退出。但开发调试期间请始终保持 USBNetwork / SSH 可用作为兜底。

如果真的遇到 KUAL 启动后卡死，**长按电源键强制重启即可完全恢复**：framework 是被 SIGSTOP 冻结的，不是被 stop，冻结状态不持久，重启后一切如常。

## 10. 与 KOReader 插件的关系

KOReader 原版插件原样保留在仓库根目录的 `readingquota.koplugin/`，继续维护、不受影响。两端共享 [`usage-api.md`](../usage-api.md) 这一份接口契约：任何一端对接口语义（`used` 恒为已用百分比、null 提供商显示「暂无数据」、45 秒超时等）的改动，另一端都要同步。
