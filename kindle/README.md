# AI Quota — Kindle 原生移植

## 1. 项目简介

这是 AI Credits Usage API 的 Kindle 原生 C 客户端：不依赖 KOReader，用 FBInk 直接写 framebuffer 渲染 Grok、Claude Code、Codex、Kimi Code、OpenCode Go 和 Antigravity 额度，提供 KUAL 菜单与书库 Scriptlet 双入口。完整的设计依据、技术选型和风险分析见仓库根目录的 [`kindle-port.zh-CN.md`](../kindle-port.zh-CN.md)。

目标设备：Kindle，固件 5.16.2.1.1（softfloat ABI），已越狱。

## 2. 目录结构

```
kindle/
  src/            C 源码：main / ui / keypad / settings / quota / http / input / config / net / light / watchdog
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

> 完整的装机流程（含首次运行的安全顺序、故障处理、升级与卸载）见 [`install.md`](install.md)。下面是简版。

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

启动后脚本会冻结 Amazon framework（SIGSTOP），应用独占屏幕。点任意一张卡片会弹出放大框（矢量按新字号重画，不是像素放大），再点任意位置关闭——底下的仪表盘按像素还原，只有弹窗那一块闪。界面三个按钮：

- **刷新** —— 重新向配置的接口拉取数据。
- **设置** —— 打开机内设置页（见下节），接口地址 / WiFi / 自动刷新都在这里改。
- **关闭** —— 退出应用，脚本自动恢复 framework。应用**不会**自动退出：放着不动就一直常驻，直到点「关闭」或触发自愈层。

接口响应中的 `providers` 是当前采集名单；关闭的工具不会显示卡片，其余卡片会自动重新紧凑排列。需要只查询部分工具时，优先用设置页 **Providers**（写入 `show_providers`）；也会在请求 URL 上带 `?providers=`，只影响本次 Kindle 请求，不改服务端采集名单（不会调用 `PUT /api/providers`）。

**常驻与省电**：应用不再有空闲自动退出，挂着就是仪表盘。常驻期间的功耗形态：CPU 阻塞在 `poll` 等触摸（零占用，内核自动进深_idle）；射频只在拉取的几秒钟在线，拉完立即交还——且只有「应用自己开的射频」才会被关掉，进应用前就连着的 WiFi 不受影响；e-ink 靠自身稳态保持画面，不需要供电。自动刷新周期若拉回的数据与屏上完全相同、电量角标也没变，则**跳过重绘**；即使数据有变，定时刷新的重绘也走不闪屏的 GL16（手动刷新、改地址重拉才用全闪的 GC16）——夜间挂机不闪屏。挂机期间设备**不会休眠也不会锁屏**——包装脚本持有 `preventScreenSaver` 并抓取内核 wake lock（`/sys/power/wake_lock`），应用在挂机等待的分段间隙还会每 3–5 分钟续期一次 screensaver 保持，三层都在挡 powerd 的空闲计时。代价是 SoC 待机电流成为常驻底噪，挂机数天请按这个估算电量；**想让设备正常息屏，先点「关闭」**。标题栏右上角状态角标：WiFi 圆点（实心点=已连接，灰点=已断开，拉取后变灰即代表射频已经归还）、已连接时显示当前网络名（SSID，仅限 ASCII 名称，中文名显示为 WiFi）、五格电池条（充电中左侧带「充电」二字；0 格且未充电时整体反显作为低电量告警）。角标只在启动、拉取、设置返回这些既有重绘点采样一次，另外闲置熄灯到期时会顺手再采一次电量——格数或充电态变了才对那一小块发 DU，不整屏闪、不加新定时器。电量先走各机型 sysfs（yoshi / wario / power_supply），读不到再问 `com.lab126.powerd` 的 `battLevel` / `isCharging`——和前光同一条 lipc，PW2 及以后不再因为路径叫 `yoshi_battery` 而整块角标消失。SSID 只在射频已醒着时探测一次。自动刷新跳帧比的是格数而不是原始百分比，所以 87→86 这种一格之内的掉电不会逼出一次看不见的重绘。精确百分比仍在 `--dump-state` 的 `battery_level=` 里。标题栏副标题下还有一行死文字：`自动刷新 5 分钟` / `自动刷新关闭`，常驻时一眼能看出页面会不会自己更新。

**前光与闲置熄灯**：前光是挂机期间最大的可控功耗。仪表盘无触摸 **3 分钟**后前光自动降到 0 并记住原档位，下一次触摸**立即恢复**（同一次触摸照常参与按钮命中，不是「只唤醒」）；设置页 / 键盘打开期间不算闲置。卡片放大框开着时闲置熄灯照常计时（点一下同时恢复前光并关闭弹窗）。在设置页手动关掉的灯不会被闲置逻辑打开；点「关闭」退出时，若灯仍处于闲置熄灭状态会先还原到原档位再退出（手动关过的保持关）。熄灯过程不闪屏，恢复也不闪屏。

### 设置页

| 行 | 值 | 点按行为 |
| --- | --- | --- |
| 接口地址 | 当前地址的 host 部分 | 弹数字键盘编辑完整 URL |
| 刷新后关WiFi | 开 / 关 | 直接切换。关掉后拉取完成的 WiFi 保持连接 |
| 自动刷新 | 关闭 / N 分钟 | 弹键盘输入，0=关闭、1–60 分钟。到点自动开 WiFi 拉一次再关 |
| Idle light | 关闭 / N 分钟 | 闲置熄灯分钟数；0=关闭该功能 |
| Display | API / used / remain / large / compact | 循环切换本机显示偏好（large/compact 已存键，布局仍为常规卡片） |
| Providers | N / 6 | 进入子页：逐个 ON/off，`^`/`v` 调整顺序；至少保留 1 个。请求带 `?providers=`，**不**调用 `PUT /api/providers` |
| Diagnose | run | 可取消的连接探测；多行展示 WiFi/DNS/connect/HTTP/JSON 阶段；失败不覆盖上次成功额度 |
| 前光 | 开 / 关 · N | 一行合并：**左半边**点按切换开关——关掉记住档位、再开恢复（与仪表盘闲置熄灯共用记忆档位）；**右侧** `−` / `+` 步进档位，每点一档立刻发给 powerd、DU 局部刷新不闪页。减到 0 即关灯，关着时按 `+` 从记忆档位打开。范围 0–`flMaxIntensity`（常见 24 或 25） |

接口 / WiFi / 自动刷新在点「返回」时写入配置文件并立即生效；改接口地址会顺带重新拉取一次。前光走 `com.lab126.powerd`，立刻改整机前光，**不写**配置文件，退出应用后 Kindle 系统里仍是这个亮度。读不到 powerd 时前光行显示 `--`，点按无效果。自动刷新按**上次拉取完成的时间**排程：设置页 / 键盘 / 卡片放大框打开期间到点的刷新会跳过，回到仪表盘后立即补一次；拉回数据与屏上相同且电池格数、充电态都没变的那次刷新不闪屏。设置页打开期间闲置熄灯计时暂停，返回仪表盘后重新计 3 分钟。从设置页返回时：改了接口地址仍是一次整屏 GC16 并重拉；只调了前光或只改了配置键则用 GL16 轻刷恢复仪表盘，不再整屏闪白。

## 6. 配置

配置文件是 `/mnt/us/extensions/aiquota/config`，位于 FAT32 分区，插 USB 就能在电脑上直接编辑；应用内的「设置」页改的就是同一份文件，改完即存。可靠保存会附带 `config.bak`。主要键：

| 键 | 含义 |
| --- | --- |
| `api_url` | 数据接口地址。可以写 `host`、`host:port` 或完整 URL |
| `touch_swap_xy` | 触摸坐标交换 X/Y 轴（0/1），由 `--calibrate` 确定 |
| `touch_mirror_x` | 触摸 X 轴镜像（0/1） |
| `touch_mirror_y` | 触摸 Y 轴镜像（0/1） |
| `wifi_auto_off` | 拉取完成后是否关掉「由应用自己开启的」WiFi（默认 1） |
| `auto_refresh_min` | 自动刷新间隔（分钟），默认 0=关闭，范围 1–60 |
| `light_idle_off_min` | 闲置熄灯分钟数；0=关闭；默认 3 |
| `display_mode_pref` | 0=API，1=used，2=remain，3=large，4=compact |
| `refresh_preset` | 0=custom，1=manual，2=normal(5 分钟)，3=night(30 分钟)；助手函数可映射，设置页仍以分钟输入为主 |
| `show_providers` | 本机有序名单，如 `grok,claude,agy`；拉额度时合并为 `?providers=`，不改服务端全局采集名单 |
| `config_version` | 配置架构版本；启动时旧版本会迁移 |

旧版本配置文件里的 `idle_timeout_s` 行会被当作未知键忽略，下次从设置页保存配置时自动删除。

`api_url` 会做归一化：没有 scheme 就补 `http://`、去掉路径尾部斜杠、路径结尾不是 `/api/usage` 就补上，并原样保留非 providers 类 query。本机 `show_providers` 在每次请求时覆盖 URL 里旧的 `providers`/`tools` 参数。首次无配置文件时会引导进入设置页。

