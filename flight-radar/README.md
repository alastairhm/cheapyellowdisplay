# Flight radar: Docker backend + ESP32 client

Local flight tracking for Dishforth. A Docker container on your home
server polls the OpenSky Network API and caches nearby aircraft; the
ESP32 polls that container over your LAN instead of hitting OpenSky
directly.

## 1. Docker service (`docker-service/`)

```
cd docker-service
docker compose up -d --build
```

This starts a FastAPI app on port 8000 that:
- Polls OpenSky's `states/all` endpoint every 20s (configurable) for a
  bounding box around Dishforth
- Computes distance (km) and bearing (deg) from your home location for
  each aircraft
- Serves the result at `GET /flights`, e.g.:

```json
{
  "home": {"lat": 54.137, "lon": -1.42},
  "updated_at": 1735600000,
  "stale": false,
  "count": 2,
  "flights": [
    {
      "callsign": "BA1234",
      "lat": 54.20,
      "lon": -1.35,
      "alt_m": 10600,
      "speed_kt": 430,
      "heading_deg": 95,
      "distance_km": 8.2,
      "bearing_deg": 41
    }
  ],
  "error": null
}
```

Config (env vars, set in `docker-compose.yml`):
- `HOME_LAT` / `HOME_LON` — your reference point (defaults to Dishforth)
- `BBOX_*` — the OpenSky query bounding box
- `MAX_RANGE_KM` — filter flights further than this from home (0 disables)
- `POLL_INTERVAL_SECONDS` — how often to hit OpenSky
- `OPENSKY_USERNAME` / `OPENSKY_PASSWORD` — optional, raises the free
  rate limit from ~400 to ~4000 requests/day. Sign up free at
  opensky-network.org.

Check it's working: `curl http://<server-ip>:8000/flights`

## 2. ESP32 sketch (`esp32/esp32_flight_radar.ino`)

Written for the **ESP32-2432S028R "Cheap Yellow Display" (CYD)** —
2.8" 320x240 ILI9341 TFT built onto the board itself, no wiring
needed.

Draws a live radar: range rings, a rotating sweep, and a blip +
callsign for each aircraft (colour-coded by altitude: red < 3000m,
yellow < 8000m, green above). A side panel lists tracked flights and
shows whether the backend's data is live or stale.

**Libraries** (Arduino Library Manager):
- `TFT_eSPI` (by Bodmer)
- `ArduinoJson` (by Benoit Blanchon)

**One-time TFT_eSPI setup** — this library needs to be told it's
driving a CYD board *before* your sketch will show anything:
1. Find your Arduino libraries folder, open `TFT_eSPI/User_Setup_Select.h`
2. Comment out the default `#include <User_Setup.h>`
3. Uncomment `#include <User_Setups/Setup303_CYD_ESP32-2432S028R.h>`
   (recent TFT_eSPI versions ship this). If your version doesn't have
   it, search "Setup42_ILI9341_ESP32 CYD" for a community setup file —
   the pins needed are MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, RST -1,
   BL 21.
4. Board setting in Arduino IDE: **ESP32 Dev Module**

It polls `http://<SERVER_HOST>:8000/flights` every 15s and redraws
the radar. The sweep animates independently at ~25fps between polls
so it doesn't sit still waiting on the network.

### SD card: config + aircraft icons

The CYD's onboard microSD slot (shares the TFT's SPI bus, CS on
GPIO 5) is used for two things:

**1. Settings, without recompiling.** Format a card FAT32, copy
`esp32/sd_card_files/config.txt` to its root, and edit it:

```
WIFI_SSID=YourNetwork
WIFI_PASSWORD=YourPassword
SERVER_HOST=192.168.1.50
SERVER_PORT=8000
RANGE_KM=40
```

Any key you omit falls back to the placeholder value baked into the
sketch. If no SD card is present (or `config.txt` is missing), the
sketch just uses those baked-in defaults — you'd only need to edit
and reflash in that case.

**2. Aircraft icons.** Copy `esp32/sd_card_files/icons/` (three
16x16 BMPs: `plane.bmp`, `heli.bmp`, `generic.bmp`) to the card
alongside `config.txt`. These are plain white silhouettes on a black
background — the sketch treats black as transparent and recolours
every non-black pixel to match the flight's altitude band (red/
yellow/green), so you get one icon file per aircraft *type*, not per
colour. Icon is chosen heuristically from speed/altitude: low and
slow → helicopter, fast → airliner, everything else → generic
diamond. If the SD card isn't present, blips fall back to plain
coloured dots — nothing breaks.

Want different icon shapes? Any 16x16, 24-bit uncompressed BMP with
a black background works — edit and re-export with any image editor,
or regenerate with Pillow (`Image.save(path, format='BMP')` on a
16x16 RGB image saves the right format automatically).

Full card layout:
```
/config.txt
/icons/plane.bmp
/icons/heli.bmp
/icons/generic.bmp
```

## Notes

- OpenSky's free tier is rate-limited; the container polls on a timer
  and caches, so any number of ESP32s (or other clients) can hit
  `/flights` freely without touching OpenSky's limit themselves.
- No TLS/cert handling needed on the ESP32 — it only talks plain HTTP
  to your own container on the LAN.
- If `/flights` returns `"stale": true`, the container's last OpenSky
  poll failed (check `"error"` field and the container logs).
