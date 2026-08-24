# Kindle 原生移植实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `kindle-port.zh-CN.md` 把 readingquota KOReader 插件移植为 Kindle 原生 C 应用（FBInk 渲染 + KUAL/书库双入口），KOReader 插件原样保留。

**Architecture:** C 单二进制，静态链接 libfbink，单进程完成绘制 / evdev 触摸 / HTTP 请求；所有绘制带 `no_refresh`，末尾一次 `fbink_refresh_rect()`。配置文件为 `key=value` 纯文本，放 `/mnt/us/extensions/aiquota/config`。framework 接管用 `killall -STOP/CONT` + `trap "" TERM`（见移植文档 §6）。

**Tech Stack:** koxtoolchain `kindlepw2`（arm-kindlepw2-linux-gnueabi, soft-float）交叉编译；FBInk（libfbink.a + fbink.h，含 `fbink_fill_rect_gray` / OT 字体 API）；vendored cJSON；Noto Sans SC 子集字体（pyftsubset）；宿主侧用 WSL Ubuntu 24.04 的 gcc 跑纯逻辑单元测试。

**源文档（执行前必读）：**
- `kindle-port.zh-CN.md` —— 完整设计（映射表 §2、目标结构 §4、阶段 §5、接管脚本 §6、风险表 §7、验收清单 §8）
- `readingquota.koplugin/quotapage.lua` —— 布局算式（39-71 行）、卡片结构、按钮（221-234 行）
- `readingquota.koplugin/quotabar.lua` —— 分段进度条 pitch 取整算法（46-61 行）
- `readingquota.koplugin/quotadata.lua` —— 校验语义、URL 归一化（49-62 行）、格式化函数（157-191 行）
- `usage-api.md` —— 接口契约 + 示例 JSON
- `preview-partial.png` —— 布局视觉参照

**全局约束：**
- 禁止 `git commit` / 任何 git 变更（用户未要求；计划末尾统一提示用户自行提交）。
- 仓库 `.gitattributes` 已有 `*.sh text eol=lf`，所有 shell 脚本必须 LF。
- WSL 默认发行版 Ubuntu 24.04；sudo 用 `echo 550312171 | sudo -S ...`。Windows 仓库路径在 WSL 中为 `/mnt/g/code/koread_ext`。大型构建一律放 WSL 原生文件系统（`~/`），不要在 `/mnt/g` 里编译。
- 所有新代码注释用英文（与仓库现有代码风格一致）。
- 凡标注 **[需真机]** 的步骤跳过执行，但要写进 `kindle/README.md` 的机上验证清单。

---

### Task 1: WSL 工具链 + FBInk + cJSON + 字体工具（后台，约 30-60 分钟）

**Files:**
- Create（WSL 侧）: `~/koxtoolchain/`、`~/x-tools/arm-kindlepw2-linux-gnueabi/`、`~/fbink/`
- Create（仓库侧）: `kindle/third_party/cJSON/{cJSON.c,cJSON.h}`、`kindle/third_party/fbink/{libfbink.a,fbink.h}`、`kindle/fonts/NotoSansSC.ttf`（未裁剪的原始字体，后续 Task 6 裁剪）

- [ ] **Step 1: 装 crosstool-ng 依赖**

```bash
echo 550312171 | sudo -S apt-get update -qq
echo 550312171 | sudo -S apt-get install -y -qq build-essential bison flex texinfo help2man gawk libncurses-dev libtool-bin unzip wget curl file python3 python3-venv
```

- [ ] **Step 2: 构建 kindlepw2 工具链**

```bash
cd ~ && git clone --depth 1 https://github.com/koreader/koxtoolchain.git
cd ~/koxtoolchain && ./gen-tc.sh kindlepw2
# 产出：~/x-tools/arm-kindlepw2-linux-gnueabi/bin/arm-kindlepw2-linux-gnueabi-gcc
```

验证：`~/x-tools/arm-kindlepw2-linux-gnueabi/bin/arm-kindlepw2-linux-gnueabi-gcc --version` 有输出。

- [ ] **Step 3: 构建 FBInk 静态库**

