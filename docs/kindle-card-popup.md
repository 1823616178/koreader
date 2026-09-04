# Kindle 端：点卡片弹出放大框 —— 需求与可行性

登记日期：2026-09-04，对照 master `1c04eaf`。范围仅 `kindle/` 原生客户端。
相关文档：[`kindle-ui-and-power.md`](kindle-ui-and-power.md)（UI 与省电规划）、[`kindle-light-and-battery.md`](kindle-light-and-battery.md)（前光/电量）。

---

## 0. 需求

点击仪表盘上的某一张卡片，弹出一个框，把这张卡片的内容放大显示在框上。

## 1. 结论

**可行，且不需要新依赖、不需要扩字体子集。** 建议实现方式：

- **矢量重绘，不是像素放大**——用放大后的字号和几何把同一份数据重画一遍，不要去缩放截下来的像素
- 被盖住的区域用 FBInk 的 `fbink_rect_dump` 存下、关闭时 `fbink_restore` 还原，**不整屏重绘**
- 弹出和关闭各一次 GC16，**只作用在弹窗矩形上**，不整屏闪

工作量集中在三块：卡片命中层（现在没有）、弹窗绘制（复用现有卡片绘制函数）、主循环里的模态等待（照抄设置页的形状）。

---

## 2. 现状：缺什么

| 需要的能力 | 现状 | 差距 |
| --- | --- | --- |
| 知道点到了哪张卡 | `ui_hit_button()` (`ui.c:660`) **只测底部三个按钮** | 卡片矩形根本没被记下来 |
| 卡片位置 | `draw_provider_slot()` (`ui.c:365`) 现算现用：`x = h_padding + (slot%2)*(card_w+card_gap)`，`y = y0 + (slot/2)*(card_h+row_gap)` | 算完就丢，没存进 `struct Ui` |
| 卡片身份 | 槽位顺序随 `snap->providers` 动态变化（关掉的提供商不占槽） | 槽号 ≠ 卡片身份，必须一起记 |
| 局部保存屏幕 | `third_party/fbink/fbink.h:1307` 有 `fbink_rect_dump` / `fbink_restore` / `fbink_free_dump_data` | **已具备**，项目还没用过 |
| 模态层 | `keypad.c` / `settings.c` 是**整屏** modal（`fbink_cls` 掉整页） | 弹窗是**局部** modal，是新形状 |
| 放大绘制 | `draw_provider_card()` / `draw_info_card()` 全部从 `ui->px_*`、`ui->card_w`、`ui->card_inner`、`ui->wb_h` 取几何 | 这正好是可以放大的抓手，见 §3.2 |

---

## 3. 设计

### 3.1 为什么是矢量重绘，不是像素放大

FBInk 确实能把 dump 缩放着打回去（`FBInkConfig.scaled_width` / `scaled_height`）。**不要用。** 卡片上的字是 OT 字体抗锯齿渲染出来的灰阶，放大约 2 倍再打到墨水屏上就是一团糊，e-ink 没有子像素可以骗眼睛。

重画一遍反而**改动更小**：卡片内容本来就是从 `QuotaSnapshot` 现算的，把字号和盒子尺寸乘个系数再调同一个绘制函数即可，文字由字体引擎按新字号重新光栅化，边缘依旧干净。

### 3.2 放大怎么实现：一份缩放过的 `Ui` 副本

`struct Ui` (`ui.h:54`) 全是 int + 一个 `FBInkConfig`，**按值拷贝是安全的**。绘制函数只读这些字段，不写。所以：

```c
Ui big = *ui;                    /* 副本，绝不能拿去做命中测试 */
big.card_w     = pop_w;
big.card_inner = pop_w - 2 * (big.card_padding + big.border);
/* 其余按 pop_w / ui->card_w 的比例放大 */
big.px_card_title = ui->px_card_title * pop_w / ui->card_w;
big.px_label      = ...;  big.px_percent = ...;  big.px_detail = ...;
big.line_h = ...; big.bar_h = ...; big.gap3 = ...; big.gap5 = ...;
big.card_padding = ...;
big.wb_h   = big.line_h + big.bar_h + big.gap3 + big.line_h;
big.card_h = 2 * (big.card_padding + big.border) + big.line_h
             + 3 * big.gap5 + 3 * big.wb_h;

draw_provider_card(&big, px, py, title, pq, snap, five_label, monthly_label);
```

