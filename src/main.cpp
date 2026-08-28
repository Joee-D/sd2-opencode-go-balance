// ============================================================
//  SD2 小电视 OpenCode Go 额度显示器
//
//  硬件: ESP8266(ESP-12F) + 1.3" ST7789 240x240 (无CS)
//  参考: https://github.com/Jason6111/sd2
//  接口: GET https://opencode.ai/zen/go/v1/usage
//        （OpenCode Zen / Go 套餐官方用量接口，Bearer 鉴权）
//
//  公共基础功能来自 sd2-common（WiFi/NTP/休眠/背光/HTTP/格式化）
//  使用前先修改 src/config.h（WiFi / API Key）
//  显示三行额度: 5H / WEEK / MONTH，剩余百分比 + 重置时间
// ============================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <SD2Common.h>

#include "config.h"
#include "OpenCodeGoClient.h"
#include "OpenCodeLogo.h"

TFT_eSPI tft = TFT_eSPI();

// ---------- 公共基础组件（sd2-common）----------
static sd2::Wifi wifi;
static sd2::SleepScheduler sleepSched(SLEEP_START_HOUR, SLEEP_END_HOUR);
// SD2 固定硬件：背光 GPIO5(D1)，反相 PWM（代码自动反转，数值越大越亮）
static sd2::Backlight backlight(5, sd2::Backlight::PWM_INVERTED);

// ---------- 颜色（与 sd2-deepseek-balance / sd2-openwrt-traffic 一致）----------
#define C_BG      tft.color565(0x00, 0x00, 0x00)
#define C_BORDER  tft.color565(0x28, 0x32, 0x49)
#define C_SUB     tft.color565(0x8A, 0x94, 0xB8)
#define C_LABEL   tft.color565(0x9A, 0xA5, 0xC8)
#define C_ACCENT  tft.color565(0x4D, 0x6B, 0xFE)
#define C_WHITE   tft.color565(0xFF, 0xFF, 0xFF)
#define C_GREEN   tft.color565(0x34, 0xD3, 0x99)
#define C_RED     tft.color565(0xFF, 0x6B, 0x6B)
#define C_YELLOW  tft.color565(0xF6, 0xC3, 0x43)

// ---------- 状态 ----------
static bool bootDone = false;
static bool hasData = false;
static bool fetching = false;
static bool wifiFailShown = false;
static bool ntpDone = false;
static uint32_t lastFetchMs = 0;
static uint32_t lastSleepCheck = 0;
static uint32_t lastHeapPrint = 0;
static uint32_t bootStart = 0;

static OpenCodeGoUsage lastData;

// ---------- 界面工具 ----------
void drawText(int x, int y, const String &s, uint16_t color, const GFXfont *font) {
    tft.setFreeFont(font);
    tft.setTextColor(color);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(s, x, y);
}

int textWidth(const String &s, const GFXfont *font) {
    tft.setFreeFont(font);
    return tft.textWidth(s);
}

void drawMiniText(int x, int y, const String &s, uint16_t color) {
    tft.setTextFont(1);
    tft.setTextColor(color);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(s, x, y);
}

int miniTextWidth(const String &s) {
    tft.setTextFont(1);
    return tft.textWidth(s);
}

// ---------- ISO 时间解析 ----------
// OpenCode 接口返回 UTC 时间（如 "2026-08-28T15:42:25.791Z"）。
// 用 mktime 交给系统时区处理，再补回 UTC -> 北京的偏移。
static bool parseIsoTime(const String &iso, time_t &out) {
    if (iso.length() < 19)
        return false;

    struct tm tmv = {};
    tmv.tm_year = iso.substring(0, 4).toInt() - 1900;
    tmv.tm_mon = iso.substring(5, 7).toInt() - 1;
    tmv.tm_mday = iso.substring(8, 10).toInt();
    tmv.tm_hour = iso.substring(11, 13).toInt();
    tmv.tm_min = iso.substring(14, 16).toInt();
    tmv.tm_sec = iso.substring(17, 19).toInt();

    time_t t = mktime(&tmv) + TZ_OFFSET_SEC;
    if (t <= 0)
        return false;
    out = t;
    return true;
}

String formatReset(const String &iso) {
    if (iso.length() == 0)
        return "";
    time_t t;
    if (!parseIsoTime(iso, t))
        return "R --";
    return "R " + sd2::formatLocalTime(t, "%m-%d %H:%M");
}

// ---------- 启动页 ----------
void drawBootPage(bool fail) {
    tft.fillScreen(C_BG);
    tft.pushImage((240 - OC_LOGO_W) / 2, 90, OC_LOGO_W, OC_LOGO_H, oc_logo, 0x0000);
    const char *hint = fail ? "WiFi failed, retrying..." : "Connecting WiFi...";
    int hw = textWidth(hint, &FreeSans9pt7b);
    drawText((240 - hw) / 2, 140, hint, fail ? C_RED : C_LABEL, &FreeSans9pt7b);
}

// ---------- 主页面元素 ----------
uint16_t barColorFor(int remaining) {
    if (remaining >= 50) return C_GREEN;
    if (remaining >= 20) return C_YELLOW;
    return C_RED;
}

