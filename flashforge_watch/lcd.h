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
  constexpr int dw = 320, dh = 240;
  const int cropW = min(w, h*dw/dh), cropH = min(h, w*dh/dw);
  const int left = (w-cropW)/2, top = (h-cropH)/2;
  uint16_t line[dw];
  lcd.startWrite();
  lcd.setAddrWindow(0,0,dw,dh);
  for (int y=0;y<dh;y++) {
    const uint8_t *row = rgb + 3*w*(top+y*cropH/dh);
    for (int x=0;x<dw;x++) {
      const uint8_t *p = row + 3*(left+x*cropW/dw);
      line[x] = lcd.color565(p[0],p[1],p[2]);
    }
    lcd.writePixels(line,dw);
  }
  lcd.endWrite();
  frameVisible = true;
}

static void screenStatus(bool hasFrame, uint16_t border, int percent,
                         int secondsLeft, const String &password, const String &phase) {
  static bool setupVisible = false;
  static uint16_t lastBorder = 0;
  static String lastPhase;
  if (!hasFrame) {
    if (setupVisible && !frameVisible && border==lastBorder && phase==lastPhase) return;
    if (!setupVisible || frameVisible) {
      lcd.fillScreen(ST77XX_BLACK);
      lcd.setTextSize(2); lcd.setTextColor(ST77XX_WHITE,ST77XX_BLACK);
      lcd.setCursor(4,34); lcd.print("Wi-Fi: Flashforge-Watch");
      lcd.setCursor(4,64); lcd.print("Key: " + password);
      lcd.setCursor(4,94); lcd.print("Setup: 192.168.4.1");
    }
    frameVisible = false;
    lcd.fillRect(4,130,312,88,ST77XX_BLACK);
    lcd.setTextWrap(true); lcd.setCursor(6,136); lcd.print(phase); lcd.setTextWrap(false);
    setupVisible = true; lastPhase=phase; lastBorder=border;
  } else {
    setupVisible = false;
    String caption;
    if (percent >= 0) caption = String(percent) + "%";
    if (secondsLeft > 0) {
      if (caption.length()) caption += "  ";
      unsigned minutes = (unsigned(secondsLeft)+59)/60;
      char eta[24];
      snprintf(eta,sizeof eta,"ETA %u:%02u",minutes/60,minutes%60);
      caption += eta;
    }
    lcd.setTextSize(2); lcd.setTextColor(ST77XX_WHITE,ST77XX_BLACK);
    lcd.setCursor(6,6); lcd.print(caption);
  }
  if (border || !hasFrame) for (int i=0;i<3;i++) lcd.drawRect(i,i,320-2*i,240-2*i,border);
}