`wb_h` / `card_h` 必须用放大后的分量重算，不能直接乘——它们是派生量，这跟 `configure_vertical_metrics()` (`ui.c:535`) 的算法保持一致。

**放大倍数天然就是 2 倍。** `card_w = (content_w - card_gap) / 2` (`ui.c:579`)，所以弹窗取满 `content_w` 宽就是约 2.03× 线性放大；高度按同比例得 `≈ 2 × card_h`。而 `content_h = 4*card_h + 4*row_gap + button_h` 是能塞进屏幕的，所以 `2*card_h` 大约占正文区一半高，**竖直方向富余，不会撑破屏幕**。

### 3.3 弹窗几何

- 宽 `ui->content_w`，高 `2 * big-card_h`（即上面算出的 `big.card_h`）
- 水平居中（左边就是 `h_padding`），竖直在标题栏以下的正文区居中
- 外框：`ui_fill` 黑底 + 白内矩形，边框比卡片粗一档（`ui_scale(2)` 起），让它明显浮在仪表盘之上
- 卡片内容画在白内矩形里，复用 `draw_provider_card` / `draw_info_card`

### 3.4 保存与还原

```c
FBInkDump dump = {0};                       /* 必须零初始化 */
fbink_rect_dump(fbfd, &pop_rect, &dump);    /* 画之前存 */
...                                          /* 画弹窗 + GC16 刷 pop_rect */
fbink_restore(fbfd, &cfg, &dump);           /* 关闭时还原 */
fbink_free_dump_data(&dump);                /* 堆内存是调用方的责任 */
```

**降级路径（必须写）：** `fbink_rect_dump` 失败（OOM）或 `fbink_restore` 返回 `-ENOTSUP`（位深/旋转变了）时，关闭弹窗就退回整屏 `ui_draw_dashboard()` + GL16 全刷。功能照常，只是关闭时闪一下。这跟 `net.c` / `light.c` 的一贯姿态一致：**降级，不崩**。

**内存。** 8bpp 下弹窗约 `content_w × 2*card_h` 字节，PW4 上大致 1 MB 量级；32bpp 则 ×4。上机跑 `--dump-state` 看 `bpp=` 确认（`main.c:411` 已经打印）。这块内存只在弹窗打开期间存在。

### 3.5 刷新波形

| 时刻 | 波形 | 范围 |
| --- | --- | --- |
| 弹出 | GC16 | **只有弹窗矩形** |
| 关闭（restore 成功） | GC16 | 同一个矩形 |
| 关闭（降级） | GL16 | 整屏 |

弹窗是大面积内容变化，GC16 才干净；但它被限制在一个矩形里，不是整屏闪白。按压不做反显闪烁——把整张卡反黑再弹回来是一次很显眼的 DU 黑闪，而**弹窗本身出现就是最好的按压反馈**。

### 3.6 主循环接线（`main.c:807` 那一段）

现在是：`wait_tap_until` 返回 tap → `light_idle_restore()` → `ui_hit_button()` → NONE 就 `continue`。

改成 NONE 之后再试 `ui_hit_card()`；命中则进一个内层模态等待：

```
打开弹窗
loop: wait_tap_until(fd, ..., deadline = idle_deadline, &x, &y)
      到期是闲置熄灯 -> light_idle_off(); 继续等
      拿到 tap      -> light_idle_restore(); 关闭弹窗; break
```

两条规则，都照抄设置页现成的语义：

1. **弹窗打开期间不自动刷新。** 否则拉到新数据会重画弹窗底下的仪表盘，还原时糊上去的就是过期画面。关闭后过期的 deadline 会**立即**补一次刷新——`wait_tap_until` 对已过期的 deadline 直接返回 1，设置页现在就是这么工作的。
2. **闲置熄灯照常计时。** 前光跟屏幕上是什么无关；弹窗开着挂一夜也该灭灯。关闭弹窗后重新计 3 分钟（`main.c:806` 同一行逻辑）。

