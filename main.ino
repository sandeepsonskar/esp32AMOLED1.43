#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ---- Display pins ----
#define LCD_CS    9
#define LCD_CLK   10
#define LCD_SIO0  11
#define LCD_SIO1  12
#define LCD_SIO2  13
#define LCD_SIO3  14
#define LCD_EN    42
#define LCD_RST   21

// ---- RTC pins (PCF85063) ----
#define RTC_SDA   47
#define RTC_SCL   48
#define RTC_INT   15       // not needed for a clock (alarm/timer output)
#define PCF_ADDR  0x51

// ---- Time setting ----
// Set to 1, upload once to write the PC's time into the RTC, then set to 0 and upload again.
#define FORCE_SET_TIME   0
#define TIME_ADJUST_SEC  12   // compile-to-boot delay compensation

#define LCD_WIDTH  466
#define LCD_HEIGHT 466
#define CX 233
#define CY 233

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_CLK, LCD_SIO0, LCD_SIO1, LCD_SIO2, LCD_SIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RST, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

uint16_t cBG, cDigit, cTrack, cAmPm, cDot;

// ---------- RTC (PCF85063) ----------
uint8_t toBCD(int v)   { return ((v / 10) << 4) | (v % 10); }
int     fromBCD(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }

// returns 0 = ok, 1 = ok but oscillator-stop flag set (time invalid), -1 = I2C error
int readRTC(int &h, int &m, int &s) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)PCF_ADDR, (uint8_t)3) < 3) return -1;
  uint8_t sec = Wire.read();
  uint8_t min = Wire.read();
  uint8_t hr  = Wire.read();
  s = fromBCD(sec & 0x7F);
  m = fromBCD(min & 0x7F);
  h = fromBCD(hr & 0x3F);          // 24-hour mode
  return (sec & 0x80) ? 1 : 0;
}

void setRTCFromCompileTime() {
  int hh, mm, ss, dd, yy;
  char mon[4];
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  sscanf(__DATE__, "%3s %d %d", mon, &dd, &yy);
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int mo = (strstr(months, mon) - months) / 3 + 1;

  ss += TIME_ADJUST_SEC;
  mm += ss / 60; ss %= 60;
  hh += mm / 60; mm %= 60;
  hh %= 24;

  // weekday (0 = Sunday)
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = yy;
  if (mo < 3) y--;
  int wd = (y + y / 4 - y / 100 + y / 400 + t[mo - 1] + dd) % 7;

  Wire.beginTransmission(PCF_ADDR);
  Wire.write(0x04);
  Wire.write(toBCD(ss));          // writing seconds clears the OS flag
  Wire.write(toBCD(mm));
  Wire.write(toBCD(hh));
  Wire.write(toBCD(dd));
  Wire.write(wd);
  Wire.write(toBCD(mo));
  Wire.write(toBCD(yy % 100));
  Wire.endTransmission();
  Serial.printf("RTC set to %02d:%02d:%02d\n", hh, mm, ss);
}

void initRTC() {
  Wire.begin(RTC_SDA, RTC_SCL);
  Wire.setClock(400000);
  pinMode(RTC_INT, INPUT_PULLUP);

  Wire.beginTransmission(PCF_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("PCF85063 NOT found at 0x51 (check SDA=47, SCL=48)");
    return;
  }
  // Control_1: make sure the clock is running (STOP=0) and in 24-hour mode
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(0x00);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)PCF_ADDR, (uint8_t)1);
  uint8_t ctrl = Wire.read();
  if (ctrl & 0x22) {
    Wire.beginTransmission(PCF_ADDR);
    Wire.write(0x00);
    Wire.write(ctrl & ~0x22);
    Wire.endTransmission();
  }

  int h, m, s;
  int st = readRTC(h, m, s);
  if (FORCE_SET_TIME || st == 1) setRTCFromCompileTime();
}

// ---------- Big smooth 7-segment digits ----------
const int DW = 60, DH = 120, T = 12, G = 2, GAP = 12, CW = 22;

const uint8_t segMask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void hSeg(int xa, int xb, int yc, uint16_t c) {
  int h = T / 2;
  gfx->fillRect(xa + h, yc - h, xb - xa - T, T, c);
  gfx->fillTriangle(xa, yc, xa + h, yc - h, xa + h, yc + h, c);
  gfx->fillTriangle(xb, yc, xb - h, yc - h, xb - h, yc + h, c);
}

void vSeg(int xc, int ya, int yb, uint16_t c) {
  int h = T / 2;
  gfx->fillRect(xc - h, ya + h, T, yb - ya - T, c);
  gfx->fillTriangle(xc, ya, xc - h, ya + h, xc + h, ya + h, c);
  gfx->fillTriangle(xc, yb, xc - h, yb - h, xc + h, yb - h, c);
}

