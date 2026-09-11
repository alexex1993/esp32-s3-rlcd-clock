# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Arduino/PlatformIO firmware for a **Waveshare ESP32-S3-RLCD-4.2** (SKU 33298/33507): a desk clock that shows local time, the current outdoor temperature/humidity/pressure, a 12-hour forecast, today's sunset and an 18650 battery gauge on a 4.2" reflective monochrome ST7305 panel (300x400 native, drawn as 400x300 landscape, 1 bpp, no backlight, no greys). **KEY steps through two more screens**: world headlines, fetched on the spot, and then a **live Telegram pager** — it holds a connection open over a SOCKS5 tunnel, long-polls for messages sent to a bot, and chirps out of the on-board speaker when one arrives.

**Use the `esp32s3-rlcd42` skill for anything hardware-level on this board** — pin traps, the ST7305 and its U8g2 constructor, a blank or shifted panel, HPM/LPM low-power mode, `qio_opi` and PSRAM, the shared I2C bus, I2S audio and the speaker-amplifier enable, the TF card, battery sensing, the USB CDC console, deep sleep, or a board that does not behave. Invoke it before guessing at a pin or a register.

The skill lives in https://github.com/alexex1993/mcu-skills (`esp32s3-rlcd42/`); if it is not listed as available, install it from there rather than working without it.

## Commands

```bash
pio run                      # build
pio run -t upload            # build + flash over Type-C
pio device monitor           # USB CDC console at 115200
pio run -t upload -t monitor # flash then attach
pio run -t clean
```

There is one env, `esp32-s3-rlcd-4_2`, so no `-e` flag is needed. `test/` and `lib/` hold only PlatformIO's stock READMEs — there is no test suite and no linter configured.

### Console commands (over `pio device monitor`)

- `T2026-09-09 21:30:00` — set the clock by hand
- `N` — force a Wi-Fi sync window now (only when built with credentials)
- `P` — one Telegram read now, radio up and down around it (only when built with a bot token and a proxy; refused while the pager screen is up, where a session is already running)
- `B` — chirp the speaker, the only way to test it: it is silent when it works and silent when it does not
- `?` — status: RTC, clock validity, battery, indoor climate (the SHTC3, read on demand — it is no longer on the panel), outdoor conditions, forecast age, sunset, headline and message counts with their ages, the proxy in use, time to next sync

`N` is time and weather only. Neither list has a timer: pressing KEY is the only thing that fetches the headlines, and the only thing besides `P` that reads Telegram. `P` exists because three separate things in `.env` can be wrong behind the pager (the proxy, its credentials, the bot token) and a button on the desk is a poor way to read the reason off the console.

### Secrets in `.env`

`.env` in the project root (gitignored) holds `SSID=` / `PWD=` / `NEWS_API=` / `TOKEN_BOT=` / `SOCKS5_HOST=` / `SOCKS5_PORT=` / `SOCKS5_USER=` / `SOCKS5_PASSWORD=`. `scripts/env_flags.py` runs as a `pre:` extra script and turns them into `-DWIFI_SSID` / `-DWIFI_PASS` / `-DNEWS_API_KEY` / `-DTELEGRAM_BOT_TOKEN` / `-DSOCKS5_HOST` / `-DSOCKS5_PORT` / `-DSOCKS5_USER` / `-DSOCKS5_PASS`. Everything is a string literal except `SOCKS5_PORT`, which the firmware wants as a number. Nothing is read from the process environment on purpose: shell `PWD` is the current directory and would silently become the password.

The macros gate different amounts of the firmware, and **every branch must still build** — check them when touching anything network-adjacent:

- **`WIFI_SSID` is the switch for the whole networking half.** NTP resync, Open-Meteo, both on-demand fetches and the `N` console command sit behind `#ifdef WIFI_SSID`; `weather.cpp`, `news.cpp` and `telegram.cpp` compile to stubs without it.
- **`NEWS_API_KEY` costs only the news screen**, which then says which key is missing. `news.cpp` gates on `WIFI_SSID && NEWS_API_KEY` (as `NEWS_ENABLED`) and so does `refreshNews()` in `main.cpp` — without both, KEY still switches screens but never brings the radio up.
- **`TELEGRAM_BOT_TOKEN` and `SOCKS5_HOST` together cost only the pager screen.** `PAGER_ENABLED` in `app.h` is the single place that decision is made — `WIFI_SSID && TELEGRAM_BOT_TOKEN && SOCKS5_HOST` — and `telegram.cpp`, `socks5.cpp`, `refreshPager()` and the `P` command all gate on it. `SOCKS5_PORT` defaults to 1080 and `SOCKS5_USER`/`SOCKS5_PASS` are optional: an empty user means the RFC 1929 exchange is never offered.