```bash
cd ~ && git clone https://github.com/NiLuJe/FBInk.git fbink && cd fbink
git checkout "$(git tag -l 'v*' | sort -V | tail -1)"   # 最新 release tag
export PATH="$HOME/x-tools/arm-kindlepw2-linux-gnueabi/bin:$PATH"
make kindle CROSS_TC=arm-kindlepw2-linux-gnueabi DRAW=1 OPENTYPE=1 MINIMAL=1
# 若上述目标名/变量在该版本不存在，读 Makefile 找等价的 Kindle 静态库构建方式；
# 唯一硬性要求：产出 libfbink.a，且 fbink.h 里存在 fbink_fill_rect_gray 与 fbink_print_ot。
```

- [ ] **Step 4: 拷贝产物进仓库**

```bash
mkdir -p /mnt/g/code/koread_ext/kindle/third_party/{fbink,cJSON} /mnt/g/code/koread_ext/kindle/fonts
cp ~/fbink/libfbink.a ~/fbink/fbink.h /mnt/g/code/koread_ext/kindle/third_party/fbink/
cd /mnt/g/code/koread_ext/kindle/third_party/cJSON
curl -sLO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
curl -sLO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
```

- [ ] **Step 5: 下载 Noto Sans SC + 准备 fonttools**

```bash
cd /mnt/g/code/koread_ext/kindle/fonts
# 任一可用源均可，要 TTF/OTF 单文件；示例：
curl -sL -o NotoSansSC.ttf "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf"
python3 -m venv ~/fontenv && ~/fontenv/bin/pip install -q fonttools brotli
file NotoSansSC.ttf   # 须显示 font 类型，不是 HTML
```

- [ ] **Step 6: 回报**

报告：FBInk 版本号、`fbink.h` 中以下符号的确切签名（从文件里原样摘录）：`fbink_open`、`fbink_init`、`fbink_get_state`、`fbink_add_ot_font`、`fbink_print_ot`、`fbink_free_ot_fonts`、`fbink_cls`、`fbink_fill_rect_gray`、`fbink_refresh_rect`、`fbink_wait_for_complete`，以及 `FBInkConfig`、`FBInkOTConfig`、`FBInkOTFit`、`FBInkState` 结构体字段列表、WFM 枚举值（GC16/DU/A2 的常量名）。这些是 Task 4-5 写代码的契约依据。

---

### Task 2: 纯逻辑模块 —— config / quota / http + 宿主侧单元测试

**Files:**
- Create: `kindle/src/config.h`、`kindle/src/config.c`
- Create: `kindle/src/quota.h`、`kindle/src/quota.c`
- Create: `kindle/src/http.h`、`kindle/src/http.c`
- Create: `kindle/tests/test_main.c`（聚合所有用例，宿主 gcc 编译运行）
- Test: WSL 里 `gcc -Wall -Wextra -o /tmp/test_config kindle/tests/test_main.c kindle/src/config.c ...`

**接口契约（原样实现，不得改名）：**

`kindle/src/config.h`：
```c
#ifndef AIQUOTA_CONFIG_H
#define AIQUOTA_CONFIG_H

#include <stddef.h>

#define CONFIG_DEFAULT_API_BASE "http://123.161.179.32:46927"
#define CONFIG_PATH "/mnt/us/extensions/aiquota/config"

typedef struct {
    char api_url[512];   /* normalized, always ends with /api/usage */
    int touch_swap_xy;   /* 0 or 1: swap touch X/Y axes */
    int touch_mirror_x;  /* 0 or 1: x = width  - 1 - x */
    int touch_mirror_y;  /* 0 or 1: y = height - 1 - y */
} AppConfig;

void config_defaults(AppConfig *cfg);
/* Loads key=value file over the defaults. Missing file is fine: returns -1, defaults kept. */
int  config_load(AppConfig *cfg, const char *path);
int  config_save(const AppConfig *cfg, const char *path);
/* Port of quotadata.lua normalizeUrl (lines 49-62): trim -> default if empty ->
 * add http:// if no scheme -> strip trailing '/' -> append /api/usage if missing. */
void config_normalize_url(char *dst, size_t dst_size, const char *src);

#endif
```

