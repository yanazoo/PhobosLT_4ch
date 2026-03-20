#pragma once

#include "buzzer.h"
#include "config.h"
#include "kalman.h"
#include "led.h"

typedef enum {
    STOPPED,
    WAITING,
    RUNNING
} laptimer_state_e;

#define LAPTIMER_LAP_HISTORY 10
#define LAPTIMER_RSSI_HISTORY 100

class LapTimer {
   public:
    void init(Config *config, uint8_t pilotIndex, Buzzer *buzzer, Led *l);
    void start();
    void stop();
    void handleLapTimerUpdate(uint32_t currentTimeMs, uint8_t rssiValue, bool isDominant);
    void setRssiOnly(uint8_t rssiValue);  // update display RSSI without lap detection
    uint8_t getRssi();
    uint32_t getLapTime();
    bool isLapAvailable();
    uint8_t getLapCount();
    uint32_t getLapTimeAt(uint8_t index);
    laptimer_state_e getState() { return state; }

   private:
    laptimer_state_e state = STOPPED;
    Config *conf;
    uint8_t pilot;  // pilot index (0-3)
    Buzzer *buz;
    Led *led;
    KalmanFilter filter;
    boolean lapCountWraparound;
    uint32_t raceStartTimeMs;
    uint32_t startTimeMs;
    uint8_t lapCount;
    uint32_t lapTimes[LAPTIMER_LAP_HISTORY];
    uint8_t filteredRssi = 0;

    uint8_t rssiPeak;
    uint32_t rssiPeakTimeMs;
    uint32_t peakEntryTimeMs = 0;   // millis() when filteredRssi first crossed enterRssi (for duration guard)
    uint8_t exitConfirmCount = 0;   // consecutive samples below exitRssi required for lap detection
    uint8_t peakDominantCount = 0;  // consecutive dominant samples seen above enterRssi
    bool    peakValidated = false;  // true once ≥2 consecutive dominant readings confirm the peak is genuine

    bool lapAvailable = false;

    void lapPeakCapture(bool isDominant, uint8_t rawRssi);
    bool lapPeakCaptured();

    void startLap();
    void finishLap();
};
