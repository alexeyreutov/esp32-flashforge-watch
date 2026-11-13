#pragma once
#include <Adafruit_ST7789.h>

// Waveshare ESP32-S3-LCD-2: display and SD share the SPI bus.
static Adafruit_ST7789 lcd(&SPI, 45, 42, -1);
static bool frameVisible = false;
static void beginScreen() {
  pinMode(41, OUTPUT); digitalWrite(41, HIGH); // deselect SD
  SPI.begin(39, 40, 38, 45);
  lcd.init(240, 320, SPI_MODE0);
  lcd.setRotation(1);
  lcd.fillScreen(ST77XX_BLACK);
  lcd.setTextWrap(false);
  pinMode(1, OUTPUT); digitalWrite(1, HIGH);
}

static void paintCamera(const uint8_t *rgb, int w, int h) {
  constexpr int viewWidth = 320, viewHeight = 186;
  int dw = min(viewWidth, viewHeight*w/h), dh = max(1, dw*h/w);
  int x0 = (viewWidth-dw)/2, y0 = (viewHeight-dh)/2;
  static int oldWidth = 0;
  if (!frameVisible || oldWidth != dw) lcd.fillRect(0,0,320,viewHeight,ST77XX_BLACK);
  oldWidth = dw;
  uint16_t line[viewWidth];
  lcd.startWrite();
  lcd.setAddrWindow(x0,y0,dw,dh);
  for (int y=0;y<dh;y++) {
    const uint8_t *row = rgb + 3*w*(y*h/dh);
    for (int x=0;x<dw;x++) {
      const uint8_t *p = row + 3*(x*w/dw);
      line[x] = lcd.color565(p[0],p[1],p[2]);
    }
    lcd.writePixels(line,dw);
  }
  lcd.endWrite();
  frameVisible = true;
}

static void screenStatus(const String &phase, bool printing, int layer,
                         bool hasFrame, bool warning, const String &password) {
  if (!hasFrame && frameVisible) {
    lcd.fillRect(0,0,320,186,ST77XX_BLACK);
    frameVisible = false;
  }
  if (!hasFrame) {
    lcd.setTextSize(2); lcd.setTextColor(ST77XX_WHITE,ST77XX_BLACK);
    lcd.setCursor(4,34); lcd.print("Wi-Fi: Flashforge-Watch");
    lcd.setCursor(4,64); lcd.print("Key: " + password);
    lcd.setCursor(4,94); lcd.print("Setup: 192.168.4.1");
  }
  lcd.fillRect(0,186,320,54,ST77XX_BLACK);
  lcd.setTextColor(warning ? ST77XX_YELLOW : ST77XX_WHITE);
  lcd.setTextSize(2); lcd.setCursor(4,190);
  String title = printing ? "Printing" : "Flashforge Creator 5 Pro";
  if (printing && layer>=0) title += "  Layer " + String(layer);
  lcd.print(title);
  String detail = phase.substring(0,phase.indexOf(" |"));
  lcd.setCursor(4,216); lcd.setTextSize(detail.length()>25 ? 1 : 2);
  lcd.setTextWrap(true); lcd.print(detail); lcd.setTextWrap(false);
}
