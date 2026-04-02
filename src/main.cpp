#include <Arduino.h>

#include "debug.h"
#include "RX5808.h"
#include "config.h"
#include "led.h"
#include "webserver.h"

// Pure round-robin TDM scanner for 4 pilots using 1 RX5808.
// All pilots are scanned equally in strict rotation.
// After each frequency change, delay(SCAN_SETTLE_MS) waits for PLL to lock —
// no extra skip cycles needed. Every read after the delay goes straight into
// handleLapTimerUpdate so rssiPeak is never forcibly reset between rounds.

#define SCAN_SETTLE_MS    8   // PLL settle time (ms). RX5808 typical lock = 3.5 ms.
                              // 3 ms: TDM round ~16 ms but PLL not fully settled → noisy RSSI → false laps.
                              // 5 ms: TDM round ~24 ms, PLL settled → good balance of accuracy vs speed.
                              // 6 ms: cleaner RSSI but inter-pilot crosstalk increased → worse overall.

// Dominance check: this pilot's raw RSSI must exceed all other pilots' filtered RSSI by at least
// DOMINANCE_DELTA to allow peak capture. Prevents RF front-end saturation from a nearby drone
// (which raises all channels to ~100) from generating false lap counts on uninvolved pilots.
// Value of 10: genuine pass raw ~130 vs saturation raw ~120 (margin=10).
// Combined with PEAK_CONFIRM_SAMPLES=2 in laptimer.cpp, the first-round TDM ordering
// issue (where the real pilot hasn't been read yet) is handled without blocking
// simultaneous multi-pilot detection.
#define DOMINANCE_DELTA  10

static RX5808 rx(PIN_RX5808_RSSI, PIN_RX5808_DATA, PIN_RX5808_SELECT, PIN_RX5808_CLOCK);
static Config config;
static Webserver ws;
static Buzzer buzzer;
static Led led;
static LapTimer timers[NUM_PILOTS];
static BatteryMonitor monitor;

static TaskHandle_t xTimerTask = NULL;

static uint8_t slot = 0;

// Parallel task on Core 0: handles WiFi, web, buzzer, LED, battery, EEPROM
static void parallelTask(void *pvArgs) {
    for (;;) {
        uint32_t currentTimeMs = millis();
        buzzer.handleBuzzer(currentTimeMs);
        led.handleLed(currentTimeMs);
        ws.handleWebUpdate(currentTimeMs);
        config.handleEeprom(currentTimeMs);
        monitor.checkBatteryState(currentTimeMs, config.getAlarmThreshold());
        buzzer.handleBuzzer(currentTimeMs);
        led.handleLed(currentTimeMs);
    }
}

static void initParallelTask() {
    disableCore0WDT();
    xTaskCreatePinnedToCore(parallelTask, "parallelTask", 4096, NULL, 0, &xTimerTask, 0);
}

void setup() {
    DEBUG_INIT;
    DEBUG("PhobosLT 4ch starting...\n");

    config.init();
    rx.init();
    buzzer.init(PIN_BUZZER, BUZZER_INVERTED);
    led.init(PIN_LED, false);

    for (uint8_t i = 0; i < NUM_PILOTS; i++) {
        timers[i].init(&config, i, &buzzer, &led);
    }

    monitor.init(PIN_VBAT, VBAT_SCALE, VBAT_ADD, &buzzer, &led);
    ws.init(&config, timers, &monitor, &buzzer, &led);

    led.on(400);
    buzzer.beep(200);

    initParallelTask();

    DEBUG("PhobosLT 4ch ready. Scanning %d pilots.\n", NUM_PILOTS);
}

// Main loop on Core 1: pure round-robin TDM scanning
void loop() {
    slot = (slot + 1) % NUM_PILOTS;

    uint16_t freq = config.getFrequency(slot);
    if (freq == 0 || freq == POWER_DOWN_FREQ_MHZ) {
        ws.setRssi(slot, 0);
        return;
    }

    if (rx.getCurrentFrequency() != freq) {
        rx.setFrequency(freq);
        delay(SCAN_SETTLE_MS);  // wait for PLL to lock
    }

    // Average 3 consecutive reads. EMA in laptimer.cpp handles inter-round smoothing.
    uint32_t rssiSum = 0;
    for (int r = 0; r < 3; r++) rssiSum += analogRead(PIN_RX5808_RSSI);
    uint16_t rawRssi = rssiSum / 3;
    if (rawRssi > 2047) rawRssi = 2047;
    uint8_t rssi = rawRssi >> 3;

    // Dominance check: compare this pilot's raw RSSI against other pilots' last filtered RSSI.
    // If another pilot's filtered value is nearly as high, RF saturation is likely — suppress
    // peak capture for this pilot to prevent false laps.
    uint8_t maxOtherRssi = 0;
    for (uint8_t i = 0; i < NUM_PILOTS; i++) {
        if (i != slot) maxOtherRssi = max(maxOtherRssi, timers[i].getRssi());
    }
    bool isDominant = (rssi >= (uint16_t)maxOtherRssi + DOMINANCE_DELTA);

    timers[slot].handleLapTimerUpdate(millis(), rssi, isDominant);

    ws.setRssi(slot, timers[slot].getRssi());
    ws.setCurrentSlot(slot);
}