`kindle/src/quota.h`：
```c
#ifndef AIQUOTA_QUOTA_H
#define AIQUOTA_QUOTA_H

#include <stddef.h>

typedef struct {
    int valid;            /* window object existed and had numeric used */
    double used;          /* clamped 0..100, always "used percent" semantics */
    int has_reset_at;
    long long reset_at;   /* unix epoch ms */
} QuotaWindow;

typedef struct {
    QuotaWindow five;
    QuotaWindow seven;
} ProviderQuota;

typedef struct {
    char display_mode[12];        /* "used" | "remaining" */
    double alert_percent;         /* clamped, default 85 */
    int has_generated_at;
    long long generated_at;       /* unix epoch ms */
    ProviderQuota claude, codex, kimi;
    struct { ProviderQuota gemini, third_party; } agy;
} QuotaSnapshot;

void quota_empty_snapshot(QuotaSnapshot *snap);
/* Parse + validate with quotadata.lua fetch() semantics. Returns 0 on valid JSON object, -1 otherwise. */
int  quota_parse_snapshot(const char *json, QuotaSnapshot *snap);
/* 100 - used when mode is "remaining"; caller guarantees w->valid. */
double quota_display_value(const QuotaWindow *w, const char *display_mode);
double quota_clamp(double v);
/* Ports of quotadata.lua formatPercent / formatReset / formatTimestamp. */
void quota_format_percent(char *buf, size_t n, double value);
void quota_format_reset(char *buf, size_t n, const QuotaWindow *w, long long now_ms);
void quota_format_timestamp(char *buf, size_t n, int has_value, long long ms);

#endif
```

`kindle/src/http.h`：
```c
#ifndef AIQUOTA_HTTP_H
#define AIQUOTA_HTTP_H

#include <stddef.h>

/* Plain HTTP/1.1 GET over TCP (no TLS). Sends "Connection: close" and reads to EOF.
 * Connect timeout 45 s, per-op recv/send timeout 50 s (mirrors socketutil:set_timeout(45, 50)
 * in quotadata.lua -- the 45 s floor is REQUIRED, the API cold-starts local services).
 * On success returns a malloc'd NUL-terminated body and sets *status to the HTTP code.
 * On failure returns NULL and fills errbuf with a short ASCII reason. */
char *http_get(const char *url, int *status, char *errbuf, size_t errbuf_size);

#endif
```

**语义要点（照 quotadata.lua 逐条核对）：**
- 类型判断而非真值判断：`used: 0` 合法；`used` 非 number → 该窗口 `valid=0`；窗口为 `null`/非对象 → `valid=0`；单窗口/单提供商为空绝不影响其他卡片。
- `display_mode`：`"remaining"` 原样，其他一律 `"used"`；`alert_percent` 非 number 时默认 85；均 clamp 到 0–100。
- `quota_format_reset`：`reset_at` 未知 → `"重置时间未知"`；`<=0` → `"即将重置"`；`<3600` → `"%d 分钟后"`（`max(1, ceil(s/60))`）；`<86400` → `"%d 小时 %d 分后"`；`<7*86400` → `"%d 天 %d 小时后"`；否则 `"MM-DD HH:MM"`（本地时间）。`quota_format_timestamp`：`"MM-DD HH:MM:SS"`，无值 → `"--"`。
- `quota_format_percent`：四舍五入后与整数差 `<0.05` → `"%d%%"`，否则 `"%.1f%%"`；该函数只在值有效时被调用。
- http.c：`getaddrinfo`（支持域名）→ 非阻塞 connect + poll 45s → `SO_RCVTIMEO`/`SO_SNDTIMEO` 50s → 发送 `GET <path> HTTP/1.1\r\nHost: <host>\r\nAccept: application/json\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n` → 读到 EOF → 解析状态行取 status、跳过 header 取 body。URL 解析只支持 `http://host[:port][/path]`，遇到 `https://` 直接报错（接口无 TLS，见 usage-api.md）。

- [ ] **Step 1: 写三个模块 + 测试**