## 7. 调试开关

三个开关都在 SSH 里直接跑二进制使用（`/mnt/us/extensions/aiquota/bin/aiquota --xxx`）：

- `--dump-state` —— 打印 `fbink_get_state()` 的全部字段（分辨率、DPI、旋转、bpp 等），末尾附 `battery_level=` / `battery_charging=`（sysfs 多路径，失败则问 powerd；-1 表示该设备读不到，界面上也就不显示电量）以及 `frontlight_intensity=` / `frontlight_max=`（powerd lipc；-1 表示读不到）。用来核对设备实际参数。
- `--demo` —— 用内置的示例 JSON（[`docs/superpowers/plans/usage-api.md`](../docs/superpowers/plans/usage-api.md) 里的完整示例）渲染界面，不联网，用来验证 8 张卡片、月额度、布局和字体。
- `--calibrate` —— 触摸标定。按提示依次点屏幕四角，程序根据落点推导坐标变换，最后直接输出建议的配置行：

```
suggested config lines:
touch_swap_xy=0
touch_mirror_x=0
touch_mirror_y=0
```

把这三行抄进 `config` 即可。`--calibrate` 每个角有固定的 600 秒上限：超时没点满四个角就直接退出，避免没人接手的标定会话无限占屏。

这几个调试开关都是独立运行的，不经过包装脚本，因此不会冻结 framework——屏幕上可能残留系统界面的重绘，属正常现象。正常模式请通过 KUAL / 书库入口启动，由包装脚本负责 framework 的冻结与恢复。

