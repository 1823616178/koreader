# AI 额度面板（KOReader 插件）

*[English](README.md)*

一个紧凑的 KOReader 全屏面板，通过 Usage API 同时显示 Claude Code、Codex、Kimi Code 和 Antigravity（Gemini、Claude/GPT）的 5 小时与周额度。

面板使用三行两列布局，五个额度卡片和一个接口状态卡片会完整显示在同一页，不需要滚动或翻页。每个额度窗口包含百分比、分段进度条和预计重置时间；达到接口返回的 `alertPercent` 时会显示 `!`。

![](preview-partial.png)

## 数据接口

默认请求：

```text
http://123.161.179.32:46927/api/usage
```

页面首次打开后自动加载。底部按钮可以刷新数据、修改接口地址或关闭面板。地址输入框既接受主机地址（例如 `http://123.161.179.32:46927`），也接受完整的 `/api/usage` 地址。

插件遵循 `usage-api.md` 的字段约定：

- `used` 始终按已用百分比解析。
- `displayMode=remaining` 时显示 `100 - used`。
- 单个提供商或窗口为 `null` 时只显示该项“暂无数据”。
- 请求超时为 50 秒，以覆盖 Kimi 和 AGY 首次采集。
- 接口地址保存在 KOReader 的 `settings/readingquota.lua` 中。

## 安装

把 `readingquota.koplugin/` 整个复制到 KOReader 的插件目录：

```text
/mnt/us/koreader/plugins/
```

重启 KOReader 后，从“更多工具 → AI 额度”进入。插件可在文件管理器和阅读器中使用，也提供 `AI credits` dispatcher 动作供手势或按键绑定。

设备必须能访问运行 Usage API 的同一可信局域网。接口无身份验证，不应直接暴露到公网。

## 开发与验证（WSL）

`dev.sh` 默认使用 `/usr/lib/koreader`：

```sh
./dev.sh check   # 使用 KOReader 自带 LuaJIT 检查语法
./dev.sh link    # 链接到模拟器插件目录
./dev.sh smoke   # 渲染加载、已用、剩余和错误状态
./dev.sh run     # 启动 KOReader 模拟器
./dev.sh log     # 查看日志
```

冒烟测试使用合成 API 数据，不依赖局域网服务是否在线，输出 `preview-*.png` 供布局检查。

## 文件结构

| 文件 | 作用 |
| --- | --- |
| `_meta.lua` | 插件名称与描述 |
| `main.lua` | 菜单入口和 dispatcher 动作 |
| `quotapage.lua` | 一屏卡片布局、刷新和接口地址设置 |
| `quotabar.lua` | 墨水屏友好的分段进度条 |
| `quotadata.lua` | HTTP 请求、JSON 校验、空值与时间格式化 |
| `usage-api.md` | Usage API 数据结构说明 |
