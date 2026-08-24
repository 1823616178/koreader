# AI Quota — Kindle 安装指南

从零把 `dist/aiquota-kindle.tar.gz` 装到 Kindle 上并安全地跑起来。构建过程见 [`README.md`](README.md) §3，这里只讲装机。

---

## 0. 先读这一段

这个应用启动时会 **SIGSTOP 冻结 Amazon framework**，独占整块屏幕，退出时再恢复。为此内置了六层自愈（[`README.md`](README.md) §9），软件层面的故障都能自己恢复；但触摸坐标、lipc 属性名这类东西**因机型而异**，第一次装机务必按 §4 的顺序来。

**装之前请先把 USBNetwork / SSH 弄通。** 这是你唯一的兜底通道，一条命令就能拿回设备：

```sh
sh /mnt/us/extensions/aiquota/bin/restore.sh
```

即使完全没有 SSH，最坏情况也只是**长按电源键强制重启**——SIGSTOP 冻结不持久，重启后一切如常，本应用不往系统里写任何开机项，不存在软砖风险。

---

## 1. 前提

| 项 | 是否必需 | 说明 |
| --- | --- | --- |
| Kindle 已越狱 | 必需 | 目标固件 5.16.2.1.1（softfloat ABI） |
| KUAL | 必需 | 菜单入口 |
| SH_Integration | 可选 | 只影响书库图标入口；没装的话 KUAL 入口照常可用 |
| USBNetwork / SSH | 强烈建议 | 首次装机的兜底通道，标定触摸也需要 |

---

## 2. 安装包

构建产物在仓库的 `dist/` 下（`dist/` 不入库，需要先跑一次 `./tools/build.sh`）：

```
dist/
  aiquota-kindle.tar.gz     打包好的部署包（约 130 KB），走 SSH 解压
  aiquota/                  同样内容的展开目录，走纯 USB 拷贝
```

两者内容完全一致，任选一种装法。

---

## 3. 两种装法

### 装法 A：纯 USB 拷贝（推荐，不需要 SSH）

USB 连上 Kindle，把 `dist/aiquota/` 里的两样东西拷到 Kindle 盘符根目录，与已有的同名文件夹**合并**（不要覆盖整个 `extensions/`，那会删掉你别的 KUAL 扩展）：

```
dist/aiquota/extensions/aiquota/    →  <Kindle 盘>/extensions/aiquota/
dist/aiquota/documents/ai-quota.sh  →  <Kindle 盘>/documents/ai-quota.sh
```

安全弹出 USB。

### 装法 B：SSH 解压

把 `aiquota-kindle.tar.gz` 拷到 Kindle 盘符根目录，安全弹出，然后 SSH 进设备：

```sh
tar -xzf /mnt/us/aiquota-kindle.tar.gz -C /mnt/us
rm /mnt/us/aiquota-kindle.tar.gz   # 可选：清掉安装包
```

### 装完的目录长这样

```
/mnt/us/
  extensions/aiquota/
    config.xml            KUAL 扩展声明
    menu.json             菜单项：AI Quota → AI 额度
    config                运行时配置（插 USB 就能改）
    icon.png              书库图标
    bin/aiquota           ARM 单二进制
    bin/aiquota.sh        接管 / 恢复 framework，KUAL 调用的就是它
    bin/watchdog.sh       独立会话看门狗
    bin/restore.sh        恢复逻辑 + SSH 兜底命令
    bin/NotoSansSC-subset.ttf
  documents/ai-quota.sh   书库入口（需要 SH_Integration）
```

重启 KUAL（或重启设备），菜单里会出现 **AI Quota → AI 额度**。

---

## 4. 首次运行：按这个顺序

**先别急着从 KUAL 点。** 前三步都是 SSH 里直接跑二进制，**不经过包装脚本、不冻结 framework**，出问题最多屏幕花一下，绝对安全。

### 4.1 确认二进制能跑、设备参数对

```sh
/mnt/us/extensions/aiquota/bin/aiquota --dump-state
```

应该打印一大串 `screen_width=` / `screen_dpi=` / `device_codename=` 之类的字段。核对分辨率和旋转与设备实际一致。

### 4.2 确认字体和布局

```sh
/mnt/us/extensions/aiquota/bin/aiquota --demo
```

用内置示例数据画一屏，不联网。对照仓库根目录的 `preview-partial.png` 看布局；中文如果全是方框，说明字体没随包装上。

### 4.3 标定触摸（**别跳过**）

```sh
/mnt/us/extensions/aiquota/bin/aiquota --calibrate
```

按提示依次点屏幕四角，程序画十字线并推导坐标变换，最后输出：

```
suggested config lines:
touch_swap_xy=0
touch_mirror_x=0
touch_mirror_y=0
```

把这三行抄进 `/mnt/us/extensions/aiquota/config`。

触摸坐标变换因机型而异，**没标定的话按钮可能点不中**——那时应用是好的，但你点什么都没反应，只能靠 SSH 兜底或等空闲超时。所以这一步值得花 30 秒。

### 4.4 正式启动前的准备