`kindle/tests/test_main.c` 用例（纯 assert，main 里顺序跑，全过打印 `ALL PASS`）：
- `config_normalize_url`：`""` → `http://123.161.179.32:46927/api/usage`；`" 123.161.179.32:46927/ "` → `http://123.161.179.32:46927/api/usage`；`"http://h/api/usage"` → 原样；`"http://h/x"` → `http://h/x/api/usage`。
- config 存取往返：save 到 /tmp 再 load，字段全等。
- `quota_parse_snapshot`：用 `usage-api.md` 的完整示例 JSON → 各窗口值、`display_mode="used"`；改造示例：`"kimi": null` → kimi 两窗口 `valid=0` 且其余不受影响；`agy.thirdParty.five.used=0` → `valid=1`、`used=0`；`displayMode="remaining"` + `used=42` → `quota_display_value` = 58；`alertPercent` 缺失 → 85；垃圾输入 `"not json"` → 返回 -1。
- `quota_format_percent`：`42` → `"42%"`；`0.67791` → `"0.7%"`；`quota_format_reset`：固定 `now_ms` 造各分支（含 `重置时间未知` / `即将重置` / `分钟后` / `小时 分` / `天 小时` / 绝对日期）。
- http：先在 WSL 起 `python3 -m http.server 18080 --bind 127.0.0.1`（后台），测试 `http_get("http://127.0.0.1:18080/...")` status==200 且 body 非空；`http_get("http://127.0.0.1:9/")`（不可达）返回 NULL 且 errbuf 非空。

- [ ] **Step 2: 宿主编译并跑测试（预期全过）**

```bash
cd /mnt/g/code/koread_ext
gcc -std=c99 -Wall -Wextra -Werror -Ikindle/src -Ikindle/third_party/cJSON \
    -o /tmp/aiquota_test kindle/tests/test_main.c kindle/src/config.c kindle/src/quota.c \
    kindle/src/http.c kindle/third_party/cJSON/cJSON.c
/tmp/aiquota_test   # 预期输出 ALL PASS
```

---

### Task 3（检查点）: FBInk 头文件确认

- [ ] 等 Task 1 完成，读 `kindle/third_party/fbink/fbink.h`，把 Task 1 Step 6 的签名清单贴进本任务记录；Task 4/5 的绘制代码严格按真实签名写，不按记忆写。

---

### Task 4: 绘制层 —— ui.c + keypad.c

**Files:**
- Create: `kindle/src/ui.h`、`kindle/src/ui.c`
- Create: `kindle/src/keypad.h`、`kindle/src/keypad.c`
- Test: 交叉编译通过（`make` 见 Task 5）；视觉验收 [需真机] `--demo` 对照 `preview-partial.png`

**`kindle/src/ui.h` 契约：**
```c
#ifndef AIQUOTA_UI_H
#define AIQUOTA_UI_H

#include "quota.h"

typedef struct Ui Ui;   /* opaque, owns fb geometry, fonts, computed layout, button rects */

typedef enum { UI_ACTION_NONE = 0, UI_ACTION_REFRESH, UI_ACTION_EDIT_URL, UI_ACTION_CLOSE } UiAction;

/* font_path: bundled Noto Sans SC subset. dpi comes from fbink_get_state(). */
Ui  *ui_create(int fbfd, int screen_w, int screen_h, int dpi, const char *font_path);
void ui_destroy(Ui *ui);

/* Full redraw of the whole dashboard with no_refresh, caller does one fbink_refresh_rect after.
 * status_state: "连接中" | "失败" | "已连接"; status_detail: url or error text; loading: 0/1. */
void ui_draw_dashboard(Ui *ui, const QuotaSnapshot *snap,
                       const char *status_state, const char *status_detail, int loading);

/* Hit-test the bottom button row with a *screen-space* tap. */
UiAction ui_hit_button(const Ui *ui, int x, int y);

/* Invert a button's rect briefly for press feedback (DU waveform). */
void ui_flash_button(Ui *ui, int fbfd, UiAction action);

#endif
```

**`kindle/src/keypad.h` 契约：**
```c
#ifndef AIQUOTA_KEYPAD_H
#define AIQUOTA_KEYPAD_H

#include <stddef.h>

typedef struct Keypad Keypad;

/* result: 1 = confirmed (text valid), 0 = cancelled. text holds the edited string. */
Keypad *keypad_create(Ui *ui);              /* reuses Ui geometry + fonts */
void keypad_draw(Keypad *kp, const char *title, const char *text);   /* full redraw, no_refresh */
/* Returns: 0 keep editing, 1 confirm, -1 cancel; mutates text in place. */
int  keypad_handle_tap(Keypad *kp, int fbfd, int x, int y, char *text, size_t text_size);
void keypad_destroy(Keypad *kp);

#endif
```

