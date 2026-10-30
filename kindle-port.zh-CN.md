# Kindle 原生移植：可行性与设计文档

*目标：把现在这个 KOReader 插件改造成在 Kindle 上独立运行的原生应用，用 FBInk 渲染、KUAL 启动、并作为一个独立入口出现在书库里，不依赖 KOReader。*

**目标设备：** Kindle，固件 5.16.2.1.1，已越狱。

---

## 1. 可行性结论

**可行，没有阻断性技术风险。**

三条理由：

**其一，架构是社区走熟的路。** 「KUAL 启动 + FBInk 直接写 framebuffer + 冻结 Amazon framework」不是新发明，已有多个在跑的先例：

- [OpenReader](https://github.com/terpinedream/OpenReader/) —— 用 fbink 渲染，`SIGSTOP` 冻结 Amazon 服务，自己写 `touch_reader.c` 读 `/dev/input/event1`。
- [trmnl-kindle](https://github.com/usetrmnl/trmnl-kindle/) —— KUAL 扩展形态的全屏仪表盘。
- [gambatte-k2](https://github.com/crazy-electron/gambatte-k2) —— GTK2 + FBInk 的灰阶渲染。
- **KOReader 自己** —— 它在 Kindle 上就是从 KUAL 启动、停掉 framework、独占屏幕的。它的 `platform/kindle/koreader.sh` 是这套流程最完整的参考实现。

**其二，现有代码到 Kindle 的映射几乎是一一对应的。** 见下一节的映射表——这是本次移植最有利的因素。现有 Lua 代码里没有任何依赖 KOReader 独有抽象的地方，所有 UI 原语在 libfbink 里都有直接对应物。

**其三，固件版本落在成熟区间。** 5.16.2.1.1 **小于 5.16.3**，属于 softfloat ABI，用传统的 `kindlepw2` 工具链即可，不需要碰 5.16.3+ 才引入的 kindlehf 那套较新的硬浮点产物。社区预编译的 fbink 也能直接用。

**已敲定的技术栈：C 单二进制 + libfbink（静态链接）+ 自绘数字网格键盘 + KUAL 菜单与书库 Scriptlet 双入口。**

---

## 2. 现有代码到 Kindle 的映射

这是可行性的最强论据。现有实现的每一个部件都有明确落点：

| 现有 Lua | Kindle 端对应物 |
| --- | --- |
| `quotabar.lua:46-61` 的 `bb:paintRect()` 分段循环 | `fbink_fill_rect_gray()`，pitch / 取整算法原样搬 |
| `TextWidget` + `Font:getFace("cfont", n)` | `fbink_add_ot_font()` + `fbink_print_ot()` |
| `Screen:scaleBySize()` | 基于 `fbink_get_state()` 返回的 dpi 自定义缩放宏 |
| `UIManager:setDirty()` 的批量重绘 | 全程 `FBInkConfig.no_refresh = true`，末尾一次 `fbink_refresh_rect()` |
| `Blitbuffer.COLOR_BLACK / GRAY / LIGHT_GRAY` | `fbink_fill_rect_gray()` 的 8-bit 灰阶值 |
| `quotapage.lua:39-71` 的尺寸算式 | 同样的算式，改用整型像素运算 |
| `quotapage.lua:221-234` 的 `ButtonTable` | 自绘按钮 + 矩形命中区表 |
| `quotadata.lua:87` 的 `socketutil:set_timeout(45, 50)` | `SO_RCVTIMEO` / `SO_SNDTIMEO`，**必须保留** |
| `socket.http` | 手写 HTTP/1.0 GET over TCP |
| `rapidjson.decode` | vendored cJSON |
| `quotadata.lua:49-62` 的 `normalizeUrl` | 同逻辑的 C 实现 |
| `LuaSettings`（`settings/readingquota.lua`） | `key=value` 纯文本配置文件 |
| `InputDialog` + KOReader 屏幕键盘 | **自绘数字网格键盘**（见 §4） |
| `Device.input.group.Back` / MultiSwipe 关闭 | evdev 按键 + 触摸手势 |

超时那一条特别标注一下：`quotadata.lua` 里 45/50 秒的超时不是随便写的，`usage-api.md` 明确说明 Kimi 和 AGY 首次请求需要临时启动本地服务，客户端超时建议至少 45 秒。C 版本必须原样保留这个值，否则首次使用会稳定失败。

---

## 3. 技术选型与被排除的方案

### 选定：C 单二进制 + libfbink

单进程内同时完成绘制、evdev 触摸读取和 HTTP 请求。所有绘制调用带 `no_refresh`，最后统一刷新一次，没有中间态闪烁。静态链接 libfbink 后是一个自包含的可执行文件，预计 300–600 KB。

libfbink 提供的 API 恰好覆盖全部需求：

- 初始化：`fbink_open()` / `fbink_init()` / `fbink_get_state()`
- 字体：`fbink_add_ot_font()` / `fbink_print_ot()` / `fbink_free_ot_fonts()`
- 绘制：`fbink_fill_rect_gray()` / `fbink_cls()`
- 刷新：`fbink_refresh_rect()` / `fbink_wait_for_complete()`，`FBInkConfig` 上有 `no_refresh`、`is_flashing`、`wfm_mode` 字段

### 排除：GTK2 原生应用

Kindle 固件本身跑着 X11 服务器和 `awesome` 窗口管理器（用窗口标题里 `com.lab126.*` 风格的 app id 控制顶栏 chrome）。GTK2 应用是 X 客户端，**必须让整个 framework 全程活着**才能显示。

针对「GTK2 是否省电」这个问题的明确回答：**不省电，严格说反而略费。** 决定 Kindle 功耗的是两件事——WiFi 射频是否常开、是否阻止了系统休眠——这两点在任何技术栈下完全一样，且都远大于 e-ink 刷新本身的耗电。而 FBInk 路线可以把 Java framework 用 `SIGSTOP` 冻住（CPU 占用≈0），GTK2 路线做不到。

GTK2 的真正优势是不用和 framework 抢屏幕（awesome 会正经管理窗口，不会被状态栏重绘覆盖），代价是要额外装 kindle-sdk + GTK2 开发头文件 + meson，而且 GTK 默认不懂 e-ink 波形，刷新最后还是得靠 fbink/lipc 兜底。工程量最大而收益最小。

### 排除：POSIX sh + FBInk CLI

零编译是它唯一的优点。但本应用要「完整交互」：10 条分段进度条 × 12 格 = 120 次 fbink 进程调用，一次整页重绘要好几秒；触摸还得另外接一个 C 小程序。适合只读仪表盘，撑不起交互式应用。

### 排除：服务端渲染 PNG

Kindle 侧代码量极小，但布局逻辑离开了设备，屏幕分辨率、DPI、旋转都要在服务端硬编码，且完全无法交互。

### 排除：upstart 开机接管

在 Amazon framework 之前启动、把 Kindle 变成专用仪表盘设备。技术上可行（OpenReader 的 `openreader-boot.conf` 就是这么做的），但一旦配置出错，救援成本极高。**不做。**

---

## 4. 目标结构

现有 KOReader 插件完整保留，两端共享 `usage-api.md` 这份接口契约。新增：

```
kindle/
  src/
    main.c        入口、状态机（loading / ready / error / keypad）、事件循环
    ui.c   ui.h   卡片布局、分段进度条、按钮命中区  ← quotapage.lua + quotabar.lua
    keypad.c .h   数字网格键盘（0-9 . : 退格 确定 取消）
    quota.c  .h   JSON 解析、空值处理、百分比与重置时间格式化  ← quotadata.lua
    http.c   .h   纯 HTTP/1.0 GET over TCP
    input.c  .h   evdev 触摸：节点探测 + 坐标变换 + SYN_REPORT 组包
    config.c .h   key=value 配置读写                ← LuaSettings
    net.c    .h   WiFi 唤醒与连通性等待（lipc）
  third_party/cJSON/
  fonts/NotoSansSC-subset.ttf
  Makefile
  extension/                    → 部署到 /mnt/us/extensions/aiquota/
    config.xml
    menu.json
    bin/aiquota.sh              framework 接管 / 恢复 + 启动二进制
    bin/aiquota                 构建产物
  scriptlet/ai-quota.sh         → 部署到 /mnt/us/documents/
  README.md
tools/
  build.sh                      WSL 交叉编译 + 打包
```

### 几个设计约定

**HTTP 不引 libcurl。** Usage API 是纯 HTTP、无 TLS、无认证，一次 `getaddrinfo` + `connect` + `GET` 就够，约 120 行，避免把 libcurl + openssl 拖进静态链接。请求发 HTTP/1.0 而非 1.1：1.1 的 chunked 编码本客户端不做解码，1.0 功能等价。

**配置文件放 `/mnt/us/extensions/aiquota/config`。** FAT32 分区，插 USB 就能在电脑上直接改，等于给数字键盘留了一条逃生通道。

**字体随包分发。** 界面用到的汉字是固定集合（「小时」「周额度」「已用」「剩余」「暂无数据」「连接中」「失败」「已连接」「数据接口」「状态」「显示」「快照」「告警」「刷新」「接口地址」「关闭」「即将重置」「重置时间未知」「分钟后」「天」等），用 `pyftsubset` 裁 Noto Sans SC 到这个集合加 ASCII，产物小于 30 KB。不依赖设备上是否存在可用 CJK 字体。

**数字网格键盘。** Kindle 的系统键盘属于 framework，自己的程序调不到，必须自绘。目标输入形如 `192.168.1.100:46927`，一个 `0-9 . : 退格 确定 取消` 的大按钮网格就够，墨水屏上大按钮反而比全键盘好点。域名和路径场景通过改配置文件解决。

---

## 5. 实施阶段

### 阶段 0：环境与设备侦察（不写业务代码）

1. WSL 里拉 [koxtoolchain](https://github.com/koreader/koxtoolchain)，跑 `./gen-tc.sh kindlepw2`。约 30 分钟，一次性。
2. 拉 [FBInk](https://github.com/NiLuJe/FBInk)，`CROSS_TC=arm-kindlepw2-linux-gnueabi make kindle` 得到精简静态构建的 `libfbink.a` 和 `fbink.h`。只需要 `DRAW=1 OPENTYPE=1`，不需要 `IMAGE=1`。
3. 设备上开 USBNetwork / SSH，**逐条手工验证**下列命令，把实际结果写进阶段 1 的常量表：

```sh
fbink -e                                                     # 分辨率、bpp、旋转
lipc-get-prop com.lab126.wifid cmState                       # 期望 CONNECTED
lipc-set-prop com.lab126.pillow disableEnablePillow disable  # 之后能否 enable 恢复
killall -STOP awesome cvm                                    # 之后 killall -CONT 能否完全恢复
cat /dev/input/event0                                        # 逐个试，找出触摸节点编号
```

这一步的产出直接决定后面所有硬编码值，不能跳过。

### 阶段 1：能显示的骨架

- `Makefile`：静态链接 libfbink，`-Os -ffunction-sections -Wl,--gc-sections`。
- `main.c` 提供三个调试开关：
  - `--dump-state` —— 打印 `fbink_get_state()` 全部字段
  - `--calibrate` —— 触摸标定，点哪画哪，输出建议的坐标变换参数
  - `--demo` —— 用 `usage-api.md` 里的示例 JSON 渲染，不联网
- `ui.c` 把 `quotapage.lua:39-71` 的尺寸算式（`h_padding` / `card_gap` / `card_padding` / `card_width` / `card_inner_width` / `line_height`）和 `quotabar.lua:46-61` 的 pitch 取整算法逐行搬过来。

**验收：** `--demo` 的画面与仓库里的 `preview-partial.png` 布局一致。

### 阶段 2：数据链路

- `http.c` —— socket GET，`SO_RCVTIMEO` 设 50 秒。
- `quota.c` —— cJSON 解析，**严格照搬 `quotadata.lua` 的校验语义**：
  - `used` 恒为「已用百分比」，`displayMode == "remaining"` 时显示 `100 - used`
  - 单个提供商或窗口为 `null` 时，只把该项显示成「暂无数据」，绝不影响其他卡片
  - 用类型判断而非真值判断（`0` 是合法的 `used` 值）
  - `clamp` 到 0–100
- `net.c` —— 拉取前若 `cmState != CONNECTED`，执行 `lipc-set-prop -i com.lab126.cmd wirelessEnable 1` 并轮询等待，超时 20 秒。wifid 是独立守护进程，不受 cvm 冻结影响。拉取完成后按「谁开的谁关」原则释放：只有射频是本应用唤醒的才执行 `wirelessEnable 0`（`net_release_wifi()`，幂等），用户预先连着的连接不动——空闲期射频完全离线，这是整机最大的省电项。
- `config.c` —— URL 归一化照搬 `quotadata.lua:49-62`：补 `http://` 前缀、去掉尾部斜杠、补 `/api/usage` 后缀。

**验收：** SSH 里直接跑二进制，能拉到真实数据并正确渲染。

### 阶段 3：交互

- `input.c` —— 用 `EVIOCGBIT` 扫描 `/dev/input/event*`，找带 `ABS_MT_POSITION_X` 的节点；按 `SYN_REPORT` 组包成 tap 事件；套用阶段 0 标定得到的变换。
- 按钮命中区 —— 刷新 / 接口地址 / 关闭，对应 `quotapage.lua:221-234`。
- `keypad.c` —— 数字网格键盘。
- **刷新策略** —— 整页首绘用 GC16 闪一次；按钮按下反色、键盘输入回显这类局部更新用 DU 或 A2 波形，避免每次点击都全屏闪。这是墨水屏体验好坏的分水岭。

**验收：** 全程只用手指完成「改地址 → 刷新 → 关闭」。

### 阶段 4：framework 接管与双入口

代码量最小，风险最高。见下一节。

---

## 6. framework 接管

`extension/bin/aiquota.sh` 的命令序列，顺序参考 KOReader 的 `platform/kindle/koreader.sh`：

```sh
#!/bin/sh
restore_framework() {
    killall -CONT awesome cvm
    lipc-set-prop com.lab126.pillow disableEnablePillow enable
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0
    echo aiquota > /sys/power/wake_unlock # 释放启动侧抓的内核 wake lock
    cat /var/tmp/aiquota-fb.dump > /dev/fb0
}

trap "" TERM                            # 挡住 KUAL 退出时的级联 SIGTERM
trap restore_framework EXIT INT HUP     # 无论怎么退出都恢复

cat /dev/fb0 > /var/tmp/aiquota-fb.dump # 备份当前画面
lipc-set-prop com.lab126.pillow disableEnablePillow disable
lipc-set-prop -i com.lab126.powerd preventScreenSaver 1
echo aiquota > /sys/power/wake_lock     # 抓内核 wake lock，挡住无视 preventScreenSaver 的空闲挂起
killall -STOP awesome cvm
usleep 250000

/mnt/us/extensions/aiquota/bin/aiquota
```

### 为什么用 SIGSTOP 而不是 `stop lab126_gui`

社区里有「从 KUAL 启动仪表盘必然被级联杀死」的踩坑记录（见[这个 issue](https://github.com/yizhixiaoheigou/kindle-dashboard/issues/1)，结论是「只能从 SSH 启动，KUAL 必然失败」）。但那个结论**只对 `stop lab126_gui` 成立**——停整个 upstart job 会沿进程树把脚本自己一起带走。

KOReader 每天都从 KUAL 启动，靠的是两件事：

1. `trap "" TERM` —— 在动 framework 之前先屏蔽 SIGTERM。
2. `killall -STOP` 而不是 `stop` —— SIGSTOP 只是冻结进程，不触发 upstart 的自动重启逻辑，也不会波及自己这条进程链。

所以 KUAL 入口是可行的，前提是照上面的顺序写。

### 双入口

**KUAL 菜单** —— 标准 `extensions/aiquota/{config.xml, menu.json, bin/}` 结构，`config.xml` 里的 id 必须和文件夹名一致。菜单项一律写成 `sh /mnt/us/extensions/aiquota/bin/aiquota.sh` 显式调解释器，因为 FAT32 不保留可执行位。

**书库 Scriptlet** —— `/mnt/us/documents/ai-quota.sh`，头部带元数据注释：

```sh
# Name: AI 额度
# Icon: /mnt/us/extensions/aiquota/icon.png
# DontUseFBInk
exec sh /mnt/us/extensions/aiquota/bin/aiquota.sh
```

放在 documents 目录里的 `.sh` 会被 SH_Integration 索引成一本「书」，带图标出现在书库首页，点一下就运行——这就是要的独立入口。

**`# DontUseFBInk` 必须加。** 默认情况下 SH_Integration 会把脚本的 stdout / stderr 管道到 FBInk 打在屏幕上，会和我们自己接管的画面直接打架。

---

## 7. 风险表

| 风险 | 严重度 | 缓解 |
| --- | --- | --- |
| **冻结 framework 后程序崩溃 → 系统卡死，只能强制重启** | 高 | 五层防御，详见 `kindle/README.md` §9：① 脚本 `trap restore_framework EXIT INT HUP` 无条件恢复（恢复逻辑在 `bin/restore.sh`，冻结前先 source 进内存）＋二进制里的 SIGSEGV / SIGABRT handler；② OOM 优先级反转（脚本 `-1000`、应用 `+1000`），让内存压力只杀应用；③ 独立会话的看门狗兜底恢复；④ 心跳见证主线程（每个阻塞调用先 `wd_expect()` 报备上限），假死 120 秒后强杀；⑤ 杀不掉（D 状态）就由看门狗直接解冻。原第 ⑥ 层「空闲 `idle_timeout_s` 秒自动退出」已按常驻仪表盘需求移除，兜底职责随之消失——触摸点不中的场景应用是活的、心跳正常，看门狗不会介入。手动兜底：`sh .../bin/restore.sh`。开发期全程挂 USBNetwork/SSH |
| 从 KUAL 启动被级联杀死 | 中 | `trap "" TERM` + `killall -STOP`，不用 `stop lab126_gui`（见 §6） |
| 触摸坐标变换因机型而异（可能要 swap / mirror） | 中 | 内置 `--calibrate` 模式，标定结果写进配置文件 |
| 5.16.2.1.1 上 lipc 属性名与参考实现有出入 | 中 | 阶段 0 先在设备上逐条手工验证，再写死进脚本 |
| 设备无可用 CJK 字体 → 中文全是方框 | 中 | 随包分发 Noto Sans SC 子集，不依赖系统字体 |
| FAT32 不保留可执行位 | 低 | 一律 `sh /path/x.sh` 显式调解释器 |
| Windows CRLF 混进 `.sh` | 低 | 仓库 `.gitattributes` 已有 `*.sh text eol=lf`，已覆盖 |
| WiFi 未连接时请求直接失败 | 低 | `net.c` 启动时主动唤醒并等待，超时后走错误卡片而非崩溃 |

---

## 8. 端到端验证清单

1. `file kindle/extension/bin/aiquota` → `ARM ... EABI5 ... soft-float`；确认没有对 libfbink 的动态依赖。
2. SSH 里 `aiquota --dump-state`，分辨率 / 旋转与设备实际一致。
3. `aiquota --calibrate`，四角点击落点准确，参数写进配置。
4. `aiquota --demo`，对照 `preview-partial.png` 检查布局；再改 demo JSON 造出「某提供商为 null」和「超过 `alertPercent`」两种情况，确认「暂无数据」和 `!` 标记都对。
5. 联网跑一次真实拉取；再把 URL 改成不可达地址，确认走错误卡片而不是崩溃。
6. KUAL 入口启动 → 完整交互一轮 → 关闭 → **确认 framework 完全恢复**（能翻书、能进设置、能息屏唤醒）。**这是整个移植的关键验收点。**
7. 书库 Scriptlet 图标入口，重复第 6 步。
8. 断电重启一次，确认没有残留的冻结进程或被改动的 lipc 状态。

---

## 9. 工作量

约 1500–2000 行 C（vendored 的 cJSON 不计）。

阶段 0 的工具链构建是一次性的约 30 分钟。阶段 1–2 是主体工作量。阶段 4 代码量最小但风险最高，必须在有 SSH 兜底的前提下做。

---

## 10. 参考资料

**渲染**

- [NiLuJe/FBInk](https://github.com/NiLuJe/FBInk) —— [CLI.md](https://github.com/NiLuJe/FBInk/blob/master/CLI.md)、[fbink.h](https://github.com/NiLuJe/FBInk/blob/master/fbink.h)
- [koreader/koxtoolchain](https://github.com/koreader/koxtoolchain) —— 交叉编译工具链

**Kindle 平台**

- [KindleModding Wiki](https://kindlemodding.org/) —— [Scriptlets](https://kindlemodding.org/kindle-dev/scriptlets.html)、[Awesome WM](https://kindlemodding.org/kindle-dev/awesome-window-manager/)、[GTK 教程前置条件](https://kindlemodding.org/kindle-dev/gtk-tutorial/prerequisites.html)
- [Installing KUAL / MRPI](https://kindlemodding.gitbook.io/kindlemodding/post-jailbreak/installing-kual-mrpi)

**参考实现**

- [koreader `platform/kindle/koreader.sh`](https://github.com/koreader/koreader/blob/master/platform/kindle/koreader.sh) —— framework 接管的完整流程
- [koreader `frontend/device/kindle/device.lua`](https://github.com/koreader/koreader/blob/master/frontend/device/kindle/device.lua) —— WiFi / 电源 / 触摸的 lipc 命令
- [terpinedream/OpenReader](https://github.com/terpinedream/OpenReader/) —— `touch_reader.c` 的 evdev 读取
- [usetrmnl/trmnl-kindle](https://github.com/usetrmnl/trmnl-kindle/) —— KUAL 仪表盘扩展
- [kindle-dashboard issue #1](https://github.com/yizhixiaoheigou/kindle-dashboard/issues/1) —— 中文实战踩坑记录

**本仓库**

- [`usage-api.md`](usage-api.md) —— 接口契约，两端共享
- [`README.zh-CN.md`](README.zh-CN.md) —— KOReader 版说明