void drawDigit(int x, int y, int d, uint16_t c) {
  uint8_t m = segMask[d];
  int h = T / 2;
  int xl = x + h, xr = x + DW - h;
  int yt = y + h, ym = y + DH / 2, yb = y + DH - h;
  if (m & 1)  hSeg(xl + G, xr - G, yt, c);   // top
  if (m & 2)  vSeg(xr, yt + G, ym - G, c);   // upper right
  if (m & 4)  vSeg(xr, ym + G, yb - G, c);   // lower right
  if (m & 8)  hSeg(xl + G, xr - G, yb, c);   // bottom
  if (m & 16) vSeg(xl, ym + G, yb - G, c);   // lower left
  if (m & 32) vSeg(xl, yt + G, ym - G, c);   // upper left
  if (m & 64) hSeg(xl + G, xr - G, ym, c);   // middle
}

void drawTime(int h12, int mm, bool colonOn) {
  int hn = (h12 >= 10) ? 2 : 1;
  int total = hn * DW + (hn - 1) * GAP + GAP + CW + GAP + 2 * DW + GAP;
  int x = CX - total / 2;
  int y = CY - 10 - DH / 2;

  if (hn == 2) { drawDigit(x, y, h12 / 10, cDigit); x += DW + GAP; }
  drawDigit(x, y, h12 % 10, cDigit); x += DW + GAP;

  if (colonOn) {
    gfx->fillCircle(x + CW / 2, y + DH * 3 / 10, 7, cDigit);
    gfx->fillCircle(x + CW / 2, y + DH * 7 / 10, 7, cDigit);
  }
  x += CW + GAP;

  drawDigit(x, y, mm / 10, cDigit); x += DW + GAP;
  drawDigit(x, y, mm % 10, cDigit);
}

// ---------- Rings ----------
void drawDots() {
  for (int i = 0; i < 60; i++) {
    float a = (i * 6 - 90) * DEG_TO_RAD;
    int x = CX + (int)(192 * cos(a));
    int y = CY + (int)(192 * sin(a));
    gfx->fillCircle(x, y, (i % 5 == 0) ? 4 : 2, cDot);
  }
}

void drawSecondsRing(float prog) {
  const int R = 219;
  for (int w = -6; w <= 6; w++) gfx->drawCircle(CX, CY, R + w, cTrack);
  int n = (int)(prog * 360);
  for (int a = 0; a <= n; a++) {
    float k = a / 360.0f;
    uint16_t col = gfx->color565(0, (int)(40 + 70 * k), (int)(120 + 135 * k));
    float r = (a - 90) * DEG_TO_RAD;
    gfx->fillCircle(CX + (int)(R * cos(r)), CY + (int)(R * sin(r)), 6, col);
  }
}

void drawCenteredText(const char *text, uint8_t size, uint16_t color, int yCenter) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((LCD_WIDTH - w) / 2 - x1, yCenter - h / 2 - y1);
  gfx->print(text);
}

// ---------- Main ----------
void setup() {
  Serial.begin(115200);

  pinMode(LCD_EN, OUTPUT);
  digitalWrite(LCD_EN, HIGH);
  delay(300);

  if (!gfx->begin()) {
    Serial.println("Display/canvas init failed! Check PSRAM setting.");
    while (true) delay(1000);
  }
  panel->setBrightness(200);

  cBG    = RGB565_BLACK;
  cDigit = RGB565_WHITE;
  cTrack = gfx->color565(0, 0, 50);
  cAmPm  = gfx->color565(80, 160, 255);
  cDot   = RGB565_WHITE;

  initRTC();
  gfx->fillScreen(cBG);
  gfx->flush();
}

void loop() {
  static int hh = 12, mm = 0, ss = 0, lastSec = -1;
  static unsigned long lastPoll = 0, secMs = 0;
  unsigned long now = millis();

  if (now - lastPoll >= 100) {            // poll the RTC 10 times a second
    lastPoll = now;
    int h, m, s;
    if (readRTC(h, m, s) >= 0) {
      hh = h; mm = m; ss = s;
      if (ss != lastSec) { lastSec = ss; secMs = now; }
    }
  }

  float frac = (now - secMs) / 1000.0f;   // smooth sweep between seconds
  if (frac > 1.0f) frac = 1.0f;
  float prog = (ss + frac) / 60.0f;

  int h12 = hh % 12;
  if (h12 == 0) h12 = 12;
  bool pm = (hh >= 12);

  gfx->fillScreen(cBG);
  drawSecondsRing(prog);
  drawDots();
  drawTime(h12, mm, frac < 0.5f);         // colon blinks every second
  drawCenteredText(pm ? "PM" : "AM", 3, cAmPm, CY + 105);
  gfx->flush();
}