**布局算式（从 quotapage.lua 39-71 逐行搬，整型像素，`S(n)=lround(n*dpi/167.0)`）：**
- `h_padding=S(16)`, `card_gap=S(8)`, `card_padding=S(9)`, `border=max(S(1),1)`, `line_height=S(27)`
- `content_width = W - 2*h_padding`；`card_width = (content_width - card_gap)/2`；`card_inner = card_width - 2*(card_padding+border)`
- 字号（px）：卡题 `S(19)`、标签 `S(14)`、百分比 `S(16)`、细节 `S(12)`；fbink OT 若支持 `size_px` 直接传像素，否则换算 pt。
- 进度条（quotabar.lua 46-61 原样）：`pitch=(width+gap)/segments`（浮点），`left=floor(i*pitch+0.5)`，`right=floor((i+1)*pitch+0.5)-gap`，`block_w=max(right-left,1)`；`segments=12`，`gap=S(2)`，高 `S(8)`；填充数规则照 `getFilledCount`（>0 至少 1 格，<1 最多 segments-1 格）。
- 灰阶值：黑 `0x00`、空槽（有数据）`0xCC`、空槽（无数据）`0x88`（对应 Blitbuffer COLOR_LIGHT_GRAY / COLOR_GRAY）；卡底色白、边框 `border` 像素黑框。
- 卡片内部：题行（左 72% 卡题 / 右 28% "已用"或"剩余"）→ `S(5)` → 窗口块 ×2（值行 `label 42%宽 | value 右对齐`，`used>=alertPercent` 时 value 追加 `" !"`；bar；`S(3)`；细节行右对齐 `formatReset`，无数据 `"暂无数据"`）→ 间隔 `S(5)`。
- 信息卡：标题 "数据接口"；块一：状态行 + 细线（高 `S(8)`，`0xCC`）+ detail 行；块二：显示模式行 + 细线 + `"快照 <ts> · 告警 <pct>"` 行（照 quotapage.lua 174-204 的结构）。
- 六卡布局：2 列 × 3 行（Claude Code / Codex / Kimi Code / AGY · Gemini / AGY · Claude/GPT / 信息卡），行间 `card_gap`；按钮行 3 枚：`刷新`（loading 时显示 `加载中…` 且禁用）/ `接口地址` / `关闭`，宽均分 content_width，高 `S(42)`，黑框白底黑字居中；整块内容在标题栏以下垂直居中（CenterContainer 语义）。
- 标题栏：高 `S(48)`，居中标题 `"AI 额度"`，其下居中副标题（host 部分，无配置显示 `未配置`），底部 1px 黑线（对照 quotapage.lua 257-287）。
- 命中区：按钮行三枚按钮的矩形存进 Ui；`ui_hit_button` 只做矩形包含判断。
- 文字截断：helper `ui_text()` 画 OT 文本并用 `FBInkOTFit`（或 fbink.h 实际提供的 fit/bbox 机制）量宽，超 `max_w` 时逐字符（按 UTF-8 边界）缩短并追加 `…` 后重画；因全程 `no_refresh`，fb 内覆盖画不可见，末尾统一刷新。**先读真实 fbink.h 再定实现**。
- 全绘序列：`fbink_cls`（白）→ 标题栏 → 6 卡 → 按钮行 → 调用方一次 `fbink_refresh_rect` 全屏 GC16 闪一次。

**keypad 布局：** 4 列网格，键：`1 2 3 ⌫` / `4 5 6 :` / `7 8 9 .` / `取消 0 确定(占2列)`；键区顶部显示标题 `"接口地址"` 和当前输入回显行；键为黑框大按钮（高 ≥ `S(56)`），按下反色（DU 波形局部刷新），回显行输入变化用 DU 局部刷新；允许字符仅限 `0-9 . :`；`确定` 仅在 text 非空时生效。确认/取消由 `keypad_handle_tap` 返回值表达，main 负责后续动作。

---

### Task 5: 设备层 + 入口 —— input.c / net.c / main.c + Makefile 交叉编译

**Files:**
- Create: `kindle/src/input.h`、`kindle/src/input.c`
- Create: `kindle/src/net.h`、`kindle/src/net.c`
- Create: `kindle/src/main.c`
- Create: `kindle/Makefile`
- Test: `make` 交叉编译 + `file` / `readelf` 验证；`--demo/--calibrate/--dump-state` 运行 [需真机]