1. 配好接口地址（见 §5）。
2. 把 `idle_timeout_s` **临时改成 `60`**：万一触摸没标准点不动按钮，一分钟后应用自己退出还你设备，比默认的 10 分钟舒服。确认一切正常后再改回 `600`。
3. SSH 窗口保持开着，兜底命令准备好：

   ```sh
   sh /mnt/us/extensions/aiquota/bin/restore.sh
   ```

### 4.5 从 KUAL 启动

菜单 **AI Quota → AI 额度**。屏幕会闪一下然后进入应用。走一轮完整交互：

- **刷新** —— 重新拉取数据
- **接口地址** —— 数字键盘改 API 地址（只有 `0-9 . :` 加退格 / 确定 / 取消，面向 `IP:端口`；域名和带路径的地址请直接改配置文件）
- **关闭** —— 退出

**重点确认关闭后 framework 完全恢复**：能翻书、能进设置、能息屏唤醒。这是整个移植的关键验收点。

### 4.6 书库入口

装了 SH_Integration 的话，首页会出现「AI 额度」图标，重复 4.5 验证一遍。

---

## 5. 配置

`/mnt/us/extensions/aiquota/config`，位于 FAT32 分区，**插 USB 就能在电脑上用记事本直接编辑**，改完重开应用生效。

| 键 | 含义 |
| --- | --- |
| `api_url` | 数据接口地址。写 `host`、`host:port` 或完整 URL 都行 |
| `touch_swap_xy` | 触摸 X/Y 轴交换（0/1），由 `--calibrate` 确定 |
| `touch_mirror_x` | 触摸 X 轴镜像（0/1） |
| `touch_mirror_y` | 触摸 Y 轴镜像（0/1） |
| `idle_timeout_s` | 多少秒没有触摸就自动退出、交还 framework。范围 60–3600，写 0 或非法值取默认 600 |

`api_url` 会自动归一化：没有 scheme 补 `http://`、去掉尾部斜杠、结尾不是 `/api/usage` 就补上。所以写 `123.161.179.32:46927` 等价于 `http://123.161.179.32:46927/api/usage`。

注意：**不支持 https**（没链 TLS 库），接口必须是明文 HTTP。

---

## 6. 出问题怎么办

### 屏幕卡在应用界面，点什么都没反应

按顺序试：

1. **等一会儿。** 假死超过约 60–70 秒，看门狗会自动强杀应用并恢复 framework。彻底点不中按钮的话，最多等 `idle_timeout_s`（默认 600 秒）应用会自己退出。
2. **SSH 兜底**（立刻生效）：

   ```sh
   sh /mnt/us/extensions/aiquota/bin/restore.sh
   ```

   这条命令是幂等的，跑几次都无害；它只在 framework 确实处于冻结状态时才回贴备份画面。
3. **长按电源键强制重启。** 冻结状态不持久，重启后完全恢复，不会留下任何残留。

事后如果发现是点不中按钮，回到 §4.3 重新标定。

### KUAL 菜单里看不到 AI Quota

- 确认路径是 `/mnt/us/extensions/aiquota/`，文件夹名必须是 `aiquota`（要和 `config.xml` 里的 `<id>` 一致）。
- 确认 `config.xml` 和 `menu.json` 在 `extensions/aiquota/` 下，不是在 `bin/` 里。
- 重启 KUAL 或重启设备再看。

### 书库里没有「AI 额度」图标

需要 SH_Integration。没装的话用 KUAL 入口即可，功能完全一样。

### 界面显示「WiFi 未连接」

应用会主动唤醒 WiFi 并等最多 20 秒。如果一直失败，先在系统设置里确认 WiFi 能正常连上。

### 界面显示连接失败 / HTTP 错误

在 SSH 里直接验证接口通不通：

```sh
/mnt/us/extensions/aiquota/bin/aiquota --demo   # 先确认渲染本身没问题
```

然后检查 `config` 里的 `api_url`，确认 Kindle 和服务在同一网络、端口没被防火墙挡、用的是 `http://` 而不是 `https://`。

### 跑二进制报 Permission denied

`/mnt/us` 是 FAT32，存不住可执行位。设备上一般挂载成全员可执行所以不会有事；万一遇到，改用 `sh` 显式调用脚本（KUAL 菜单本来就是 `sh /mnt/us/.../aiquota.sh`）。二进制本身 exec 失败时包装脚本的 EXIT trap 仍会恢复 framework，不会把你卡住。

---

## 7. 升级

重新跑 `./tools/build.sh`，然后按 §3 覆盖 `extensions/aiquota/` 即可。

**注意保留你的 `config`**：升级包里的 `config` 是默认值，直接覆盖会把 `api_url` 和触摸标定冲掉。安全做法是先备份，或者只覆盖 `bin/` 目录。

---

## 8. 卸载

删掉两处就干净了：

```sh
rm -rf /mnt/us/extensions/aiquota
rm -f  /mnt/us/documents/ai-quota.sh
```

运行期在 `/var/tmp/` 下留过两个临时文件（`aiquota.ping` 心跳、`aiquota-fb.dump` 屏幕备份），重启即消失，想手动清也行：

```sh
rm -f /var/tmp/aiquota.ping /var/tmp/aiquota-fb.dump
```

本应用不注册任何开机项、不改动任何系统文件，卸载后系统与装之前完全一致。
