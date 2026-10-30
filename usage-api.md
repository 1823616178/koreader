# AI Credits Usage API

本文档描述如何从其他项目读取本仪表盘聚合的 Codex、Kimi Code 和 Antigravity（AGY）额度。

## 接口概览

```text
GET /api/usage
```

默认完整地址：

```text
http://127.0.0.1:8787/api/usage
```

响应类型为 `application/json; charset=utf-8`，并带有 `Cache-Control: no-store`。

当前接口没有身份验证。请只在本机或可信局域网中使用，不要直接暴露到公网。

## 快速调用

### curl

```bash
curl --fail --silent --show-error \
  http://127.0.0.1:8787/api/usage
```

### JavaScript / Node.js

```js
const response = await fetch('http://127.0.0.1:8787/api/usage');
if (!response.ok) {
  throw new Error(`Usage API returned HTTP ${response.status}`);
}

const usage = await response.json();
console.log('Codex weekly used:', usage.codex.seven?.used);
console.log('AGY Gemini 5h used:', usage.agy.gemini.five?.used);
```

### Python

```python
import requests

response = requests.get("http://127.0.0.1:8787/api/usage", timeout=45)
response.raise_for_status()
usage = response.json()

print(usage["codex"]["five"])
print(usage["agy"]["thirdParty"]["seven"])
```

## 完整响应示例

下面的数值仅用于说明数据结构：

```json
{
  "displayMode": "used",
  "alertPercent": 85,
  "generatedAt": 1785695952333,
  "codex": {
    "fetchedAt": 1785695910000,
    "five": {
      "used": 12,
      "resetAt": 1785712676000
    },
    "seven": {
      "used": 34,
      "resetAt": 1786300383000
    }
  },
  "kimi": {
    "fetchedAt": 1785695950844,
    "five": {
      "used": 2,
      "resetAt": 1785709259452
    },
    "seven": {
      "used": 25,
      "resetAt": 1786000859452
    }
  },
  "agy": {
    "fetchedAt": 1785695952333,
    "gemini": {
      "five": {
        "used": 0.67791,
        "resetAt": 1785712676000
      },
      "seven": {
        "used": 0.135493,
        "resetAt": 1785835207000
      }
    },
    "thirdParty": {
      "five": {
        "used": 0,
        "resetAt": 1785713952000
      },
      "seven": {
        "used": 0,
        "resetAt": 1786300752000
      }
    }
  }
}
```

## TypeScript 类型

```ts
interface UsageWindow {
  /** 已使用比例，范围为 0 到 100。 */
  used: number;

  /** 当前额度窗口重置时间，Unix epoch 毫秒；未知时为 null。 */
  resetAt: number | null;
}

interface ProviderUsage {
  /** 此额度快照的采集时间，Unix epoch 毫秒；没有数据时为 null。 */
  fetchedAt: number | null;

  /** 5 小时额度窗口。 */
  five: UsageWindow | null;

  /** 周额度窗口。 */
  seven: UsageWindow | null;
}

interface AgyUsage {
  fetchedAt: number | null;
  gemini: ProviderUsageWindows;

  /** Antigravity 中共享额度的 Claude 和 GPT 模型。 */
  thirdParty: ProviderUsageWindows;
}

interface ProviderUsageWindows {
  five: UsageWindow | null;
  seven: UsageWindow | null;
}

interface UsageApiResponse {
  /** 前台默认显示模式；不会改变各窗口中 used 字段的含义。 */
  displayMode: 'used' | 'remaining';

  /** 前台使用的已用额度警示阈值。 */
  alertPercent: number;

  /** 本次 HTTP 响应生成时间，Unix epoch 毫秒。 */
  generatedAt: number;

  codex: ProviderUsage;
  kimi: ProviderUsage;
  agy: AgyUsage;
}
```

## 字段语义

### 百分比

所有 `used` 字段始终表示“已使用百分比”，有效范围为 `0` 到 `100`，可能包含小数。

即使响应中的 `displayMode` 为 `remaining`，`used` 的语义也不会改变。调用方如需显示剩余额度，应自行计算：

