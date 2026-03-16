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

#define SCAN_SETTLE_MS 2  // PLL settle time after frequency change

static RX5808 rx(PIN_RX5808_RSSI, PIN_RX5808_DATA, PIN_RX5808_SELECT, PIN_RX5808_CLOCK);
static Config config;
static Webserver ws;
static Buzzer buzzer;
static Led led;
static LapTimer timers[NUM_PILOTS];
static BatteryMonitor monitor;

static TaskHandle_t xTimerTask = NULL;

static uint8_t tdmSlot      = 0;
static uint8_t prioritySlot = NUM_PILOTS;  // NUM_PILOTS = no priority active

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
    } else {
        // TDM mode: round-robin through all pilots
        slot = tdmSlot;
        tdmSlot = (tdmSlot + 1) % NUM_PILOTS;

        uint16_t freq = config.getFrequency(slot);
        if (freq == 0 || freq == POWER_DOWN_FREQ_MHZ) {
            ws.setRssi(slot, 0);
            return;
        }

        if (rx.getCurrentFrequency() != freq) {
            rx.setFrequency(freq);
            delay(SCAN_SETTLE_MS);
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

    if (prioritySlot < NUM_PILOTS && slot != prioritySlot) {
        // Non-priority pilot during priority mode:
        // Update display RSSI only — suppresses false laps from adjacent-frequency bleed-through
        timers[slot].setRssiOnly(rssi);
    } else {
        timers[slot].handleLapTimerUpdate(now, rssi);
    }

    ws.setRssi(slot, timers[slot].getRssi());
    ws.setCurrentSlot(slot);

    // Update priority slot based on current RSSI
    if (rssi >= config.getEnterRssi(slot)) {
        prioritySlot = slot;
    } else if (prioritySlot == slot && rssi < config.getExitRssi(slot)) {
        prioritySlot = NUM_PILOTS;
    }
}
