# AI credits dashboard for KOReader

*[简体中文](README.zh-CN.md)*

A compact, full-screen KOReader plugin that reads the aggregate Usage API and shows Claude Code, Codex, Kimi Code, and Antigravity (Gemini plus Claude/GPT) credits.

Five provider tiles and one connection-status tile fit in a two-column, three-row layout without scrolling or pagination. Each quota window shows its percentage, a segmented e-ink-friendly bar, and reset time.

![](preview-partial.png)

The default endpoint is:

```text
http://123.161.179.32:46927/api/usage
```

Use **API address** on the page to change it. Both a server base URL and a complete `/api/usage` URL are accepted. The setting is persisted in KOReader's `settings/readingquota.lua`.

Copy `readingquota.koplugin/` to the KOReader plugins directory, restart KOReader, and open **More tools → AI credits**. Keep the unauthenticated API on a trusted LAN only.

## Development

With KOReader installed at `/usr/lib/koreader` under WSL:

```sh
./dev.sh check
./dev.sh link
./dev.sh smoke
./dev.sh run
./dev.sh log
```

The smoke test renders loading, used, remaining, partial-data, and rebuild states with synthetic API data, so it does not require the LAN service to be online.
