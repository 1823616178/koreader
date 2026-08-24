# AI Quota — Kindle 原生移植

## 1. 项目简介

这是 KOReader 插件 `readingquota.koplugin` 的 Kindle 原生 C 移植：不依赖 KOReader，用 FBInk 直接写 framebuffer 渲染界面，提供 KUAL 菜单与书库 Scriptlet 双入口。完整的设计依据、技术选型和风险分析见仓库根目录的 [`kindle-port.zh-CN.md`](../kindle-port.zh-CN.md)。

目标设备：Kindle，固件 5.16.2.1.1（softfloat ABI），已越狱。

## 2. 目录结构

```
kindle/
  src/            C 源码：main / ui / keypad / quota / http / input / config / net / watchdog
  third_party/    vendored 依赖：cJSON（JSON 解析）、fbink.h + libfbink.a（静态链接）
  fonts/          NotoSansSC.ttf（完整字体，不入库）+ 构建时生成的子集
  extension/      KUAL 扩展，部署到 /mnt/us/extensions/aiquota/
    config.xml    KUAL 扩展声明（id 必须与文件夹名一致）
    menu.json     KUAL 菜单项
    config        运行时配置文件（key=value）
    icon.png      书库 Scriptlet 的图标
    bin/aiquota.sh  framework 接管 / 恢复 + 启动二进制
    bin/watchdog.sh 独立会话看门狗：包装脚本被杀或应用卡死时兜底恢复 framework
    bin/restore.sh  恢复逻辑单一来源：被上面两个脚本 source，也可 SSH 里直接跑
    bin/aiquota     构建产物（ARM 单二进制）
  scriptlet/      书库入口 ai-quota.sh，部署到 /mnt/us/documents/
  tests/          host 单测（test_main.c）+ 包装脚本 / 看门狗的 WSL 模拟测试
  Makefile        交叉编译入口（make test 跑 host 单测）
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

配置文件是 `/mnt/us/extensions/aiquota/config`，位于 FAT32 分区，插 USB 就能在电脑上直接编辑，改完重新打开应用生效。五个键：

| 键 | 含义 |
| --- | --- |
| `api_url` | 数据接口地址。可以写 `host`、`host:port` 或完整 URL |
| `touch_swap_xy` | 触摸坐标交换 X/Y 轴（0/1），由 `--calibrate` 确定 |
| `touch_mirror_x` | 触摸 X 轴镜像（0/1） |
| `touch_mirror_y` | 触摸 Y 轴镜像（0/1） |
| `idle_timeout_s` | 多少秒没有触摸就自动退出、交还 framework。范围 60–3600，写 0 或非法值取默认 600 |

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

把这三行抄进 `config` 即可。`--calibrate` 同样有空闲上限（它刻意走默认配置，所以固定是 600 秒）：超时没点满四个角就直接退出。

这几个调试开关都是独立运行的，不经过包装脚本，因此不会冻结 framework——屏幕上可能残留系统界面的重绘，属正常现象。正常模式请通过 KUAL / 书库入口启动，由包装脚本负责 framework 的冻结与恢复。

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
9. **空闲自动退出**：把 `idle_timeout_s` 临时改成 `60`，从 KUAL 启动后放着不动，确认一分钟后应用自己退出、framework 恢复。
10. **手动兜底演练**：从 KUAL 启动，然后在 SSH 里 `sh /mnt/us/extensions/aiquota/bin/restore.sh`，确认 framework 立刻回来。这条要在真机上验证过，出事时才敢用。
11. **看门狗强杀**：从 KUAL 启动后，SSH 里 `kill -STOP $(pidof aiquota)` 制造假死，确认约 60–70 秒后应用被 `kill -9`、framework 自动恢复。

## 9. 风险提示

最高危的一条（设计文档 §7）：**冻结 framework 之后如果程序崩溃，系统将呈现卡死状态**。为此内置了六层防御，做到任何软件层面的故障都自愈、不需要强制重启：

1. **包装脚本兜底**：`bin/aiquota.sh` 用 `trap restore_framework EXIT INT HUP` 保证应用以任何方式退出（正常关闭、崩溃、被 kill）都恢复 framework；`trap "" TERM` 挡住 KUAL 退出时的级联 SIGTERM。二进制内部的 SIGSEGV / SIGABRT handler 保证崩溃时进程立刻退出、让 trap 触发。恢复逻辑本身放在 `bin/restore.sh`，包装脚本在**冻结之前**就把它 source 进内存（trap 不能依赖再去读一次 `/mnt/us`），source 失败直接退出——绝不在没有退路的情况下冻结。
2. **OOM 优先级反转**：包装脚本和看门狗把自己的 `oom_score_adj` 设成 `-1000`（对 OOM killer 免疫），应用则设成 `+1000`（OOM killer 的第一顺位）。内存压力下死的一定是应用，第 1 层的 trap 随即恢复 framework——OOM 事件从"系统卡死"变成"应用被正常关闭"。这一条直接堵掉了 SIGKILL 唯一的现实来源。
3. **独立看门狗**：`bin/aiquota.sh` 启动应用前先用 `setsid` 拉起 `bin/watchdog.sh`（屏蔽 TERM/INT/HUP 的独立会话）。万一包装脚本仍被 SIGKILL 导致 trap 来不及执行，看门狗发现 `/proc` 里 cvm 仍处于 STOP 状态时会代为恢复。正常退出时它什么都不做。
4. **心跳防假死**：应用每 15 秒写一次 `/var/tmp/aiquota.ping`，但**心跳线程只在主线程"报备过的时间窗内"才写**——每一个会阻塞的调用（WiFi 唤醒、DNS、connect、收发、等触摸）都先用 `wd_expect()` 声明自己的上限（`src/watchdog.h`）。主线程死循环或超期不归，心跳就停。看门狗发现心跳超过 60 秒且连续两次确认后 `kill -9` 应用，第 1 层的 trap 立即恢复 framework。整个应用不再有任何无上限的等待。
5. **杀不掉就直接解冻**：`kill -9` 对 D 状态（不可中断睡眠）的进程无效，此时等包装脚本的 trap 等于永远等下去。看门狗连试两次仍杀不掉，就不再等待，自己恢复 framework 然后退出。用户立刻拿回设备，framework 会重绘盖掉卡住的应用，残留进程留到下次重启清理。
6. **空闲自动退出**：连续 `idle_timeout_s`（默认 600 秒）没有触摸，应用自己退出并交还 framework。触摸设备静默、用户走开、误触启动——这些都变成自愈而不是无限期占着冻结状态。

**手动兜底**：只要 SSH 还通，一条命令就能拿回设备：

```sh
sh /mnt/us/extensions/aiquota/bin/restore.sh
```

它是幂等的，重复执行只会重发无害的 CONT / lipc 调用；只有在 cvm 确实处于 STOP 状态时才回贴备份的画面，所以不会把旧画面闪到已经恢复的 framework 上。

六层之外只剩纯内核级故障（看门狗自己陷入 D 状态或被内核直接干掉）无法用软件覆盖；此时**长按电源键强制重启即可完全恢复**：framework 是被 SIGSTOP 冻结的，冻结状态不持久，重启后一切如常，且本应用不在系统里留任何开机项，不存在软砖风险。开发调试期间仍建议保持 USBNetwork / SSH 可用。

两个模拟测试覆盖了上述脚本逻辑，在 WSL 上直接跑（stub 掉 lipc/killall/pidof 和所有设备路径）；心跳的时间窗判定由 `make test` 里的单测覆盖：

```sh
bash kindle/tests/wrapper_sim.sh    # 包装脚本：崩溃 / 非零退出 / TERM / HUP / 缺 restore.sh
bash kindle/tests/watchdog_sim.sh   # 看门狗：心跳超时、杀不掉后的升级、OOM 兜底、幂等性
```

## 10. 与 KOReader 插件的关系

KOReader 原版插件原样保留在仓库根目录的 `readingquota.koplugin/`，继续维护、不受影响。两端共享 [`usage-api.md`](../usage-api.md) 这一份接口契约：任何一端对接口语义（`used` 恒为已用百分比、null 提供商显示「暂无数据」、45 秒超时等）的改动，另一端都要同步。