### 3.7 关闭方式

**点任意位置关闭。** 不画关闭按钮：子集字体没有 `×` 字形（`ui.c:443` 的注释已经记了标题栏因此没有关闭图标）。若之后要一个看得见的关闭件，「返回」二字已经在 `fonts/glyphs.txt` 里（设置页在用），不必重裁字体。

---

## 4. 需要改的地方

| 文件 | 改动 |
| --- | --- |
| `kindle/src/ui.h` | `struct Ui` 加 `card_rect[9]` / `card_id[9]` / `card_drawn`；新增 `UiCard` 枚举与 `ui_hit_card()`、`ui_popup_draw()`、`ui_popup_rect()` 声明 |
| `kindle/src/ui.c` | `draw_provider_slot` / `draw_info_slot` 顺手登记矩形与身份；新增缩放副本绘制；`ui_draw_dashboard` 开头清空登记 |
| `kindle/src/main.c` | 命中卡片后的内层模态等待、dump/restore、降级重绘 |
| `kindle/tests/test_main.c` | 槽位→卡片身份的映射（给定 `ProviderSelection`）是纯函数，可测；矩形命中同理 |
| `kindle/README.md` | §5 使用：补一句「点卡片放大」 |
| `kindle/fonts/glyphs.txt` | **不动**——弹窗里的字符串和卡片是同一批 |

`net.c` / `light.c` / `config.c` / `settings.c` 完全不涉及。

---

## 5. 风险

| 风险 | 处理 |
| --- | --- |
| `fbink_restore` 位深/旋转不符返回 `-ENOTSUP` | framework 冻结期间不会转屏；仍必须接住返回值，走 §3.4 的整屏降级 |
| dump 内存在低内存设备上申请失败 | 同上，降级到整屏重绘；不因此放弃弹窗功能 |
| 弹窗期间来了自动刷新 | §3.6 规则 1：暂停，关闭后立即补 |
| 触摸标定不准点错卡 | 卡片是半屏大的目标，比底部按钮宽容得多；标定本身有 `--calibrate` |
| 槽位顺序随提供商开关变化 | 身份跟矩形一起登记，绝不用槽号反推卡片 |
| 卡片内容本来就是截断的（信息卡的 URL / 错误文本） | 这正是收益所在，见 §6 |

---

## 6. 收益（为什么这个需求值得做）

最实在的一处不是「看得更清楚」，而是**信息卡**：`draw_info_card` 的 `status_detail` 装的是接口地址或错误文本，现在被 `ui_text` 用省略号截在半张卡宽里（`ui.h:76`）。放大到整屏宽之后，排查「拉取失败」时终于能看全错误信息——**这是目前唯一一处屏幕上看不全的内容**。

提供商卡片的放大则是纯可读性收益：`px_detail` 只有十来像素，放大一倍在墨水屏上差别很明显。

---

## 7. 明确不做

- 像素放大镜（跟手指移动的放大区域）
- 双指捏合缩放、弹窗内滚动
- 弹窗里编辑任何值（编辑一律走设置页 + 键盘）
- 弹窗打开期间的任何周期性重绘
- 为弹窗新增配置键（不需要开关，不喜欢就别点）

---

## 8. 验收

1. 点任意一张提供商卡片：弹窗出现，内容与该卡一致但明显放大；**只有弹窗那块闪**，仪表盘其余部分不动
2. 点弹窗内外任意位置：弹窗消失，底下的仪表盘**逐像素回到原样**（不是重绘出来的近似）
3. 关掉部分提供商（`?providers=` 筛选）后重排卡片，点每一张弹出的都是对应的那一张
4. 点信息卡：接口地址 / 错误文本完整可读，不再有省略号
5. 弹窗开着挂 3 分钟以上：前光按时熄灭；点一下同时恢复前光并关闭弹窗
6. 自动刷新设为 1 分钟，弹窗开着等 2 分钟：期间不刷新，关闭后立即补一次
7. `--demo` 下八张卡逐张点开，不崩、不漏还原
