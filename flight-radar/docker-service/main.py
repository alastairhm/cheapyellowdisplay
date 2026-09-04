"""
Flight radar backend for ESP32.

Polls the OpenSky Network REST API on a timer, keeps the latest result
cached in memory, and serves a small, pre-computed JSON payload
(bearing + distance from a fixed home location already worked out)
so the ESP32 just has to parse and display it.
"""

import asyncio
import math
import os
import time
from typing import Optional

import httpx
from fastapi import FastAPI

# ---------------------------------------------------------------------------
# Configuration (override via environment variables / docker-compose.yml)
# ---------------------------------------------------------------------------

HOME_LAT = float(os.getenv("HOME_LAT", "54.1370"))   # Dishforth, N Yorkshire
HOME_LON = float(os.getenv("HOME_LON", "-1.4200"))

# Bounding box to query. Default is roughly a 50km box around Dishforth.
BBOX_LAMIN = float(os.getenv("BBOX_LAMIN", "53.90"))
BBOX_LAMAX = float(os.getenv("BBOX_LAMAX", "54.40"))
BBOX_LOMIN = float(os.getenv("BBOX_LOMIN", "-1.90"))
BBOX_LOMAX = float(os.getenv("BBOX_LOMAX", "-0.95"))

# Only return aircraft within this radius of home (km). Set to 0 to disable.
MAX_RANGE_KM = float(os.getenv("MAX_RANGE_KM", "40"))

POLL_INTERVAL_SECONDS = int(os.getenv("POLL_INTERVAL_SECONDS", "20"))

# Optional OpenSky account (raises your rate limit from ~400 to ~4000 req/day).
# Leave blank to query anonymously.
OPENSKY_USERNAME = os.getenv("OPENSKY_USERNAME", "")
OPENSKY_PASSWORD = os.getenv("OPENSKY_PASSWORD", "")

OPENSKY_URL = "https://opensky-network.org/api/states/all"

# ---------------------------------------------------------------------------

app = FastAPI(title="Flight Radar Backend")

_cache: dict = {"flights": [], "updated_at": 0, "error": None}


def haversine_km(lat1, lon1, lat2, lon2) -> float:
    r = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlambda / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def bearing_deg(lat1, lon1, lat2, lon2) -> float:
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dlambda = math.radians(lon2 - lon1)
    x = math.sin(dlambda) * math.cos(p2)
    y = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dlambda)
    return (math.degrees(math.atan2(x, y)) + 360) % 360


async def poll_opensky():
    """Background loop: fetch OpenSky states, compute bearing/distance, cache."""
    params = {
        "lamin": BBOX_LAMIN,
        "lamax": BBOX_LAMAX,
        "lomin": BBOX_LOMIN,
        "lomax": BBOX_LOMAX,
    }
    auth = (OPENSKY_USERNAME, OPENSKY_PASSWORD) if OPENSKY_USERNAME else None

    async with httpx.AsyncClient(timeout=15) as client:
        while True:
            try:
                resp = await client.get(OPENSKY_URL, params=params, auth=auth)
                resp.raise_for_status()
                data = resp.json()
                flights = []

                for s in data.get("states", []) or []:
                    # OpenSky state vector indices:
                    # 1 callsign, 5 lon, 6 lat, 7 baro_altitude, 9 velocity,
                    # 10 true_track (heading), 8 on_ground
                    callsign = (s[1] or "").strip()
                    lon, lat = s[5], s[6]
                    if lat is None or lon is None:
                        continue
                    if s[8]:  # on_ground
                        continue

                    dist = haversine_km(HOME_LAT, HOME_LON, lat, lon)
                    if MAX_RANGE_KM and dist > MAX_RANGE_KM:
                        continue

                    flights.append({
                        "callsign": callsign or "UNKNOWN",
                        "lat": round(lat, 4),
                        "lon": round(lon, 4),
                        "alt_m": round(s[7]) if s[7] is not None else None,
                        "speed_kt": round((s[9] or 0) * 1.94384),  # m/s -> knots
                        "heading_deg": round(s[10]) if s[10] is not None else None,
                        "distance_km": round(dist, 1),
                        "bearing_deg": round(bearing_deg(HOME_LAT, HOME_LON, lat, lon)),
                    })

                flights.sort(key=lambda f: f["distance_km"])

                _cache["flights"] = flights
                _cache["updated_at"] = int(time.time())
                _cache["error"] = None

            except Exception as exc:  # keep serving stale data rather than crash
                _cache["error"] = str(exc)

            await asyncio.sleep(POLL_INTERVAL_SECONDS)


@app.on_event("startup")
async def startup():
    asyncio.create_task(poll_opensky())


@app.get("/flights")
def get_flights():
    """Small JSON payload for the ESP32: list of nearby aircraft."""
    return {
        "home": {"lat": HOME_LAT, "lon": HOME_LON},
        "updated_at": _cache["updated_at"],
        "stale": (time.time() - _cache["updated_at"]) > POLL_INTERVAL_SECONDS * 3 if _cache["updated_at"] else True,
        "count": len(_cache["flights"]),
        "flights": _cache["flights"],
        "error": _cache["error"],
    }


@app.get("/health")
def health():
    return {"ok": True, "last_update": _cache["updated_at"]}
