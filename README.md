# Cheap Yellow Display projects

Projects built around the **ESP32-2432S028R "Cheap Yellow Display" (CYD)** —
a 2.8" 320x240 ILI9341 TFT with onboard microSD, USB, and no wiring
required to get a screen running.

## Projects

- [`flight-radar/`](flight-radar/README.md) — local flight tracker. A
  Docker/FastAPI backend polls the OpenSky Network API and caches nearby
  aircraft; the CYD polls that backend over LAN and draws a live radar
  screen (range rings, sweep, altitude-coloured blips).

See each project's own README for setup and usage details.

## Contributing

Changes are made on a branch and merged via pull request. See
[CHANGELOG.md](CHANGELOG.md) for a history of notable changes.
