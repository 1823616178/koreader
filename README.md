# AI credits dashboard for KOReader

*[简体中文](README.zh-CN.md)*

A compact, full-screen KOReader plugin that reads the aggregate Usage API and shows one tile per provider the API returns -- Grok, Codex, Kimi Code, Antigravity (Gemini plus Claude/GPT), or anything else the server collects -- in a two-column grid. Large grids paginate to fit the screen, keeping the controls accessible. Each quota window, including monthly quotas when returned, shows its percentage, a segmented e-ink-friendly bar, and reset time.

![](preview-partial.png)

The default request is:

```text
http://127.0.0.1:46927/api/usage
```

Use **API address** on the page to change the server. Both a server base URL and a complete `/api/usage` URL are accepted. No provider filter is sent: the page shows exactly what the API returns (a saved `?providers=` query in the URL is passed through untouched). The setting is persisted in KOReader's `settings/readingquota.lua`, so upgrading the plugin does not reset it. The shipped default is loopback only (`127.0.0.1`); set your server's LAN address once, and never commit a real LAN address to the repo.

Use **Token** on the page for an optional Bearer token, sent as `Authorization: Bearer <token>` when set (see `docs/providers.md`). The token is never echoed on screen; leaving the field empty clears it. This is the client side only: the server must validate the token, and plain HTTP on a LAN is not encryption, so keep the API off the public internet either way.

Copy `readingquota.koplugin/` to the KOReader plugins directory, restart KOReader, and open **More tools → AI credits**. Keep the API on a trusted LAN only, with or without a token.

## Development

With KOReader installed at `/usr/lib/koreader` under WSL:

```sh
./dev.sh check   # syntax-check with KOReader's luajit
./dev.sh test    # data, authentication and page-state unit tests
./dev.sh link
./dev.sh smoke
./dev.sh run
./dev.sh log
```

The smoke test renders loading, used, remaining, partial-data, and rebuild states with synthetic API data, so it does not require the LAN service to be online.