## 8. 机上验证清单

来自设计文档 §8，逐条操作：

1. **二进制验证**：WSL 里 `file kindle/extension/bin/aiquota` 应输出 `ARM ... EABI5 ... soft-float`；`readelf -d` 的 NEEDED 里不应出现 libfbink（`tools/build.sh` 已自动校验这两项）。
2. **dump-state**：SSH 里跑 `aiquota --dump-state`，输出的分辨率 / 旋转应与设备实际一致；`battery_level` 应为 0–100 的真实电量（插 USB 时 `battery_charging=1`）。
3. **calibrate**：SSH 里跑 `aiquota --calibrate`，依次点四角，确认十字线落点准确；把输出的三行建议参数写进 `config`。
4. **demo 检查**：SSH 里跑 `aiquota --demo`，确认 4×2 卡片全部在屏内、底部按钮可见，标题栏右上角五格电池条正常（充电时左侧带「充电」、未充电且 0 格时反显）；点任意卡片弹出放大框、再点任意位置关闭，底下仪表盘应逐像素还原；Grok / OpenCode 的月额度和其他提供商的采集时间正常显示；再改 demo JSON 造出「某提供商为 null」「超过 alertPercent」和 OpenCode `rate-limited` 三种情况，确认「暂无数据」、`!` 和「已限流」都对。
5. **真实拉取与错误路径**：配好真实 `api_url` 跑一次联网拉取；再把 URL 改成不可达地址，确认界面走错误卡片而不是崩溃。
6. **KUAL 入口完整交互**：从 KUAL 启动 → 完成「改地址 → 刷新 → 关闭」一轮 → **确认 framework 完全恢复**（能翻书、能进设置、能息屏唤醒）。这是整个移植的关键验收点。
7. **书库 Scriptlet 入口**：从书库图标启动，重复第 6 步。
8. **断电重启**：长按电源完整重启一次，确认没有残留的冻结进程或被改动的 lipc 状态。
9. **常驻不退出**：从 KUAL 启动后放着不动超过 10 分钟，确认应用仍在、framework 仍冻结、点按钮仍有响应——自动退出已移除，「关闭」是唯一的正常出口。
10. **手动兜底演练**：从 KUAL 启动，然后在 SSH 里 `sh /mnt/us/extensions/aiquota/bin/restore.sh`，确认 framework 立刻回来。这条要在真机上验证过，出事时才敢用。
11. **看门狗强杀**：从 KUAL 启动后，SSH 里 `kill -STOP $(pidof aiquota)` 制造假死，确认约 2–2.5 分钟后应用被 `kill -9`、framework 自动恢复（心跳 30 秒一次 + 看门狗连续两次确认）。
12. **设置页持久化**：KUAL 启动 → 设置 → 改自动刷新为 5 分钟、关掉「刷新后关WiFi」→ 返回 → 退出应用重新进入，确认值都还在；USB 连电脑 diff 一下 `config` 文件应出现这两行。
13. **WiFi 所有权**：先在系统里断开 WiFi 再进应用拉取一次，SSH 里 `lipc-get-prop com.lab126.wifid cmState` 应回到非 CONNECTED；反过来先连好 WiFi 再进应用拉取，拉完 cmState 应仍是 CONNECTED。
14. **自动刷新与跳闪**：设置 `auto_refresh_min=1` 后开着页面放两分钟：第一分钟数据有变化应整屏更新、且射频窗口只有几秒；服务端数据不变的第二分钟应**不闪屏**（可关灯观察）。再开着设置页跨过一个刷新点到返回，确认返回后立即补一次拉取。

