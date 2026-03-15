#include <Arduino.h>

#include "debug.h"
#include "RX5808.h"
#include "config.h"
#include "led.h"
#include "webserver.h"
#include <ElegantOTA.h>

// Time-Division Multiplexing Scanner for 4 pilots using 1 RX5808
// Cycle: For each pilot, set frequency -> wait settle -> read RSSI -> process lap
// Total cycle ~4 x 35ms = 140ms (~7Hz per pilot update rate)

#define SCAN_SETTLE_MS 35  // PLL settle time after frequency change

static RX5808 rx(PIN_RX5808_RSSI, PIN_RX5808_DATA, PIN_RX5808_SELECT, PIN_RX5808_CLOCK);
static Config config;
static Webserver ws;
static Buzzer buzzer;
static Led led;
static LapTimer timers[NUM_PILOTS];
static BatteryMonitor monitor;

static TaskHandle_t xTimerTask = NULL;

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

// Main loop on Core 1: Time-division frequency scanning
void loop() {
    for (uint8_t slot = 0; slot < NUM_PILOTS; slot++) {
        uint16_t freq = config.getFrequency(slot);

        // Skip disabled pilots (frequency = 0 or power-down sentinel)
        if (freq == 0 || freq == POWER_DOWN_FREQ_MHZ) {
            ws.setRssi(slot, 0);
            continue;
        }

        // Switch RX5808 to this pilot's frequency
        uint16_t currentFreq = rx.getCurrentFrequency();
        if (currentFreq != freq) {
            rx.setFrequency(freq);
            delay(SCAN_SETTLE_MS);  // Wait for PLL to lock
        }

        // Read RSSI (recentSetFreqFlag is cleared by settle time)
        // Read raw analog directly since we manage timing ourselves
        uint16_t rawRssi = analogRead(PIN_RX5808_RSSI);
        if (rawRssi > 2047) rawRssi = 2047;
        uint8_t rssi = rawRssi >> 3;

        // Update lap timer with this RSSI reading
        uint32_t now = millis();
        timers[slot].handleLapTimerUpdate(now, rssi);

        // Update webserver with current RSSI
        ws.setRssi(slot, timers[slot].getRssi());
        ws.setCurrentSlot(slot);
    }

    ElegantOTA.loop();
}
