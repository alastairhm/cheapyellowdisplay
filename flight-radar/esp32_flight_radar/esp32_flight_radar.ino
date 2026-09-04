/*
  ESP32-2432S028R (Cheap Yellow Display) Flight Radar
  ----------------------------------------------------
  Polls the local Docker "flight-radar" service over HTTP on your LAN
  and draws a live radar screen on the board's built-in 2.8" 320x240
  ILI9341 TFT: rings, a rotating sweep, and an aircraft icon + callsign
  for each nearby flight, coloured by altitude band.

  Uses the onboard microSD slot for two things:
    - /config.txt   WiFi + server settings, so you don't have to
                     recompile to change them
    - /icons/*.bmp  aircraft icon silhouettes, recoloured per-flight
                     by altitude band and blitted at each blip

  REQUIRED LIBRARIES (Arduino Library Manager):
    - TFT_eSPI        (by Bodmer)
    - ArduinoJson     (by Benoit Blanchon)
    - SD               (bundled with the ESP32 core)

  REQUIRED TFT_eSPI CONFIGURATION (one-time, before first upload):
    TFT_eSPI needs to know this is a CYD board. In your Arduino
    libraries folder, open:
        TFT_eSPI/User_Setup_Select.h
    and:
      1. Comment out the default:      // #include <User_Setup.h>
      2. Uncomment (or add) the CYD setup line:
             #include <User_Setups/Setup303_CYD_ESP32-2432S028R.h>
         (Older TFT_eSPI versions may not ship this file - search
         "Setup42_ILI9341_ESP32 CYD" for a community setup file with
         these pins: MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, RST -1,
         BL 21.)
    Board selection in Arduino IDE: "ESP32 Dev Module".

  SD CARD SETUP:
    Format a microSD card FAT32, copy the contents of
    esp32/sd_card_files/ (config.txt + icons/) to its root, and
    edit config.txt with your real WiFi/server details before
    inserting it. The values in the sketch below are only used if
    config.txt is missing or a key isn't present in it.

  WiFi.h and HTTPClient.h ship with the ESP32 Arduino core.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>

// ---------------------------------------------------------------------
// Fallback configuration (used only if config.txt is missing/incomplete)
// ---------------------------------------------------------------------
String   WIFI_SSID     = "YOUR_WIFI_SSID";
String   WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
String   SERVER_HOST   = "192.168.1.50";
int      SERVER_PORT   = 8000;
float    RANGE_KM      = 40.0;

const char* FLIGHTS_PATH = "/flights";

const unsigned long POLL_INTERVAL_MS = 15000;  // how often to fetch new data
const unsigned long SWEEP_INTERVAL_MS = 40;    // sweep animation frame rate

// SD card is wired to the same physical SPI bus as the TFT (shared
// MISO/MOSI/SCK), selected by its own CS pin.
#define SD_CS 5
SPIClass sdSPI = SPIClass(VSPI);
bool sdAvailable = false;

// ---------------------------------------------------------------------

TFT_eSPI tft = TFT_eSPI();

// Screen geometry (landscape, 320x240)
const int SCREEN_W = 320;
const int SCREEN_H = 240;
const int CX = 130;           // radar centre x (left side, leaving room for a text panel)
const int CY = SCREEN_H / 2;  // radar centre y
const int RADIUS = 105;

enum IconType { ICON_PLANE, ICON_HELI, ICON_GENERIC };

struct Flight {
  String callsign;
  float distanceKm;
  int bearingDeg;
  int altM;
  int speedKt;
};

Flight flights[15];
int flightCount = 0;
bool dataStale = true;
unsigned long lastPoll = 0;
unsigned long lastSweepFrame = 0;
float sweepAngle = 0;

// ---------------------------------------------------------------------
// SD config
// ---------------------------------------------------------------------

void initSD() {
  sdSPI.begin(14, 12, 13, SD_CS);  // SCK, MISO, MOSI, SS - shared TFT bus pins
  sdAvailable = SD.begin(SD_CS, sdSPI);
}

void loadConfig() {
  if (!sdAvailable) return;
  File f = SD.open("/config.txt");
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();

    if (key == "WIFI_SSID") WIFI_SSID = val;
    else if (key == "WIFI_PASSWORD") WIFI_PASSWORD = val;
    else if (key == "SERVER_HOST") SERVER_HOST = val;
    else if (key == "SERVER_PORT") SERVER_PORT = val.toInt();
    else if (key == "RANGE_KM") RANGE_KM = val.toFloat();
  }
  f.close();
}

// ---------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------

void connectWiFi() {
  tft.setCursor(10, 10);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("Connecting WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
  }
}

// ---------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------

void pollFlights() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  HTTPClient http;
  String url = "http://" + SERVER_HOST + ":" + String(SERVER_PORT) + FLIGHTS_PATH;
  http.begin(url);
  http.setTimeout(8000);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return;

    dataStale = doc["stale"] | true;
    flightCount = 0;
    for (JsonVariantConst f : doc["flights"].as<JsonArrayConst>()) {
      if (flightCount >= 15) break;
      flights[flightCount].callsign   = String((const char*)(f["callsign"] | "UNKNOWN"));
      flights[flightCount].distanceKm = f["distance_km"] | 0.0;
      flights[flightCount].bearingDeg = f["bearing_deg"] | 0;
      flights[flightCount].altM       = f["alt_m"] | 0;
      flights[flightCount].speedKt    = f["speed_kt"] | 0;
      flightCount++;
    }
  }

  http.end();
}

// ---------------------------------------------------------------------
// BMP icon loading (24-bit uncompressed BMP from SD, recoloured on the fly)
// ---------------------------------------------------------------------

// Reads a 16x16 24-bit BMP and draws it centred at (cx, cy), skipping
// black pixels (transparent) and drawing every other pixel in `tint`.
void drawBmpIcon(const char* path, int cx, int cy, uint16_t tint) {
  if (!sdAvailable) return;

  File f = SD.open(path);
  if (!f) return;

  if (f.read() != 'B' || f.read() != 'M') { f.close(); return; }

  f.seek(10);
  uint32_t pixelOffset = readU32(f);
  f.seek(18);
  int32_t width  = (int32_t)readU32(f);
  int32_t height = (int32_t)readU32(f);
  f.seek(28);
  uint16_t bpp = readU16(f);
  if (bpp != 24) { f.close(); return; }

  int rowSize = ((width * 3 + 3) / 4) * 4;  // rows padded to 4 bytes
  int originX = cx - width / 2;
  int originY = cy - height / 2;

  for (int row = 0; row < height; row++) {
    // BMP rows are stored bottom-to-top
    f.seek(pixelOffset + (uint32_t)row * rowSize);
    for (int col = 0; col < width; col++) {
      uint8_t b = f.read();
      uint8_t g = f.read();
      uint8_t r = f.read();
      if (r > 20 || g > 20 || b > 20) {  // not-black -> part of the silhouette
        tft.drawPixel(originX + col, originY + (height - 1 - row), tint);
      }
    }
  }
  f.close();
}

uint32_t readU32(File& f) {
  uint32_t v;
  f.read((uint8_t*)&v, 4);
  return v;
}

uint16_t readU16(File& f) {
  uint16_t v;
  f.read((uint8_t*)&v, 2);
  return v;
}

IconType chooseIcon(int altM, int speedKt) {
  if (altM < 1000 && speedKt < 120) return ICON_HELI;
  if (speedKt > 250) return ICON_PLANE;
  return ICON_GENERIC;
}

const char* iconPath(IconType t) {
  switch (t) {
    case ICON_HELI: return "/icons/heli.bmp";
    case ICON_PLANE: return "/icons/plane.bmp";
    default: return "/icons/generic.bmp";
  }
}

// ---------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------

void drawRadarBase() {
  tft.fillScreen(TFT_BLACK);

  tft.drawCircle(CX, CY, RADIUS, TFT_DARKGREEN);
  tft.drawCircle(CX, CY, RADIUS * 2 / 3, TFT_DARKGREEN);
  tft.drawCircle(CX, CY, RADIUS / 3, TFT_DARKGREEN);

  tft.drawLine(CX - RADIUS, CY, CX + RADIUS, CY, TFT_DARKGREEN);
  tft.drawLine(CX, CY - RADIUS, CX, CY + RADIUS, TFT_DARKGREEN);

  tft.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(CX + 4, CY - RADIUS + 2);
  tft.print(String((int)RANGE_KM) + "km");
  tft.setCursor(CX + 4, CY - (RADIUS * 2 / 3) + 2);
  tft.print(String((int)(RANGE_KM * 2 / 3)) + "km");

  tft.drawLine(CX + RADIUS + 20, 0, CX + RADIUS + 20, SCREEN_H, TFT_DARKGREEN);
}

uint16_t altColor(int altM) {
  if (altM < 3000) return TFT_RED;
  if (altM < 8000) return TFT_YELLOW;
  return TFT_GREEN;
}

void drawBlips() {
  for (int i = 0; i < flightCount; i++) {
    float distFrac = flights[i].distanceKm / RANGE_KM;
    if (distFrac > 1.0) distFrac = 1.0;
    float r = distFrac * RADIUS;
    float rad = radians(flights[i].bearingDeg);

    int bx = CX + r * sin(rad);
    int by = CY - r * cos(rad);

    uint16_t c = altColor(flights[i].altM);

    if (sdAvailable) {
      IconType t = chooseIcon(flights[i].altM, flights[i].speedKt);
      drawBmpIcon(iconPath(t), bx, by, c);
    } else {
      tft.fillCircle(bx, by, 3, c);  // fallback if no SD card present
    }

    tft.setTextColor(c, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(bx + 9, by - 4);
    tft.print(flights[i].callsign);
  }
}

void drawSweep() {
  static int lastX = CX, lastY = CY;

  tft.drawLine(CX, CY, lastX, lastY, TFT_BLACK);

  float rad = radians(sweepAngle);
  int x = CX + RADIUS * sin(rad);
  int y = CY - RADIUS * cos(rad);
  tft.drawLine(CX, CY, x, y, TFT_GREEN);

  lastX = x;
  lastY = y;

  sweepAngle += 3;
  if (sweepAngle >= 360) sweepAngle = 0;
}

void drawSidePanel() {
  int panelX = CX + RADIUS + 28;
  tft.fillRect(panelX, 0, SCREEN_W - panelX, SCREEN_H, TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(panelX, 4);
  tft.print("DISHFORTH RADAR");

  tft.setCursor(panelX, 16);
  tft.setTextColor(dataStale ? TFT_RED : TFT_GREEN, TFT_BLACK);
  tft.print(dataStale ? "LINK: STALE" : "LINK: LIVE");

  tft.setTextColor(sdAvailable ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(panelX, 26);
  tft.print(sdAvailable ? "SD: OK" : "SD: MISSING");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(panelX, 38);
  tft.printf("Tracks: %d", flightCount);

  int y = 52;
  for (int i = 0; i < flightCount && i < 9; i++) {
    tft.setTextColor(altColor(flights[i].altM), TFT_BLACK);
    tft.setCursor(panelX, y);
    tft.printf("%-7s %2dkm", flights[i].callsign.c_str(), (int)flights[i].distanceKm);
    y += 10;
  }
}

// ---------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);  // landscape, USB on the right
  tft.fillScreen(TFT_BLACK);

  initSD();
  loadConfig();

  connectWiFi();
  drawRadarBase();
  pollFlights();
  drawBlips();
  drawSidePanel();
  lastPoll = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastSweepFrame >= SWEEP_INTERVAL_MS) {
    lastSweepFrame = now;
    drawSweep();
  }

  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    pollFlights();
    drawRadarBase();
    drawBlips();
    drawSidePanel();
  }
}
