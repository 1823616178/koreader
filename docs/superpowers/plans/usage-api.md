# AI Credits Usage API

本仪表盘把本机已登录的 Grok、Claude Code、Codex、Kimi Code、OpenCode Go 和 Antigravity（AGY）额度聚合成一个只读 HTTP 接口。其他项目应调用本接口，不要直接去读各 CLI 的凭据文件或会话目录。

文档对照实现日期：2026-08-27。接口尚未带版本号。

---

## 1. 基址与约定

| 项 | 值 |
| --- | --- |
| 默认基址 | `http://127.0.0.1:8787` |
| 局域网 | `http://<dashboard-host>:8787`（服务默认监听 `0.0.0.0`） |
| 协议 | HTTP/1.1 |
| 认证 | 无。不要暴露到公网 |
| CORS | 不返回 `Access-Control-Allow-Origin` |
| 额度字段语义 | 所有 `used` 都是**已用百分比** `0–100`，与前台显示模式无关 |

改端口或绑定地址：

```bash
HOST=127.0.0.1 PORT=8787 node server.js
```

---

## 采集名单

改下面这块即可控制本机采集哪些工具：`true` 采集，`false` 跳过。也可以写成名称数组，例如 `["grok", "claude"]`。仪表盘 Track 按钮会改写这一块。每次请求都会重新读取，保存后无需重启。

```providers
{
  "grok": true,
  "claude": true,
  "codex": true,
  "kimi": true,
  "opencode": true,
  "agy": true
}
```

可用名字：`grok`、`claude`、`codex`、`kimi`、`opencode`、`agy`。

---

## 2. 路由一览

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/usage` | 聚合额度 JSON。本文主体。 |
| `GET` | `/api/providers` | 当前纳入统计的工具开关 |
| `PUT` / `PATCH` | `/api/providers` | 更新工具开关，写入本文「采集名单」代码块 |
| `GET` | `/` | 仪表盘页面 |
| `GET` | `/k`、`/r`、`/e`、`/kr`、`/kobo`、`/eink` | KOBO / 墨水屏页，显示**剩余**额度 |
| `GET` | `/u`、`/ku` | KOBO 页，显示**已用**额度 |
| `GET` | `/styles.css`、`/app.js` | 前台静态资源 |

页面可通过 `?mode=used` 或 `?mode=remaining` 覆盖显示模式。`GET /api/usage` 可用 `providers`（或 `tools`）指定本次要查哪些工具，见下文。未传参时，以本文「采集名单」为准。

未选中的工具仍会出现在 JSON 里，但窗口为 `null`，并且本次不会去采集。以响应里的 `providers` 字段为准。

---

## 3. `GET /api/usage`

### 请求

```http
GET /api/usage HTTP/1.1
Host: 127.0.0.1:8787
```

无 body。可选 query：

| 参数 | 说明 |
| --- | --- |
| `providers` | 本次要返回额度的工具，逗号分隔。别名：`tools`、`tool`、`provider`。可重复。 |

可用名字：`grok`、`claude`、`codex`、`kimi`、`opencode`、`agy`。

```http
GET /api/usage?providers=grok,claude
GET /api/usage?tools=codex
GET /api/usage?providers=grok&providers=kimi
```

传了这个参数后，**只采集、只启用这些工具**，不改文档里的采集名单。没传则用文档默认名单。

全部名字都不认识时返回 HTTP `400`：

```json
{ "error": "Unknown tool. Use grok, claude, codex, kimi, opencode, agy" }
```

### 成功响应

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Cache-Control: no-store
```

某个提供商暂时读不到时，接口仍返回 `200`，该提供商的窗口为 `null`。不要因为一家为空就丢弃其他家的数据。

### 失败响应

未处理的服务器错误：

```http
HTTP/1.1 500 Internal Server Error
Content-Type: application/json; charset=utf-8
Cache-Control: no-store
```

```json
{ "error": "Internal server error" }
```

未知路径返回纯文本 `404 Not found`。

---

## 3b. 工具开关 `/api/providers`

关闭某个工具后，仪表盘不再采集它（不会拉起对应 CLI / PTY），卡片也会隐藏。开关只保存在本文「采集名单」代码块里。每次请求都会重新读取，手工改文档后无需重启。

### 读取

```http
GET /api/providers HTTP/1.1
```

```json
{
  "providers": {
    "grok": true,
    "claude": true,
    "codex": false,
    "kimi": true,
    "opencode": true,
    "agy": false
  }
}
```

### 更新

```http
PUT /api/providers HTTP/1.1
Content-Type: application/json
```

```json
{ "providers": { "codex": false, "agy": false } }
```

只提交要改的键；未出现的工具保持原值。`PATCH` 行为相同。非法 JSON 返回 HTTP `400`。

### 超时

首次请求可能要等 Grok / Claude / Kimi / AGY 拉起临时进程。客户端超时建议 **≥ 45 秒**。缓存命中后通常很快。

---

