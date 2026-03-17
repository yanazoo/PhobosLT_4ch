#include <Arduino.h>

#include "debug.h"
#include "RX5808.h"
#include "config.h"
#include "led.h"
#include "webserver.h"

// Adaptive scanner for 4 pilots using 1 RX5808
// TDM mode when all pilots idle: round-robin at ~125Hz/pilot
// Priority mode when drone detected (RSSI >= enterRssi): continuous reads ~10000/s
// Adjacent-frequency bleed-through suppressed via setRssiOnly() for non-priority pilots

#define SCAN_SETTLE_MS 5  // PLL settle time after frequency change (5ms for stable settle)

static RX5808 rx(PIN_RX5808_RSSI, PIN_RX5808_DATA, PIN_RX5808_SELECT, PIN_RX5808_CLOCK);
static Config config;
static Webserver ws;
static Buzzer buzzer;
static Led led;
static LapTimer timers[NUM_PILOTS];
static BatteryMonitor monitor;

static TaskHandle_t xTimerTask = NULL;

static uint8_t tdmSlot         = 0;
static uint8_t prioritySlot    = NUM_PILOTS;  // NUM_PILOTS = no priority active
static uint8_t freqSettleCycles = 0;          // remaining cycles to skip lap detection after freq switch

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

// Main loop on Core 1: adaptive frequency scanning
void loop() {
    uint8_t slot;

    if (prioritySlot < NUM_PILOTS) {
        // Priority mode: keep reading priority pilot without switching frequency
        slot = prioritySlot;
        freqSettleCycles = 0;
    } else {
        // TDM mode: round-robin through all pilots
        slot = tdmSlot;
        tdmSlot = (tdmSlot + 1) % NUM_PILOTS;

        uint16_t freq = config.getFrequency(slot);
        if (freq == 0 || freq == POWER_DOWN_FREQ_MHZ) {
            ws.setRssi(slot, 0);
            freqSettleCycles = 0;
            return;
        }

        if (rx.getCurrentFrequency() != freq) {
            rx.setFrequency(freq);
            delay(SCAN_SETTLE_MS);
            // Allow 3 full read cycles after a frequency switch before resuming lap detection.
            // During priority mode other pilots' rssiPeak may have been raised by bleed-through;
            // multiple setRssiOnly() calls give the Kalman filter time to converge AND
            // reset any stale rssiPeak values (done inside setRssiOnly when RSSI < enterRssi).
            freqSettleCycles = 3;
        } else if (freqSettleCycles > 0) {
            freqSettleCycles--;
        }
    }

    uint16_t freq = config.getFrequency(slot);
    if (freq == 0 || freq == POWER_DOWN_FREQ_MHZ) {
        prioritySlot = NUM_PILOTS;
        return;
    }

    uint16_t rawRssi = analogRead(PIN_RX5808_RSSI);
    if (rawRssi > 2047) rawRssi = 2047;
    uint8_t rssi = rawRssi >> 3;

    uint32_t now = millis();

    if ((prioritySlot < NUM_PILOTS && slot != prioritySlot) || freqSettleCycles > 0) {
        // Two cases where we skip lap detection and update display RSSI only:
        //  1. Non-priority pilot during priority mode (adjacent-frequency bleed-through)
        //  2. First N reads after a frequency switch — Kalman filter needs time to converge;
        //     setRssiOnly() also resets stale rssiPeak that bleed-through may have raised
        timers[slot].setRssiOnly(rssi);
    } else {
        timers[slot].handleLapTimerUpdate(now, rssi);
    }

    ws.setRssi(slot, timers[slot].getRssi());
    ws.setCurrentSlot(slot);

    // Update priority slot based on current RSSI
    if (rssi >= config.getEnterRssi(slot)) {
        if (prioritySlot != slot) {
            // Entering priority mode for this pilot (or switching from another pilot).
            // Reset rssiPeak + Kalman state for all OTHER pilots so that bleed-through
            // readings accumulated while scanning at this pilot's frequency cannot
            // trigger a false lap when TDM resumes for those pilots.
            for (uint8_t j = 0; j < NUM_PILOTS; j++) {
                if (j != slot) timers[j].setRssiOnly(0);
            }
        }
        prioritySlot = slot;
    } else if (prioritySlot == slot && rssi < config.getExitRssi(slot)) {
        prioritySlot = NUM_PILOTS;
    }
}
