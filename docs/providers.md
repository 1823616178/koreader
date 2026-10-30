# Provider display and authentication (shared by both clients)

Rule: show exactly what the API returns, no fixed provider list.

- The KOReader plugin sends no provider filter at all, so the server
  returns everything it collects. The page renders one tile per returned
  provider, two per row; the info tile fills the last slot when the
  count is odd, and takes a full-width row when it is even (including
  zero, e.g. while loading). A saved `?providers=` query in the URL is
  passed through untouched, so narrowing the request by hand still works.
- Ordering and titles in the plugin: known providers first, in the order
  `grok, codex, kimi`, then AGY expanded into Gemini and Claude/GPT,
  then anything else the server returns, alphabetically, with a title
  derived from its id (`cursor` -> `Cursor`). A provider counts as
  returned when its object is present in the response, even if all its
  windows are null -- that renders a card saying "暂无数据" instead of
  silently hiding the provider. AGY is a container: it expands into its
  sub-providers, unless a response gives AGY five/seven windows directly,
  in which case it stays a single card.
- The Kindle client already packed its cards the same way from the
  response's provider selection, within its known set (`grok, codex,
  kimi, agy`; AGY expands to two cards) and its local `show_providers`
  order, always ending with the info card. A brand-new provider id that
  the Kindle structs do not know will not appear there -- that client
  would need a struct/card addition, whereas the plugin needs none.
- The KOReader grid paginates to fit the screen height, with **上一页** /
  **下一页** buttons when needed. Refresh, URL, token and close controls
  remain visible on every page. Monthly windows, including Grok's, are
  displayed when present.

## API endpoint defaults

- No real LAN address is hardcoded anywhere in the repo. Defaults are
  loopback (`http://127.0.0.1:46927`); a real server address is a
  per-device setting.
- KOReader: set it once with **接口地址** on the page; it persists in
  KOReader's `settings/readingquota.lua`, so upgrading the plugin does
  not reset it.
- Kindle: edit `api_url` in `extension/config` over USB. Note that
  redeploying the tarball replaces that file, so re-apply your address
  after a reinstall (or keep a copy of your config).
- Examples in docs that need an address shape use a generic
  `192.168.1.100` address, never a real deployment address.

## Optional Bearer authentication

The Usage API server in this repo's docs is a separate project; these
clients only implement the client side of the contract:

- If a token is configured, the client sends
  `Authorization: Bearer <token>` on every `/api/usage` request.
- If no token is configured, no header is sent (old behaviour).
- Tokens are printable ASCII without spaces (max 200 chars on both
  clients); a token with whitespace, non-ASCII or control characters is
  rejected and never sent, so it cannot inject a header line.
- KOReader: set/clear it with the **令牌** button on the page. The
  stored token is never echoed back on screen; leaving the field empty
  clears it. Invalid input keeps the previous token and shows an error.
  Saving either URL or token clears the previous snapshot and schedules
  a new request, even when a fetch is already in progress.
- Kindle: set `api_token` in `extension/config` over USB. The on-device
  keypad cannot type token characters. The token is stored in plaintext
  in that device-only file, like the rest of the config.
- Server side (to be implemented where the API lives): validate the
  token when one is required and answer `401` otherwise. Clients already
  treat any non-200 status as a fetch failure and keep the last good
  snapshot.
- Kindle disk caching is available only without a configured token.
  Authenticated snapshots stay in memory for the current session; changing
  tokens cannot load another account's persisted quotas. Legacy caches
  from before this policy are ignored.

## Security limits, stated plainly

- The Kindle client speaks plain HTTP only (no TLS on the old Kindle
  stack), and the KOReader plugin accepts the URL the user configures.
  A Bearer token over plain HTTP on a LAN is not secrecy against a
  network observer -- it is an access check at the server, not encryption.
  Do not expose the API port beyond a trusted LAN, and do not put a
  token in a URL query string (it would land in logs).
- The API returns percentages and reset times only, never CLI tokens,
  OAuth material, session bodies or local paths (see
  `docs/superpowers/plans/usage-api.md`).
