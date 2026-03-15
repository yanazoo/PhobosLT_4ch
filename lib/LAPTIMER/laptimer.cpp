#include "laptimer.h"

#include "debug.h"

const uint16_t rssi_filter_q = 2000;  //  0.01 - 655.36
const uint16_t rssi_filter_r = 40;    // 0.0001 - 65.536

void LapTimer::init(Config *config, uint8_t pilotIndex, Buzzer *buzzer, Led *l) {
    conf = config;
    pilot = pilotIndex;
    buz = buzzer;
    led = l;

    filter.setMeasurementNoise(rssi_filter_q * 0.01f);
    filter.setProcessNoise(rssi_filter_r * 0.0001f);

    stop();
    memset(rssi, 0, sizeof(rssi));
}

void LapTimer::start() {
    DEBUG("LapTimer[%u] started\n", pilot);
    raceStartTimeMs = millis();
    state = RUNNING;
    lapCountWraparound = false;
    lapCount = 0;
    rssiCount = 0;
    memset(lapTimes, 0, sizeof(lapTimes));
    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    lapAvailable = false;
    buz->beep(500);
    led->on(500);
}

void LapTimer::stop() {
    DEBUG("LapTimer[%u] stopped\n", pilot);
    state = STOPPED;
    lapCountWraparound = false;
    lapCount = 0;
    rssiCount = 0;
    memset(lapTimes, 0, sizeof(lapTimes));
    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    lapAvailable = false;
    buz->beep(500);
    led->on(500);
}

void LapTimer::handleLapTimerUpdate(uint32_t currentTimeMs, uint8_t rssiValue) {
    // Apply Kalman filter to incoming RSSI
    rssi[rssiCount] = round(filter.filter(rssiValue, 0));

    switch (state) {
        case STOPPED:
            break;
        case WAITING:
            lapPeakCapture();
            if (lapPeakCaptured()) {
                state = RUNNING;
                startLap();
            }
            break;
        case RUNNING:
            if ((currentTimeMs - startTimeMs) > conf->getMinLapMs(pilot)) {
                lapPeakCapture();
            }

            if (lapPeakCaptured()) {
                finishLap();
                startLap();
            }
            break;
        default:
            break;
    }

    rssiCount = (rssiCount + 1) % LAPTIMER_RSSI_HISTORY;
}

void LapTimer::lapPeakCapture() {
    if (rssi[rssiCount] >= conf->getEnterRssi(pilot)) {
        if (rssi[rssiCount] > rssiPeak) {
            rssiPeak = rssi[rssiCount];
            rssiPeakTimeMs = millis();
        }
    }
}

bool LapTimer::lapPeakCaptured() {
    return (rssi[rssiCount] < rssiPeak) && (rssi[rssiCount] < conf->getExitRssi(pilot));
}

void LapTimer::startLap() {
    DEBUG("LapTimer[%u] Lap started\n", pilot);
    startTimeMs = rssiPeakTimeMs;
    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    buz->beep(200);
    led->on(200);
}

void LapTimer::finishLap() {
    if (lapCount == 0 && lapCountWraparound == false) {
        lapTimes[0] = rssiPeakTimeMs - raceStartTimeMs;
    } else {
        lapTimes[lapCount] = rssiPeakTimeMs - startTimeMs;
    }
    DEBUG("LapTimer[%u] Lap finished, lap time = %u\n", pilot, lapTimes[lapCount]);
    if ((lapCount + 1) % LAPTIMER_LAP_HISTORY == 0) {
        lapCountWraparound = true;
    }
    lapCount = (lapCount + 1) % LAPTIMER_LAP_HISTORY;
    lapAvailable = true;
}

uint8_t LapTimer::getRssi() {
    return rssi[rssiCount];
}

uint32_t LapTimer::getLapTime() {
    uint32_t lapTime = 0;
    lapAvailable = false;
    if (lapCount == 0) {
        lapTime = lapTimes[LAPTIMER_LAP_HISTORY - 1];
    } else {
        lapTime = lapTimes[lapCount - 1];
    }
    return lapTime;
}

bool LapTimer::isLapAvailable() {
    return lapAvailable;
}

uint8_t LapTimer::getLapCount() {
    return lapCount;
}

uint32_t LapTimer::getLapTimeAt(uint8_t index) {
    if (index >= LAPTIMER_LAP_HISTORY) return 0;
    return lapTimes[index];
}