## 4. 顶层字段

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `displayMode` | `"used"` \| `"remaining"` | 前台默认显示模式。默认 `"remaining"`。**不改变**各窗口 `used` 的含义。 |
| `alertPercent` | `number` | 已用百分比警示阈值，默认 `85`。 |
| `generatedAt` | `number` | 本次 HTTP 响应生成时间，Unix epoch **毫秒**。 |
| `providers` | `object` | 各工具是否纳入统计，键为 `grok` / `claude` / `codex` / `kimi` / `opencode` / `agy`，值为布尔。 |
| `grok` | `GrokUsage` | Grok Build / SuperGrok |
| `claude` | `ProviderUsage` | Claude Code |
| `codex` | `ProviderUsage` | Codex |
| `kimi` | `ProviderUsage` | Kimi Code |
| `opencode` | `OpencodeUsage` | OpenCode Go |
| `agy` | `AgyUsage` | Antigravity |

时间戳一律是 Unix epoch 毫秒，可直接 `new Date(value)`。

---

## 5. 窗口对象

```ts
interface UsageWindow {
  used: number;          // 已用百分比，0–100，可为小数。0 是有效值。
  resetAt: number | null; // 窗口预计重置时间；未知为 null
  status?: "ok" | "rate-limited"; // 目前仅 OpenCode 出现
}
```

显示剩余额度时由调用方换算，接口不会改写 `used`：

```js
const remaining = Math.max(0, Math.min(100, 100 - window.used));
```

判断有没有数据时用 `typeof window?.used === 'number'`，不要用真值判断（`0` 也是有效已用量）。

### 窗口名称

| 字段 | 含义 |
| --- | --- |
| `five` | 5 小时窗口。OpenCode 上是 `rolling` 的兼容别名。 |
| `seven` | 周窗口。名字保留为 `seven`，兼容既有 Claude / Codex / Kimi 结构。 |
| `monthly` | 月窗口。Grok 面板出现时才会有值；OpenCode 始终尝试填充。 |
| `rolling` | OpenCode Go 滚动 5 小时窗口。 |
| `weekly` | OpenCode Go 周窗口。 |

当前 Grok SuperGrok 的 `/usage` 面板通常只有周额度：`five` / `monthly` 多为 `null`，有效数据在 `seven`。

---

## 6. 各提供商结构

### Grok、Claude、Codex、Kimi

```ts
interface ProviderUsage {
  fetchedAt: number | null;
  five: UsageWindow | null;
  seven: UsageWindow | null;
}

interface GrokUsage extends ProviderUsage {
  monthly: UsageWindow | null;
}
```

无数据时：

```json
{
  "fetchedAt": null,
  "five": null,
  "seven": null
}
```

Grok 无数据时多一个 `monthly: null`。

### OpenCode Go

```ts
interface OpencodeUsage {
  fetchedAt: number | null;
  rolling: UsageWindow | null;
  weekly: UsageWindow | null;
  monthly: UsageWindow | null;
  five?: UsageWindow | null;   // 等于 rolling
  seven?: UsageWindow | null;  // 等于 weekly
}
```

`five` / `seven` 是兼容别名，新代码优先读 `rolling` / `weekly` / `monthly`。

窗口被限流时 `status` 为 `"rate-limited"`，且 `used` 记为 `100`。

### Antigravity

```ts
interface AgyUsage {
  fetchedAt: number | null;
  gemini: { five: UsageWindow | null; seven: UsageWindow | null };
  thirdParty: { five: UsageWindow | null; seven: UsageWindow | null };
}
```

- `gemini`：Gemini 模型共享额度
- `thirdParty`：Claude 与 GPT 模型共享额度

无数据时仍保持上述嵌套，窗口为 `null`。

---

## 7. 完整响应示例

数值仅用于说明结构：

```json
{
  "displayMode": "remaining",
  "alertPercent": 85,
  "generatedAt": 1785695952333,
  "providers": {
    "grok": true,
    "claude": true,
    "codex": true,
    "kimi": true,
    "opencode": true,
    "agy": true
  },
  "grok": {
    "fetchedAt": 1785695890000,
    "five": null,
    "seven": { "used": 8, "resetAt": 1788408060000 },
    "monthly": null
  },
  "claude": {
    "fetchedAt": 1785695900000,
    "five": { "used": 18, "resetAt": 1785712676000 },
    "seven": { "used": 42, "resetAt": 1786300383000 }
  },
  "codex": {
    "fetchedAt": 1785695910000,
    "five": { "used": 12, "resetAt": 1785712676000 },
    "seven": { "used": 34, "resetAt": 1786300383000 }
  },
  "kimi": {
    "fetchedAt": 1785695950844,
    "five": { "used": 2, "resetAt": 1785709259452 },
    "seven": { "used": 25, "resetAt": 1786000859452 }
  },
  "opencode": {
    "fetchedAt": 1785695951200,
    "rolling": { "used": 1, "resetAt": 1785712676000, "status": "ok" },
    "weekly": { "used": 46, "resetAt": 1786000859452, "status": "ok" },
    "monthly": { "used": 23, "resetAt": 1788134400000, "status": "ok" },
    "five": { "used": 1, "resetAt": 1785712676000, "status": "ok" },
    "seven": { "used": 46, "resetAt": 1786000859452, "status": "ok" }
  },
  "agy": {
    "fetchedAt": 1785695952333,
    "gemini": {
      "five": { "used": 0.67791, "resetAt": 1785712676000 },
      "seven": { "used": 0.135493, "resetAt": 1785835207000 }
    },
    "thirdParty": {
      "five": { "used": 0, "resetAt": 1785713952000 },
      "seven": { "used": 0, "resetAt": 1786300752000 }
    }
  }
}
```