**接口契约：**
```c
/* input.h */
int  input_open(void);   /* scan /dev/input/event* via EVIOCGBIT for ABS_MT_POSITION_X; -1 if none */
/* Blocking wait for one tap; returns 0 and fills screen-space x,y (config transform applied),
 * or -1 on error. Groups events by SYN_REPORT; tap = BTN_TOUCH down->up with small movement. */
int  input_wait_tap(int fd, int scr_w, int scr_h, const AppConfig *cfg, int *x, int *y);

/* net.h */
/* Ensure WiFi is up: if lipc-get-prop com.lab126.wifid cmState != CONNECTED, run
 * lipc-set-prop -i com.lab126.cmd wirelessEnable 1 and poll up to 20 s. Returns 0 if connected. */
int  net_ensure_wifi(void);
```

**`main.c` 状态机：** `LOADING`（先画骨架并刷新，再阻塞 fetch ≤50s，照 Lua 的 onShow/performFetch 行为）→ `READY` / `ERROR`；事件循环阻塞读触摸：命中按钮 → 刷新（回 LOADING）/ 打开 keypad / 关闭退出；`KEYPAD` 状态由 keypad 子循环处理，确认则保存配置（`config_save` 到 CONFIG_PATH）并刷新。
- 调试开关：`--dump-state`（打印 `fbink_get_state()` 全字段）、`--demo`（内嵌 usage-api.md 示例 JSON 渲染，不联网）、`--calibrate`（点哪画哪，打印原始坐标与建议的 swap/mirror 配置）。
- 信号处理：`SIGSEGV/SIGABRT/SIGINT/SIGTERM` handler 只做 async-signal-safe 的 `_exit(1)`（真正恢复由外层脚本的 `trap EXIT` 完成，见移植文档 §7）。
- 启动顺序：`fbink_open` → 配置 `FBInkConfig` → `fbink_init` → `fbink_get_state` → `config_load` → `net_ensure_wifi`（失败不退出，走 ERROR 卡片）→ `fbink_add_ot_font` → 状态机。

**`kindle/Makefile`：**
```make
CROSS    ?= arm-kindlepw2-linux-gnueabi-
CC       := $(CROSS)gcc
CFLAGS   := -Os -ffunction-sections -fdata-sections -Wall -Wextra -Isrc -Ithird_party/cJSON -Ithird_party/fbink
LDFLAGS  := -Wl,--gc-sections
LIBS     := third_party/fbink/libfbink.a -lm
SRCS     := $(wildcard src/*.c) third_party/cJSON/cJSON.c
BIN      := extension/bin/aiquota

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS) $(LDFLAGS)

test:   # 宿主侧纯逻辑测试
	gcc -std=c99 -Wall -Wextra -Werror -Isrc -Ithird_party/cJSON \
	    -o /tmp/aiquota_test tests/test_main.c src/config.c src/quota.c src/http.c third_party/cJSON/cJSON.c
	/tmp/aiquota_test

clean:
	rm -f $(BIN)
.PHONY: test clean
```

- [ ] **Step 1: 写四个文件 + Makefile**
- [ ] **Step 2: 交叉编译**

```bash
cd /mnt/g/code/koread_ext/kindle
PATH="$HOME/x-tools/arm-kindlepw2-linux-gnueabi/bin:$PATH" make
file extension/bin/aiquota   # 预期: ELF 32-bit LSB executable, ARM, EABI5, soft-float
readelf -d extension/bin/aiquota | grep NEEDED   # 预期: 只有 libc/libm/libpthread，无 libfbink
```

---

### Task 6: 打包 —— extension / scriptlet / 字体子集 / build.sh / README

**Files:**
- Create: `kindle/extension/config.xml`、`kindle/extension/menu.json`、`kindle/extension/bin/aiquota.sh`
- Create: `kindle/scriptlet/ai-quota.sh`
- Create: `kindle/fonts/glyphs.txt`（从源码提取）、`kindle/fonts/NotoSansSC-subset.ttf`
- Create: `tools/build.sh`
- Create: `kindle/README.md`
- Modify: `.gitignore`（追加 `kindle/extension/bin/aiquota`、`dist/`、`*.tar.gz`）

