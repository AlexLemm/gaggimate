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

## Native tests

`pio test -e native` needs a host compiler (gcc/g++ on PATH), not the ESP32 toolchain. `test_ota_loadtest` only builds
on POSIX hosts — `test/ota_common/PosixHttpTransport.h` includes `arpa/inet.h` — so it errors on Windows regardless of
your changes.