---

## 8. 调用示例

### curl

```bash
curl --fail --silent --show-error \
  http://127.0.0.1:8787/api/usage

curl --fail --silent --show-error \
  'http://127.0.0.1:8787/api/usage?providers=grok,claude'
```

只取 Grok 周剩余：

```bash
curl --fail --silent --show-error \
  http://127.0.0.1:8787/api/usage \
| jq '100 - .grok.seven.used'
```

### JavaScript / Node.js

```js
const response = await fetch('http://127.0.0.1:8787/api/usage');
if (!response.ok) {
  throw new Error(`Usage API returned HTTP ${response.status}`);
}

const usage = await response.json();
const remaining = (window) =>
  typeof window?.used === 'number' ? Math.max(0, Math.min(100, 100 - window.used)) : null;

console.log('Grok weekly remaining:', remaining(usage.grok.seven));
console.log('Claude 5h remaining:', remaining(usage.claude.five));
console.log('OpenCode rolling used:', usage.opencode.rolling?.used);
console.log('AGY Gemini 5h used:', usage.agy.gemini.five?.used);
```

### Python

```python
import requests

response = requests.get("http://127.0.0.1:8787/api/usage", timeout=45)
response.raise_for_status()
usage = response.json()

def remaining(window):
    if not window or not isinstance(window.get("used"), (int, float)):
        return None
    return max(0, min(100, 100 - window["used"]))

print(remaining(usage["grok"]["seven"]))
print(usage["claude"]["five"])
print(usage["opencode"]["rolling"])
print(usage["agy"]["thirdParty"]["seven"])
```

### Go

```go
type UsageWindow struct {
    Used    float64 `json:"used"`
    ResetAt *int64  `json:"resetAt"`
}

// 省略其他结构；调用方按字段解码，忽略未知键。
```

---

## 9. 采集与缓存

| 提供商 | 数据来源 | 默认刷新 |
| --- | --- | --- |
| Grok | PTY 启动官方 `grok`，打开 `/usage` 面板后解析，写入 `~/.grok/usage-cache.json` | 缓存 300 秒，最低 60 秒；失败后约 60 秒再试 |
| Claude | PTY 启动官方 `claude`，打开 `/usage` 面板后解析，写入 `~/.claude/usage-cache.json` | 同上 |
| Codex | 优先官方 app-server 限额快照，失败则 PTY `/status`；再失败则扫描 `~/.codex/sessions` | 成功后缓存 300 秒；失败回退会话文件，约 60 秒再试 |
| Kimi | 临时启动 `kimi web --no-open`，只请求本机 `/api/v1/oauth/usage` | 缓存 300 秒；失败保留上次成功结果 |
| OpenCode | 官方 `GET https://opencode.ai/zen/go/v1/usage` | 缓存 300 秒；失败保留上次成功结果 |
| AGY | 临时 PTY 启动 `agy`，只调本机 `RetrieveUserQuotaSummary` | 缓存 300 秒；失败保留上次成功结果 |

并发请求会共用同一次采集，不会并行拉起多个 CLI。

可用环境变量关闭主动刷新（只读缓存 / 会话文件）：

- `GROK_ACTIVE_REFRESH=false`
- `CLAUDE_ACTIVE_REFRESH=false`
- `CODEX_ACTIVE_REFRESH=false`

---

## 10. 网络与跨域

- 命令行、Node、Python、Go 等非浏览器客户端可直接调用。
- 不同源的浏览器前端会被 CORS 拦住；请走同源部署或自己的后端代理。
- 不要为了跨域把仪表盘暴露到公网。跨主机请用可信局域网、VPN，或在反代上加认证。

---

## 11. 兼容性

1. 按字段名读取，不依赖 JSON 属性顺序。
2. 允许出现未知的新提供商或新字段。
3. 窗口对象、`fetchedAt`、`resetAt` 都可空。
4. `used === 0` 表示额度没用过，不是“没有数据”。
5. 不要根据 `displayMode` 改读 `used`。前台显示剩余时在客户端用 `100 - used`。
6. Grok 的 `monthly`、OpenCode 的 `rolling` / `weekly` / `monthly` 都可能为 `null`。

---

## 12. 安全边界

接口只返回百分比和时间戳，不返回 CLI token、OAuth、会话正文或本地路径。额度仍是账号信息，且当前无认证。

仅本机使用时：

```bash
HOST=127.0.0.1 node server.js
```
