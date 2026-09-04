# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository overview

This repo holds hardware/IoT projects for the **ESP32-2432S028R "Cheap Yellow Display" (CYD)** board — a 2.8" 320x240 ILI9341 TFT with onboard microSD. Currently it contains one project:

- `flight-radar/` — local flight tracker: a Python/FastAPI Docker backend that polls the OpenSky Network API, paired with an Arduino (`.ino`) sketch that polls the backend over LAN and draws a live radar on the CYD's screen.

There is no repo-wide build system; each project under a top-level directory is self-contained with its own toolchain (Docker for the backend, Arduino IDE for the ESP32 sketch).

## flight-radar architecture

Two independent halves that only talk to each other over a plain HTTP LAN request — there is no shared code or schema file between them, so if you change the JSON shape on one side you must manually update the other:

1. **`flight-radar/docker-service/main.py`** — single-file FastAPI app.
   - A background `asyncio` task (`poll_opensky`, started in `@app.on_event("startup")`) polls OpenSky's `states/all` on a timer (`POLL_INTERVAL_SECONDS`), filters to a bounding box + `MAX_RANGE_KM`, computes `distance_km`/`bearing_deg` from a fixed home lat/lon (haversine + bearing formulas at the top of the file), and stores the result in a module-level `_cache` dict.
   - `GET /flights` just serves `_cache`, marking `stale: true` if the last successful poll is older than `3 * POLL_INTERVAL_SECONDS`. On poll failure the cache is left as-is (serves stale data rather than crashing) and `error` is populated.
   - All config is env vars with defaults baked in (see `docker-compose.yml`); no config file, no persistence — cache is in-memory and resets on restart.
   - Run/deploy: `cd flight-radar/docker-service && docker compose up -d --build`. No test suite, linter, or package manifest beyond `requirements.txt`.

2. **`flight-radar/esp32/esp32_flight_radar.ino`** — Arduino sketch, single file, no build system beyond the Arduino IDE/CLI.
   - Polls `http://<SERVER_HOST>:<SERVER_PORT>/flights` every `POLL_INTERVAL_MS` (15s), parses with ArduinoJson, and redraws: radar rings via `drawRadarBase()`, per-aircraft blips via `drawBlips()`, and a text side panel via `drawSidePanel()`.
   - The sweep line (`drawSweep()`) animates on its own faster timer (`SWEEP_INTERVAL_MS`) independent of network polls, so the radar looks alive between fetches.
   - Runtime config (WiFi creds, server host/port, range) is loaded from `/config.txt` on the microSD card at boot (`loadConfig()`), overriding the hardcoded placeholder defaults at the top of the file. Missing SD card or missing keys silently fall back to those defaults — nothing errors.
   - Aircraft icons are hand-parsed 24-bit uncompressed BMPs read directly from SD (`drawBmpIcon()` — no BMP library), recolored per-pixel by altitude band (red/yellow/green, from `altColor()`) since the source files are white-on-black silhouettes. Icon shape is chosen heuristically in `chooseIcon()` from speed/altitude (slow+low → heli, fast → plane, else generic). No SD card present → blips fall back to plain filled circles.
   - **Required one-time TFT_eSPI setup before the sketch will render anything**: in the TFT_eSPI library's `User_Setup_Select.h`, disable the default `User_Setup.h` include and enable the CYD board setup (`Setup303_CYD_ESP32-2432S028R.h`, or an equivalent community setup with pins MISO 12/MOSI 13/SCLK 14/CS 15/DC 2/RST -1/BL 21). This lives in the installed library, not in this repo — see `flight-radar/README.md` for full steps.
   - Board: "ESP32 Dev Module" in Arduino IDE. Libraries needed: `TFT_eSPI` (Bodmer), `ArduinoJson` (Blanchon), `SD` (bundled with ESP32 core).
   - No automated tests — verification is "flash it and look at the screen."

See `flight-radar/README.md` for the full `/flights` JSON schema, env var reference, and SD card file layout (`/config.txt`, `/icons/*.bmp`).