// 一行额度：标题/百分比在上，进度条居中，重置时间用小字放底部
void drawQuotaRow(int y, const char *label, const OpenCodeGoWindow &w) {
    tft.fillRect(18, y, 204, 64, C_BG);
    drawText(20, y + 3, label, C_LABEL, &FreeSans9pt7b);

    String pct = "--";
    uint16_t pctColor = C_RED;
    int remaining = 0;
    if (w.present && w.valid) {
        remaining = w.remaining();
        pct = String(remaining) + "%";
        pctColor = C_WHITE;
    }
    int pw = textWidth(pct, &FreeSans12pt7b);
    drawText(220 - pw, y + 2, pct, pctColor, &FreeSans12pt7b);

    tft.fillRect(20, y + 30, 204, 12, C_BORDER);
    if (w.present && w.valid && remaining > 0) {
        tft.fillRect(20, y + 30, 204L * remaining / 100, 12, barColorFor(remaining));
    }

    if (w.present && !w.valid) {
        drawMiniText(220 - miniTextWidth("INVALID"), y + 48, "INVALID", C_RED);
    } else {
        String reset = formatReset(w.resetsAt);
        if (reset.length() > 0)
            drawMiniText(220 - miniTextWidth(reset), y + 48, reset, C_SUB);
    }
}

void drawMainPage() {
    tft.startWrite();
    tft.fillScreen(C_BG);

    tft.pushImage(10, 1, OC_LOGO_W, OC_LOGO_H, oc_logo, 0x0000);
    tft.drawFastHLine(16, 40, 208, C_BORDER);

    drawQuotaRow(46, "5H", lastData.rolling);
    drawQuotaRow(110, "WEEK", lastData.weekly);
    drawQuotaRow(174, "MONTH", lastData.monthly);
    tft.drawFastHLine(20, 110, 204, C_BORDER);
    tft.drawFastHLine(20, 174, 204, C_BORDER);

    tft.endWrite();
}

// ---------- 定时休眠 ----------
bool isSleepHour() {
    return sd2::inSleepWindow(sd2::localHour(), SLEEP_START_HOUR, SLEEP_END_HOUR);
}

void updateSleep() {
#if ENABLE_SLEEP
    bool changed = sleepSched.update(sd2::localHour());
    if (changed && sleepSched.sleeping()) {
        backlight.off();
        Serial.println("Sleep mode: display off, fetch paused");
    } else if (changed && !sleepSched.sleeping()) {
        backlight.on();
        if (bootDone) drawMainPage();
        lastFetchMs = millis() - POLL_INTERVAL_MS; // 醒来立即刷新
        Serial.println("Wake up: display on");
    }
#endif
}

// ---------- 网络 ----------
void handleWiFi() {
    wifi.loop();
    if (wifi.justConnected()) {
        Serial.print("WiFi connected, IP: ");
        Serial.println(wifi.ip().c_str());
        bootDone = true;
        if (!sleepSched.sleeping()) drawMainPage();
        sd2::timeBegin(TZ_OFFSET_SEC, NTP_SERVER);
        lastFetchMs = millis() - POLL_INTERVAL_MS; // 立即拉取
    }

    if (!wifi.connected() && !bootDone && !wifiFailShown && millis() - bootStart > 30000) {
        wifiFailShown = true;
        drawBootPage(true);
    }
}

void handleFetch() {
    if (!wifi.connected() || fetching || sleepSched.sleeping()) return;
    if (millis() - lastFetchMs < POLL_INTERVAL_MS) return;

    // 证书校验前先等 NTP 时间同步
    if (!ntpDone) {
        if (sd2::timeSynced()) {
            ntpDone = true;
            Serial.printf("NTP time synced: %lu\n", (unsigned long)time(nullptr));
        }
        return;
    }
    if (isSleepHour()) return; // 休眠时段不拉取数据

    // 未填 Key 时直接提示
    if (strlen(OPENCODE_GO_API_KEY) < 20) {
        lastFetchMs = millis();
        return;
    }

    fetching = true;

    OpenCodeGoUsage d;
    ESP.wdtDisable(); // TLS 握手为阻塞操作，暂时关闭软看门狗
    bool ok = fetchOpenCodeGoUsage(d);
    ESP.wdtEnable(0);
    fetching = false;
    lastFetchMs = millis();

    if (ok) {
        hasData = true;
        lastData = d;

        Serial.printf("Quota: 5h %d%% / week %d%% / month %d%%\n",
                      d.rolling.percent, d.weekly.percent, d.monthly.percent);

        tft.startWrite(); // 单事务批量重绘，避免逐块清空闪动
        drawQuotaRow(46, "5H", d.rolling);
        drawQuotaRow(110, "WEEK", d.weekly);
        drawQuotaRow(174, "MONTH", d.monthly);
        tft.drawFastHLine(20, 110, 204, C_BORDER);
        tft.drawFastHLine(20, 174, 204, C_BORDER);
        tft.endWrite();
    } else {
        Serial.printf("Fetch failed: %s (HTTP %d)\n", d.error.c_str(), d.http_code);
    }
    Serial.printf("Free heap: %u B\n", ESP.getFreeHeap());
}

// ---------- 主程序 ----------
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println();
    Serial.println("SD2 OpenCode Go quota monitor starting...");

    tft.begin();
    tft.setRotation(0);

    backlight.begin();
    backlight.setBrightness(BRIGHTNESS);

    bootStart = millis();
    drawBootPage(false);

    wifi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
    handleWiFi();
    handleFetch();

    if (millis() - lastSleepCheck >= 1000) {
        lastSleepCheck = millis();
        updateSleep();
    }
    if (millis() - lastHeapPrint >= 10000) {
        lastHeapPrint = millis();
        Serial.printf("Free heap: %u B\n", ESP.getFreeHeap());
    }

    delay(20);
}