## 9. 风险提示

最高危的一条（设计文档 §7）：**冻结 framework 之后如果程序崩溃，系统将呈现卡死状态**。为此内置了五层防御，做到任何软件层面的故障都自愈、不需要强制重启：

1. **包装脚本兜底**：`bin/aiquota.sh` 用 `trap restore_framework EXIT INT HUP` 保证应用以任何方式退出（正常关闭、崩溃、被 kill）都恢复 framework；`trap "" TERM` 挡住 KUAL 退出时的级联 SIGTERM。二进制内部的 SIGSEGV / SIGABRT handler 保证崩溃时进程立刻退出、让 trap 触发。恢复逻辑本身放在 `bin/restore.sh`，包装脚本在**冻结之前**就把它 source 进内存（trap 不能依赖再去读一次 `/mnt/us`），source 失败直接退出——绝不在没有退路的情况下冻结。
2. **OOM 优先级反转**：包装脚本和看门狗把自己的 `oom_score_adj` 设成 `-1000`（对 OOM killer 免疫），应用则设成 `+1000`（OOM killer 的第一顺位）。内存压力下死的一定是应用，第 1 层的 trap 随即恢复 framework——OOM 事件从"系统卡死"变成"应用被正常关闭"。这一条直接堵掉了 SIGKILL 唯一的现实来源。
3. **独立看门狗**：`bin/aiquota.sh` 启动应用前先用 `setsid` 拉起 `bin/watchdog.sh`（屏蔽 TERM/INT/HUP 的独立会话）。万一包装脚本仍被 SIGKILL 导致 trap 来不及执行，看门狗发现 `/proc` 里 cvm 仍处于 STOP 状态时会代为恢复。正常退出时它什么都不做。
4. **心跳防假死**：应用每 30 秒写一次 `/var/tmp/aiquota.ping`，但**心跳线程只在主线程"报备过的时间窗内"才写**——每一个会阻塞的调用（WiFi 唤醒、DNS、connect、收发、等触摸）都先用 `wd_expect()` 声明自己的上限（`src/watchdog.h`）。主线程死循环或超期不归，心跳就停。看门狗发现心跳超过 120 秒且连续两次确认后 `kill -9` 应用，第 1 层的 trap 立即恢复 framework。整个应用不再有任何无上限的等待。
5. **杀不掉就直接解冻**：`kill -9` 对 D 状态（不可中断睡眠）的进程无效，此时等包装脚本的 trap 等于永远等下去。看门狗连试两次仍杀不掉，就不再等待，自己恢复 framework 然后退出。用户立刻拿回设备，framework 会重绘盖掉卡住的应用，残留进程留到下次重启清理。

历史上的第 6 层「空闲自动退出」已按常驻仪表盘的需求移除。**它的兜底职责随之消失**：触摸标定不对、点什么都没反应的场景不再会自己恢复——因为应用是活的、心跳正常，看门狗不会介入。此时只有两条路：SSH 里跑下面的 `restore.sh`，或长按电源键强制重启。第一次装机务必先完成 §4.3 的触摸标定，并保持 SSH 可用。

**手动兜底**：只要 SSH 还通，一条命令就能拿回设备：

```sh
sh /mnt/us/extensions/aiquota/bin/restore.sh
```

它是幂等的，重复执行只会重发无害的 CONT / lipc 调用；只有在 cvm 确实处于 STOP 状态时才回贴备份的画面，所以不会把旧画面闪到已经恢复的 framework 上。

五层之外只剩纯内核级故障（看门狗自己陷入 D 状态或被内核直接干掉）无法用软件覆盖；此时**长按电源键强制重启即可完全恢复**：framework 是被 SIGSTOP 冻结的，冻结状态不持久，重启后一切如常，且本应用不在系统里留任何开机项，不存在软砖风险。开发调试期间仍建议保持 USBNetwork / SSH 可用。

两个模拟测试覆盖了上述脚本逻辑，在 WSL 上直接跑（stub 掉 lipc/killall/pidof 和所有设备路径）；心跳的时间窗判定由 `make test` 里的单测覆盖：

```sh
bash kindle/tests/wrapper_sim.sh    # 包装脚本：崩溃 / 非零退出 / TERM / HUP / 缺 restore.sh
bash kindle/tests/watchdog_sim.sh   # 看门狗：心跳超时、杀不掉后的升级、OOM 兜底、幂等性
```

## 10. 与 KOReader 插件的关系

KOReader 原版插件原样保留在仓库根目录的 `readingquota.koplugin/`，继续维护、不受影响。Kindle 端当前以 [`docs/superpowers/plans/usage-api.md`](../docs/superpowers/plans/usage-api.md) 为接口契约，并兼容 OpenCode 的 `five` / `seven` 别名；接口后续增加字段时，解析器会忽略未知键。
