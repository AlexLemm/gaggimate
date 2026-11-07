# Working in this repo

## Changing the web UI means rebuilding the embedded blob

The web UI is **not** served from LittleFS. It is gzipped, packed into
`src/display/webassets/web_ui.bin` and memory-mapped out of firmware flash (GM-106). The firmware embeds whatever is
in that blob at compile time.

So `npm run build` alone changes nothing on the device: it only writes `web/dist`. Any change under `web/src/` needs
the full pipeline before the firmware build, or the new firmware ships the previous UI while its API happily serves the
new fields — which looks exactly like a browser cache problem and is not one.

```bash
scripts/build_webui.sh          # npm ci + npm run build + gzip + embed_webui.py
pio run -e display
```

Run the steps by hand if `build_webui.sh` cannot run (it needs bash, gzip and `python3`; on Windows the interpreter is
`py`, and `python3` resolves to a Microsoft Store stub that refuses to execute):

```bash
cd web && npm run build
gzip -f dist/assets/*.js dist/assets/*.css dist/*.html
cd .. && py scripts/embed_webui.py --src web/dist --out src/display/webassets
```

Check the result rather than assuming it: `src/display/webassets/web_ui.bin` must be newer than the files you edited.
Do not grep the blob for your new text — its assets are gzipped, so plaintext never matches and the check is
meaningless. Grep the decompressed bundle instead (`gunzip -c web/dist/assets/*.js.gz | grep ...`), and note that
strings inside `t\`...\`` go through the Lingui catalog, so search for a field `name` or `id` instead of a label.

## New user-visible strings need extracting, or they render as hashes

`` t`...` `` and `<Trans>` compile to a hashed message id, and the English text lives in the Lingui catalog. A string
that was never extracted has no catalog entry, so the UI renders the id — a settings tab shows `Rsjgm0` and `BRekng`
rather than its labels. After adding or editing any user-visible string:

```bash
cd web && npm run i18n:extract
```

English is the source catalog and fills in automatically; the other six languages list the new messages as missing and
fall back to English until someone translates them. Check `web/src/locales/en/messages.po` contains the new `msgid`
before rebuilding the blob, and remember that the bundle only carries the text after extraction, so grepping the
decompressed bundle for a label is a valid check only once this step has run.

## PlatformIO deletes other environments' build output

Building a second environment changes `project.checksum` and wipes `.pio/build/<other-env>`. Copy anything you need out
of `.pio/build/` before switching environment, or build and upload in one command (`pio run -e display -t upload`) so
nothing can be lost in between.

## Never flash the filesystem image to a machine in use

`pio run -t uploadfs` overwrites LittleFS, which holds profiles (`/p`) and shot history (`/h`). Firmware updates
deliberately leave that partition alone. Flash `bootloader.bin`, `partitions.bin`, `boot_app0.bin` and `firmware.bin`
only, at `0x0`, `0x8000`, `0xe000` and `0x10000`.

## Restart the display board before a large OTA

A 3.8 MB display image sent to a board that has been up for hours stops partway. The device quietly stops consuming
the body, and 15 s later the stall watchdog (`FIRMWARE_UPLOAD_STALL_MS`, `WebUIPlugin.cpp`) cancels the upload. Four
attempts died at 128 KiB, 1.44 MB, 1.13 MB and 128 KiB — the progress is random, so a repeated byte count is not a
clue about the cause.

It looks like a crash or a damaged image and is neither. The board does not reboot (no new core dump, `/api/status`
answers within a second of the failure) and the network is fine throughout (~125 KB/s on `GET /assets/*.js`). There is
simply no longer enough contiguous heap for AsyncTCP to buffer an incoming 3.8 MB while `esp_ota_write` drains it at
~40 KB/s.

Restart first, then upload immediately:

```bash
curl -s -X POST -d "restart=1" http://<ip>/api/settings
curl -X POST -H "X-GaggiMate-Firmware: 1" -H "Expect:" -H "Content-Type: application/octet-stream" \
  --data-binary @out/display-firmware.bin "http://<ip>/api/firmware/upload?target=display"
```

The restart changes no settings: every setter in `handleSettings` is guarded by `hasArg`, so a body of `restart=1`
alone writes nothing before `ESP.restart()`. A healthy upload takes ~90 s at ~42 KB/s and answers `{}` with 200; the
board then reboots itself after `FIRMWARE_RESTART_DELAY_MS`.

`-H "Expect:"` and the content type only match what the web UI's own XHR sends. Neither explains the stalls — the
128 KiB failures happened both with and without the `Expect` handshake — but sending the same request the firmware is
tested against removes two variables.

To verify an install rather than trust the 200: the asset hashes in `/` are content-derived by Vite, so fetch the
chunk holding your change and compare it to `web/dist/assets/<hash>.js.gz`. Byte-identical means the machine is
serving the build you have.

## Native tests

`pio test -e native` needs a host compiler (gcc/g++ on PATH), not the ESP32 toolchain. `test_ota_loadtest` only builds
on POSIX hosts — `test/ota_common/PosixHttpTransport.h` includes `arpa/inet.h` — so it errors on Windows regardless of
your changes.

## The toolchains are not on PATH

Three of them, each failing in a way that looks like something else.

```bash
export PATH="/c/Users/Alexi/Documents/Claude/tools/mingw64/bin:$PATH"            # gcc/g++ for native tests
export PATH="/c/Users/Alexi/Documents/Claude/tools/node-v22.23.4-win-x64:$PATH"  # node 22 for the web build
export PATH="$PATH:/c/Users/Alexi/AppData/Local/Programs/Python/Python313/Scripts" # pio
```

`node` and `npm` on PATH are **v6.10.0**, from `C:\Program Files\nodejs`. Vite and Lingui are ESM, so v6 cannot load
them — and `npm run` does not fail, it hangs. Two `node` processes sitting at 0.9 s of CPU and 1 MB of memory is what
that looks like; nothing is printed, ever. Put `tools/node-v22.23.4-win-x64` first on PATH and `lingui extract` takes
six seconds.

`py` is the Python that runs `scripts/embed_webui.py`. `python3` resolves to a Microsoft Store stub that refuses to
execute, and bash has no `python` at all.

## Do not run multi-statement perl -0pi scripts against these files

`perl -0pi -e 's{...}{...}; s{...}{...}'` with braces and newlines inside the patterns silently wrote the rest of the
script into `Controller.cpp` as source text, producing `error: 's' does not name a type`. The first substitution
applied and the remainder became replacement text.

For a multi-line edit, write the replacement to a file and splice it by line number with `awk`, or use one
substitution per `perl` invocation. Note also that most files here are **CRLF** (`core.autocrlf=true`), so a pattern
ending in `\n` does not match — use `\r?\n`.

Most, not all, and it depends on how the file was last written rather than on its type — a file this session wrote
with LF stays LF until Git next normalises it. Splitting an LF file on `\r\n` yields a single line, so a splice finds
no anchor and reports nothing to do. Check the specific file with `git ls-files --eol <path>` instead of assuming.

If you splice with Python, remember `py` is the interpreter (see the PATH section above) and it is a Windows
executable: it cannot open Git Bash paths like `/tmp`, and reports `FileNotFoundError` for a file that plainly
exists. Put scratch files on a Windows path and pass it as a raw string.
