# Changelog

All notable changes to this repository are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added

- Top-level `README.md` linking out to each project.
- `CLAUDE.md` with guidance for Claude Code on repo structure and the
  `flight-radar` architecture.
- This changelog.

## [0.1.0] - 2026-09-04

### Added

- `flight-radar/` — Docker/FastAPI backend that polls OpenSky and serves
  nearby-aircraft data, plus an ESP32 (CYD) sketch that renders a live
  radar screen from it.
