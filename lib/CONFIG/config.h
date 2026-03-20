#pragma once

#include <stdint.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>

/*
## Pinout ##

### ESP32
| ESP32 GPIO | XIAO label | RX5808 |
|:-----------|:-----------|:-------|
| 33         | —          | RSSI   |
| 19         | —          | CH1 (DATA) |
| 22         | —          | CH2 (SEL)  |
| 23         | —          | CH3 (CLK)  |
| GND        | GND        | GND    |
| 3V3        | 3V3        | +5V    |
* LED → GPIO21 / Buzzer → GPIO27

### XIAO ESP32-S3
| GPIO | XIAO label | 接続先 |
|:-----|:-----------|:-------|
| 3    | D2         | RX5808 RSSI |
| 5    | D4         | RX5808 CH1 (DATA) |
| 6    | D5         | RX5808 CH2 (SEL)  |
| 4    | D3         | RX5808 CH3 (CLK)  |
| 9    | D10        | Buzzer |
| 2    | D1         | LED    |
| 1    | D0         | VBAT   |
| GND  | GND        | GND    |
| 3V3  | 3V3        | +5V    |
*/

// ESP32-S3 (XIAO ESP32-S3)
#if defined(ESP32S3)

#define PIN_LED 2
#define PIN_VBAT 1
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 3
#define PIN_RX5808_DATA 5     //CH1
#define PIN_RX5808_SELECT 6   //CH2
#define PIN_RX5808_CLOCK 4    //CH3
#define PIN_BUZZER 9
#define BUZZER_INVERTED false

// ESP32
#else

#define PIN_LED 21
#define PIN_VBAT 35
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 33
#define PIN_RX5808_DATA 19    // CH1
#define PIN_RX5808_SELECT 22  // CH2
#define PIN_RX5808_CLOCK 23   // CH3
#define PIN_BUZZER 27
#define BUZZER_INVERTED false

#endif

#define NUM_PILOTS 4

#define EEPROM_RESERVED_SIZE 512
#define CONFIG_MAGIC_MASK (0b11U << 30)
#define CONFIG_MAGIC (0b01U << 30)
#define CONFIG_VERSION 1U

#define EEPROM_CHECK_TIME_MS 1000

typedef struct {
    uint16_t frequency;
    uint8_t enterRssi;
    uint8_t exitRssi;
    uint8_t minLap;         // in 0.1s units
    char pilotName[21];
} pilot_config_t;

typedef struct {
    uint32_t version;
    pilot_config_t pilots[NUM_PILOTS];
    uint8_t alarm;
    uint8_t announcerType;
    uint8_t announcerRate;
    char ssid[33];
    char password[33];
} laptimer_config_t;

class Config {
   public:
    void init();
    void load();
    void write();
    void toJson(AsyncResponseStream& destination);
    void toJsonString(char* buf, size_t bufSize);
    void pilotToJson(uint8_t pilot, AsyncResponseStream& destination);
    void fromJson(JsonObject source, uint8_t pilot);
    void globalFromJson(JsonObject source);
    void handleEeprom(uint32_t currentTimeMs);

    // Per-pilot getters
    uint16_t getFrequency(uint8_t pilot);
    uint32_t getMinLapMs(uint8_t pilot);
    uint8_t getEnterRssi(uint8_t pilot);
    uint8_t getExitRssi(uint8_t pilot);
    const char* getPilotName(uint8_t pilot);

    // Global getters
    uint8_t getAlarmThreshold();
    char* getSsid();
    char* getPassword();

   private:
    laptimer_config_t conf;
    bool modified;
    volatile uint32_t checkTimeMs = 0;
    void setDefaults();
};