`pio run` with `.env` emptied, and with each of `NEWS_API=`, `TOKEN_BOT=`, `SOCKS5_HOST=` and the two `SOCKS5` credentials removed in turn, are the checks. All five were green when the pager landed.

## Architecture

`main.cpp` is the only module that owns state across subsystems; everything else is a flat C-style module behind `include/app.h`, with no classes and no dynamic allocation. `include/board_pins.h` is the single source of truth for pins and I2C addresses.

**Modules** (`src/`): `rtc.cpp` (PCF85063A), `shtc3.cpp` (temp/humidity — console diagnostic only since the face went over to outdoor readings), `battery.cpp` (ADC + li-ion curve), `audio.cpp` (ES8311 + I2S, one chirp and nothing else), `weather.cpp` (Open-Meteo), `news.cpp` (NewsAPI headlines), `socks5.cpp` (RFC 1928/1929 client), `telegram.cpp` (the live getUpdates session over TLS through that tunnel), `text.cpp` (folding foreign text into the panel's alphabet, plus the shared JSON-string reader), `ui.cpp` (all three screens), `main.cpp` (scheduling, screen state, time sources, console).

### A trap this codebase has already been bitten by

**A `struct` defined at file scope in a `.cpp` still has external linkage.** Its in-class member functions are implicitly inline, so two files that each define their own `Reader` emit `Reader::next()` under one mangled name and the linker keeps whichever it saw first — silently. `telegram.cpp` then called `news.cpp`'s reader, dereferenced a `WiFiClient *stream` that was never set, and panicked with `LoadProhibited`. Both are now in anonymous namespaces; **put any new file-local type in one too.** Note that the host tests could not have caught this: they compile one module, not the pair.

The check, after touching module-scope types:

```bash
B=.pio/build/esp32-s3-rlcd-4_2/src
NM=~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm
for f in $B/*.o; do $NM --defined-only "$f" | grep -v ' [a-z] ' \
  | awk -v n="$(basename $f)" '{print $3"\t"n}'; done \
  | sort | awk -F'\t' '{if ($1==p) print $1" <- "q" and "$2; p=$1; q=$2}' | sort -u
```

Anything it prints is two files fighting over one symbol. (`static` file-scope data is fine — it mangles with an `L` and is filtered out by the `grep`.)

### Constraints that shape the code

- **`lib_deps` is deliberately a single entry (U8g2).** The PCF85063A and SHTC3 drivers are written against their datasheets, and the Open-Meteo response is parsed with `strstr`/`strtof` instead of a JSON library. Keep it that way — reach for a datasheet before a dependency.
- **Init order is load-bearing.** `rtcBegin()` owns `Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL)` and must run before `shtc3Begin()`; the ESP32-S3 Arduino default of SDA 8 / SCL 9 lands on this board's I2S pins and finds nothing. Likewise `displayBegin()` calls `SPI.begin()` with the board's pins first so U8g2's own `SPI.begin()` is a no-op — skip it and the panel stays blank with no error.
- **The panel holds its image.** `uiDraw()` rebuilds a full 15 KB frame buffer and pushes one `sendBuffer()`; `loop()` only calls it when what is on screen has actually changed — every second on the clock, once a minute on a list screen, where the bar is the only thing that moves — or when something calls `forceRedraw()`.
- **The radio is the expensive part.** One window every 30 min (`ONLINE_PERIOD_MS`, 60 s retry on failure) brings Wi-Fi up, disciplines the RTC from NTP, fetches the forecast, then shuts it down. The face is frozen for the whole window, so both errands share it and timeouts are kept tight.
- **The news query is not the obvious one, and the obvious ones are dead.** Checked against the live service: NewsAPI's `top-headlines?country=ru` answers `"totalResults":0` (their docs now list `us` as the only country), and their Russian *sources* `lenta` and `rbc` are frozen — `lenta` last published in 2022. The only Russian source of theirs still moving is RT. What works is `/v2/everything?domains=bbc.com,dw.com&language=ru`: the BBC's and DW's Russian services, current, so the news screen speaks the same language as the rest of the face. Re-verify before changing the query, not after.
- **Both lists come up on the way into their screen and nowhere else.** No timer, no place in the sync window, nothing at boot. Both deliberately leave `s_nextOnlineMs` alone, so a visit neither brings the next NTP window forward nor pushes it back — and the scheduled window is held off entirely while the pager is up, because it would tear that screen's connection down under it. The cost is a blocking few seconds with the panel frozen, which is why the screen is drawn once with `busy` set before the wait. NewsAPI's free ceiling is 100 requests a day, and one press is one request — so 100 visits a day.
- **The headlines are a fetch; the pager is a session.** `withRadio()` brings the radio up for one errand and drops it — that is the news. The pager instead holds the radio *and* the TLS connection for as long as its screen is up, long-polling getUpdates back to back over HTTP/1.1 keep-alive, so a message is on the panel about a second after it is sent and the expensive handshake is paid once per visit rather than once per poll. `pagerPoll()` is a state machine driven from `loop()`: only `PG_CONNECT` and the parse block, and `PG_WAIT` merely asks the socket whether anything has arrived, so KEY stays live and the bar keeps ticking through a 25-second poll. That is also why **the pager is the one screen with no timeout back to the clock** — it is there to be watched — and why it costs perhaps 80 mA of the 18650 while open.
- **Keep-alive is what makes the framing matter.** With `Connection: close` the body ended when the socket did. Now the socket outlives the answer, so `Reader` owns the HTTP framing: `raw()` is the byte stream, `pull()` is the body, bounded by `Content-Length` or by chunk headers, and `drain()` puts the stream on the next response's status line. Get that wrong and answer two is parsed as rubbish. Both framings are implemented because neither is guaranteed.
- **The pager is proxy-only, and that is not a configuration choice.** `api.telegram.org` is not reachable from the network this clock sits on, so `telegram.cpp` goes through `socks5.cpp` unconditionally and there is no direct path to fall back to. The destination is handed to the proxy as a *domain name* (ATYP 0x03) so the name is resolved on the far side too — the local resolver cannot answer honestly about it either. Without `SOCKS5_HOST` the screen says so rather than timing out.
- **The pager is the one thing on the board that verifies a certificate.** `weather.cpp` is plain HTTP on purpose and `news.cpp` calls `setInsecure()` on purpose; `telegram.cpp` attaches the ESP-IDF root bundle with `MBEDTLS_SSL_VERIFY_REQUIRED`. The difference is the proxy: it is a third party in the middle of that connection by design, and what rides through it is a bot token — a credential that *is* the bot. The bundle costs ~64 KB of flash on a 16 MB part. It also costs stack, which is why `main.cpp` overrides the core's weak `getArduinoLoopTaskStackSize()` to 12 KB.
- **Two mbedTLS traps sit between a working handshake and a working fetch**, and both look identical from the panel — "телеграм не ответил" right after a clean TLS connect. Do not undo either.
  - `api.telegram.org` speaks **TLS 1.3**, so the server sends its NewSessionTicket *after* the handshake. The first `mbedtls_ssl_read()` of the response therefore returns `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET`, which is negative but is **not** an error: it must be read past. A plain `if (ret <= 0) end-of-body` swallows it and the answer looks empty.
  - The socket must be **non-blocking** before the handshake. mbedTLS's `net_would_block()` checks `O_NONBLOCK` first and, on a blocking socket, returns a read that merely hit `SO_RCVTIMEO` as `MBEDTLS_ERR_NET_RECV_FAILED` rather than `WANT_READ` — so every retry loop is dead code and one slow answer ends the fetch. `socks5.cpp` hands back a blocking socket because the SOCKS5 handshake wants one; `setNonBlocking()` in `telegram.cpp` flips it between the two, and from there the deadline is enforced with `select()` slices.
- **The first request after boot is `offset=-N`; everything after it carries a real offset.** The negative form returns the last N updates *without* confirming them, so opening the pager shows the recent backlog. A real advancing offset is what makes long polling block instead of replaying the same batch — and it confirms, so the backlog survives reboots only until the first live poll consumes it. After that the messages live in this file's ring and nowhere else. That is the price of a pager that is actually live, and it is how every Telegram bot works. The ring is **not** cleared between polls: each answer carries only what is new, and adds to what is on screen.
- **Only messages that *arrive* make a noise.** `pagerTakeArrival()` is a one-shot flag that the backlog on open deliberately does not set — history you asked to see is not a page coming in. `main.cpp` holds the chirp until after the redraw at the foot of the pass, so the message is on the panel by the time you hear it.
- **`telegram.cpp` parses with a real recursive-descent skipper, not `news.cpp`'s token scanner.** A reply carries a whole second message inside `reply_to_message`, and a flat scanner reads the quoted text instead of the reply. Nesting is walked exactly and unwanted values are skipped whole. It is still a streaming parse through a 256-byte window and still allocates nothing.
- **Everything the face says about the weather comes from that one request.** `current=` (temperature, humidity, surface pressure), `hourly=` (the twelve columns) and `daily=sunset` ride together. `daily` is deliberately left at its default seven days: pinning `forecast_days=1` would also clip the hourly range and leave the strip short of twelve columns late in the evening.

### Time model

The RTC holds **local** time (UTC+3, `TZ_OFFSET_SECONDS` in `app.h`); the offset is applied only when converting a UTC source. Sources, in the order `setup()` tries them: NTP → build timestamp (seeded but deliberately *not* marked valid, so the face keeps saying "CLOCK NOT SET") → console. `applyTime()` writes both the RTC and the ESP's own clock, so time survives the RTC dropping off the bus.

`s_timeValid` in `main.cpp` means "a trustworthy source has set the clock at least once" and only ever moves false→true; a failed sync window must never invalidate an RTC that kept time. `rtcReadTime()` returning false means the PCF85063A's oscillator-stop flag is set — expected on a board with an empty backup-cell holder, which loses time on every power cycle. NTP resync waits on the SNTP callback, not `getLocalTime()`, which would return the stale-but-plausible clock immediately and let a failed sync write drift back into the RTC.

### UI

`ui.cpp` draws every weather glyph from primitives (discs, boxes, lines) rather than bitmaps — no greys to hide behind at 1 bpp. Layout is fixed pixel constants at the top of the file; the band comments there are the map. Two 1-bpp-specific habits to preserve: overlapping filled shapes need a white `halo` stamp to stay distinguishable, and variable-width text sharing a row is fit-checked before drawing rather than assumed. Every label on the face is Russian. The `helv` faces carry no Cyrillic and no `_t_cyrillic` face carries the degree sign, so the two never share a string: words go in `u8g2_font_{6x12,7x13,9x15,10x20}_t_cyrillic` and readings that need `°` stay in `helv`. The outdoor row mixes both on one baseline via `drawRun()`, which carries the x cursor between segments. No `_t_cyrillic` face has a bold cut above 6x13B either, so weight comes from `drawBoldUTF8()` — a second pass one pixel over, which is what bold means at 1 bpp.

Panel orientation is `U8G2_R1`; `U8G2_R3` if it comes out upside down in an enclosure.

### The two list screens

The world headlines and the Telegram pager are one screen with two sources. Both fill `FeedItem` (text, who it is from, a time) and both are drawn by `drawFeedScreen()` in `ui.cpp`; `drawNewsScreen()` and `drawPagerScreen()` only choose the heading, the source and what to say when there is nothing.

- **The `_t_cyrillic` faces carry exactly ASCII 0x20-0x7E and Cyrillic 0x0400-0x04F9, and nothing else** — verified by decoding the font tables, not assumed. No `«»`, no `—`, no `…`, no `₽`, no `№`, no no-break space. U8g2 draws nothing and reports nothing for a glyph it does not have, so an unfiltered Russian headline — or a message somebody typed by hand — reaches the panel with its punctuation silently missing. `textSanitise()` in `text.cpp` folds everything into that alphabet on the way in, mapping the punctuation rather than dropping it (`«»`→`"`, `—`→`-`, `…`→`...`, `₽`→`Р`, Latin-1 accents to their base letter, newlines to spaces, emoji away). It lives in its own module rather than in `news.cpp` because both screens need it and `news.cpp` is a stub without an API key. **Anything new that puts foreign text on the panel goes through it too.**
- **Neither answer is ever held whole.** Each is walked once through a 256-byte window — `news.cpp` with a token scanner, `telegram.cpp` with a nesting-aware skipper — so the size of the response does not matter and neither file allocates. The JSON escape handling, `\uXXXX` and surrogate pairs included, is shared: `jsonReadString()` in `text.cpp`, which pulls bytes through a callback so it can sit above both transports.
- **Items flow rather than sitting in fixed slots.** Each takes the lines it needs and the next starts underneath, so a screenful of short ones shows more of them; the loop stops on a whole item rather than clipping one. Wrapping breaks on spaces at character boundaries, and the `...` that marks a cut item is fit-checked by trimming, not divided out. Who it is from and when go on the end of the last line, but only where that line left room.
- **An empty screen means the fetch failed, not that one is pending** — reaching the screen is what triggers the fetch, so once `busy` clears there is nothing left to wait for. The news screen says so; the pager says *why*, from `pagerError()`, because an unreachable proxy and an empty inbox look identical otherwise and are very different things to go and fix.
- **A page with no text still fills a slot.** A sticker or an uncaptioned photo is drawn as `[без текста]` with the sender and the time: something arrived, which is what a pager is for.
- **The pager's footer reports the session, not the age of the list.** "на связи", "подключаюсь...", "переподключаюсь..." — on a screen you stand in front of waiting, whether the connection is up is the thing worth knowing. And an empty pager is three different situations with three different answers: still connecting, connected with an empty inbox ("жду сообщений"), or something in the chain is broken (`pagerError()`).

## The speaker

`audio.cpp` exists for one sound: the chirp when a page arrives. It is worth knowing what it is and is not.

- **The register sequence is Espressif's `es8311` component (esp-bsp, Apache-2.0) reduced to one clock configuration**, not a dependency and not hand-derived. Waveshare's own audio example is *not* the model — it reaches for `esp_codec_dev` and the new I2C master driver, which would fight `Wire`.
- **The sample rate is 16 kHz because of the clock, not the quality.** Arduino's `ESP_I2S` sets `mclk_multiple = 256`, so 16 kHz gives MCLK = 4.096 MHz — and `(4096000, 16000)` is a row of Espressif's coefficient table. Change the rate and the dividers in `es8311Configure()` are wrong. The ES8311 is the I2S slave; the ESP generates MCLK, BCLK and LRCK.
- **GPIO46 is the amplifier enable, active high, behind a 10K pull-down.** Low means silence with everything else correct and nothing to show for it on I2C. It is raised *before* the codec is touched, which is the order the one person known to have got this chain working used. It is also a boot/ROM-log strapping pin, and GPIO45 (LRCK) is VDD_SPI — so nothing may leave either driven high into a reset, which is why every beep tears the whole chain back down.
- **Nothing is left running between beeps.** Codec reset, I2S channel freed, amplifier dropped. An idle amplifier hisses on a desk in a quiet room, and a notification is a few hundred milliseconds a day.
- **`B` at the console is the only way to test it.** Not verified on hardware — see the reporting note at the end of this section.

## Hardware notes worth knowing before editing

- The single Type-C port is the S3's native USB with **no UART bridge chip**, which is why `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` are in `build_flags`. `setup()` waits up to 2 s for host enumeration before the first print.
- `board_build.arduino.memory_type = qio_opi` is required for the N16R8's OPI PSRAM; without it PSRAM never comes up.
- Battery: GPIO4 (ADC1_CH3, so it survives Wi-Fi) behind a 200K/100K divider. `ADC_11db` attenuation is not optional — a full cell presents ~1.40 V, above the default ~0.95 V full scale. Percentage comes from an interpolated li-ion discharge table, not a straight line.
- Open-Meteo is fetched over plain HTTP on purpose: public data, no credentials, and TLS would cost ~40 KB of heap plus handshake time with the radio up.