- [ ] **Step 1: 写接管脚本 `kindle/extension/bin/aiquota.sh`**（严格按移植文档 §6，一字不改顺序：`trap "" TERM` → `trap restore_framework EXIT INT HUP` → 备份 fb → pillow disable → preventScreenSaver 1 → `killall -STOP awesome cvm` → usleep → 跑二进制；restore 函数负责 CONT + pillow enable + preventScreenSaver 0 + 回灌 fb dump）

- [ ] **Step 2: 写 `config.xml` / `menu.json` / scriptlet**
- `config.xml`：标准 KUAL 扩展，`id` 必须为 `aiquota`（与目录名一致）。
- `menu.json`：菜单项动作一律 `sh /mnt/us/extensions/aiquota/bin/aiquota.sh`（FAT32 无可执行位）。
- `kindle/scriptlet/ai-quota.sh`：照移植文档 §6 双入口节，含 `# Name: AI 额度`、`# Icon:` 注释、`# DontUseFBInk` 行（必须，否则 SH_Integration 会把 stdout 打到屏幕上打架），末尾 `exec sh /mnt/us/extensions/aiquota/bin/aiquota.sh`。

- [ ] **Step 3: 字体子集**

```bash
cd /mnt/g/code/koread_ext
grep -hoP '[\x{0080}-\x{FFFF}]' kindle/src/*.c | sort -u | tr -d '\n' > kindle/fonts/glyphs.txt
# 人工补查：确认 小时周额度已用剩余暂无数据连接中失败已连接数据接口状态显示快照告警刷新接口地址关闭即将重置重置时间未知分钟后天加载取消确定未配置 等字都在
~/fontenv/bin/pyftsubset kindle/fonts/NotoSansSC.ttf \
  --text-file=kindle/fonts/glyphs.txt --unicodes=U+0020-007E \
  --output-file=kindle/fonts/NotoSansSC-subset.ttf --flavor=  --no-hinting --desubroutinize
ls -la kindle/fonts/NotoSansSC-subset.ttf   # 预期 < 100KB
```
（原始 NotoSansSC.ttf 不进包；subset 拷进 `kindle/extension/bin/`。）

- [ ] **Step 4: 写 `tools/build.sh`**（WSL 内运行：`export PATH` 工具链 → `make -C kindle` → 拷 binary + 字体到 extension/bin → 生成 `dist/aiquota-kindle.tar.gz`，内含 `extensions/aiquota/...` 与 `documents/ai-quota.sh` 的部署树结构；脚本头部注释说明用法）

- [ ] **Step 5: 写 `kindle/README.md`**（中文，内容：构建步骤、部署（USB 拷贝 + 解压路径）、KUAL/书库两个入口说明、配置文件位置与字段、**机上验证清单**——把移植文档 §8 的 8 条原样列入并标注操作方式、风险提示（§7 的高危项））

- [ ] **Step 6: 打包并验证产物树**

```bash
cd /mnt/g/code/koread_ext && ./tools/build.sh
tar -tzf dist/aiquota-kindle.tar.gz   # 核对 extensions/aiquota/{config.xml,menu.json,bin/aiquota,bin/aiquota.sh,bin/NotoSansSC-subset.ttf} + documents/ai-quota.sh
```

---

## Self-Review 记录

- 规格覆盖：§4 目录树 → Task 2/4/5/6 全覆盖；§5 阶段 0 工具链 → Task 1（设备侦察命令为 [需真机]，进 README）；阶段 1-3 → Task 2/4/5；阶段 4 + §6 → Task 6；§7 风险 → main.c 信号处理 + aiquota.sh trap + 字体随包（Task 4/6）；§8 验收 → README 清单（本机可做的 file/readelf/单测在 Task 2/5/6 执行）。
- 接口一致性：`AppConfig`/`QuotaSnapshot`/`UiAction`/`http_get` 等签名在 Task 2/4/5 间一致；`ui_create` 由 Task 5 的 main.c 调用，参数为 fbfd + 几何 + 字体路径。
- 已知留空（有意）：触摸坐标变换默认值待 `--calibrate` [需真机]；FBInk 确切签名以 Task 1 Step 6 回报为准。