```js
const remaining = Math.max(0, Math.min(100, 100 - window.used));
```

### 时间

- `generatedAt`：API 响应的生成时间。
- `fetchedAt`：该提供商额度快照的采集时间。
- `resetAt`：额度窗口的预计重置时间。
- 所有时间均为 Unix epoch 毫秒，可直接传给 `new Date(value)`。

### 窗口名称

- `five`：5 小时额度窗口。
- `seven`：周额度窗口。字段名保留为 `seven`，用于兼容现有 Codex 和 Kimi 数据结构。
- `agy.gemini`：Antigravity 中的 Gemini 模型共享额度。
- `agy.thirdParty`：Antigravity 中的 Claude 与 GPT 模型共享额度。

## 无数据与错误处理

某个提供商暂时无法读取时，API 通常仍返回 HTTP `200`，对应窗口为 `null`：

```json
{
  "fetchedAt": null,
  "five": null,
  "seven": null
}
```

AGY 无数据时保持嵌套结构：

```json
{
  "fetchedAt": null,
  "gemini": {
    "five": null,
    "seven": null
  },
  "thirdParty": {
    "five": null,
    "seven": null
  }
}
```

调用方不应因为一个提供商为空而丢弃其他提供商的数据。建议使用可选链或显式空值判断：

```js
function readUsed(windowData) {
  return typeof windowData?.used === 'number' ? windowData.used : null;
}
```

服务器自身发生未处理错误时返回：

```http
HTTP/1.1 500 Internal Server Error
Content-Type: application/json; charset=utf-8
```

```json
{
  "error": "Internal server error"
}
```

## 采集和缓存行为

| 提供商 | 数据来源 | 默认刷新行为 |
| --- | --- | --- |
| Codex | 本地会话中的最新 rate limit 快照 | 服务端短缓存约 8 秒 |
| Kimi | 临时启动官方本地服务 | 默认缓存 300 秒，最低 60 秒 |
| AGY | 临时 PTY 与官方本地额度 RPC | 默认缓存 300 秒，最低 60 秒 |

首次请求可能需要等待 Kimi 和 AGY 的临时服务启动。客户端超时建议设置为至少 45 秒；缓存命中后的请求通常会很快返回。

采集失败时，Kimi 和 AGY 会优先返回最近一次成功缓存；没有成功缓存时返回空结构。已有成功缓存的情况下，失败后约 60 秒才会再次尝试；如果服务启动后从未成功采集，后续请求可能再次触发采集。

## 网络和跨域

服务默认监听 `0.0.0.0:8787`，同一局域网中的其他设备可以通过服务器 IP 调用：

```text
http://<dashboard-host>:8787/api/usage
```

当前接口没有发送 `Access-Control-Allow-Origin`，因此：

- Node.js、Python、Go、curl 等服务端或命令行客户端可以直接调用。
- 不同源的浏览器前端会被浏览器 CORS 策略拦截。
- 浏览器项目可以通过自己的后端代理此接口，或者与仪表盘部署在同一来源下。

不要为了绕过 CORS 将仪表盘直接暴露到公网。需要跨主机访问时，应使用可信局域网、VPN，或在反向代理层增加身份验证。

## 兼容性建议

当前接口尚未带版本号。调用方应：

1. 按字段读取数据，不依赖 JSON 属性顺序。
2. 允许出现未知的新提供商或新字段。
3. 将窗口对象和时间字段视为可空。
4. 使用 `typeof value === 'number'` 判断百分比，不要使用简单的真值判断，因为 `0` 是有效值。
5. 不要根据 `displayMode` 修改 `used` 字段的解释。

## 安全边界

API 只向调用方返回额度百分比和时间戳，不返回 CLI 登录令牌、OAuth 凭据、会话内容或本地文件路径。

但是额度本身仍属于账号信息，并且接口当前没有认证。建议仅允许可信客户端访问，并通过 `HOST=127.0.0.1` 将仅供本机使用的部署限制在回环地址：

```bash
HOST=127.0.0.1 node server.js
```
