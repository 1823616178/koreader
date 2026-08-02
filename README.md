# readingquota.koplugin

*[中文版](README.zh-CN.md)*

A KOReader plugin that shows a full-screen "today's reading quota" page: how much
of your daily reading goal is left, drawn as a segmented bar in the style of a
terminal quota meter.

![](preview-partial.png)

## Why a KOReader plugin and not a KUAL extension

KUAL is a *launcher*, not a UI framework — it reads `menu.json` and runs shell
scripts. Drawing a screen under KUAL means `eips` or FBInk against the raw
framebuffer, parsing `/dev/input` yourself, and managing e-ink refresh modes by
hand, with no way to test outside the device.

A KOReader plugin gets the widget library, gesture handling, refresh strategy and
a desktop emulator for free, and runs unchanged on Kindle, Kobo, Android and
Linux. On Kindle the two aren't even alternatives: KOReader is normally launched
*from* KUAL, so KUAL stays the launcher and KOReader is the runtime.

## Layout

| File | Role |
| --- | --- |
| `_meta.lua` | Name and description, how KOReader identifies the plugin |
| `main.lua` | `WidgetContainer`: menu entry, dispatcher action, page launch |
| `quotapage.lua` | The full-screen page (`covers_fullscreen = true`) |
| `quotabar.lua` | The segmented bar widget |
| `quotadata.lua` | Today's numbers from the statistics DB, plus goal settings |

Today's reading time and page count are read straight out of the statistics
plugin's `statistics.sqlite3`, so nothing has to be instrumented twice. If that
database is missing or its schema has drifted, `quotadata.lua` falls back to demo
numbers and says so in the "数据来源" row rather than failing.

The daily goal lives in `settings/readingquota.lua` and can be counted either in
minutes or in pages; the page has a button to switch.

## Entry points

- File manager / reader menu → 更多工具 → 今日阅读额度
- A dispatcher action (`Reading quota`), so it can be bound to a gesture or key

Registering from the file manager as well as the reader (`is_doc_only = false`)
is what lets it work as a standalone screen rather than only as a reader
accessory.

## Development (WSL)

`dev.sh` assumes KOReader is installed at `/usr/lib/koreader` (the .deb layout)
and that an X display is reachable on `:0`.

```sh
./dev.sh check   # syntax-check every Lua file with KOReader's own luajit
./dev.sh link    # symlink the plugin into ~/.config/koreader/plugins
./dev.sh smoke   # render every page state to preview-*.png, no clicking needed
./dev.sh run     # launch the emulator
./dev.sh log     # tail crash.log
```

`smoke` is the fast loop: it boots just enough of KOReader to construct the page
and paint it into a PNG, which catches the runtime errors — bad widget fields,
nil sizes, missing requires — that a syntax check can't. It renders the live data
plus three synthetic states (partially done, goal reached, page-counting mode).

The emulator is sized to Kindle Paperwhite geometry (1072×1448) by default;
override with `EMULATE_READER_W` / `EMULATE_READER_H`.

## Deploying to a Kindle

Copy `readingquota.koplugin/` into `/mnt/us/koreader/plugins/`. It's pure Lua, so
there's nothing to cross-compile — what runs in the emulator is what runs on the
device. See [Installation on Kindle devices](https://github.com/koreader/koreader/wiki/Installation-on-Kindle-devices)
for how KOReader itself lands under `/mnt/us/koreader/` and gets launched from KUAL.

## Reference

### Read this first

**There is no official plugin-development guide.** The wiki has no
`Plugin-Development` page, and [Development_guide](https://koreader.rocks/doc/topics/Development_guide.md.html)
covers the source tree layout only — it says nothing about plugins, kodev or the
emulator. In practice the reliable sources are, in order: the installed source,
the generated API reference, and existing plugins.

### Installed source — the authority

The .deb install is the most trustworthy reference, because it's the exact
version this plugin runs against (`/usr/lib/koreader/git-rev` → **v2026.07.1**).
`koreader.rocks/doc` is generated from master, so it can describe APIs that
don't exist here yet.

| Path | What's there |
| --- | --- |
| `/usr/lib/koreader/frontend/ui/widget/` | The widget library — read the field lists at the top of each file |
| `/usr/lib/koreader/frontend/ui/elements/*_menu_order.lua` | Menu tree; explains where a `sorting_hint` lands |
| `/usr/lib/koreader/plugins/hello.koplugin/` | Minimal plugin template |
| `/usr/lib/koreader/plugins/statistics.koplugin/` | The DB this plugin reads; SQL and schema live in `main.lua` |
| `frontend/ui/widget/pagebrowserwidget.lua`, `bookstatuswidget.lua` | Full-screen page patterns (`covers_fullscreen`, key events, close handling) |

### API reference

[koreader.rocks/doc](https://koreader.rocks/doc/) — LDoc-generated, ~180 modules,
regenerated from master. Module pages follow
`https://koreader.rocks/doc/modules/<module.path>.html`, e.g.
[ui.widget.progresswidget](https://koreader.rocks/doc/modules/ui.widget.progresswidget.html).

Topic pages worth knowing:

- [UI Events](https://koreader.rocks/doc/topics/Events.md.html) — event propagation through the widget hierarchy; the thing to read before adding gestures or custom events
- [DataStore](https://koreader.rocks/doc/topics/DataStore.md.html) — settings and data directories
- [Hacking](https://koreader.rocks/doc/topics/Hacking.md.html), [Unit tests](https://koreader.rocks/doc/topics/Unit_tests.md.html), [Porting](https://koreader.rocks/doc/topics/Porting.md.html)

### Example plugins

[koreader/contrib](https://github.com/koreader/contrib) — the official collection
of community plugins. Several are app-shaped rather than reader accessories
(Calculator, Calendar, Chess, Sudoku, RSS reader, web browser), which makes them
the closest thing to a worked example of "a standalone app inside KOReader".

### Building from source

[doc/Building.md](https://github.com/koreader/koreader/blob/master/doc/Building.md)
— `./kodev build` and `./kodev run`, with `-w` / `-h` / `-d` for emulator geometry
and device presets like `kindle3`. Not needed while developing against the .deb
install, but that's the route if you ever need to patch KOReader itself.

### Wiki

- [Wiki home](https://github.com/koreader/koreader/wiki) (the wiki warns some pages are out of date)
- [User patches](https://github.com/koreader/koreader/wiki/User-patches) — for changing core behaviour without forking
- [KOReader Gestures](https://github.com/koreader/koreader/wiki/KOReader-Gestures) — the gesture system a dispatcher action binds into
