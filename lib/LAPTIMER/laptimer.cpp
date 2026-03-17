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
    filteredRssi = 0;
}

void LapTimer::start() {
    DEBUG("LapTimer[%u] started\n", pilot);
    raceStartTimeMs = millis();
    startTimeMs = raceStartTimeMs;  // ensure minLap guard works from race start
    state = RUNNING;
    lapCountWraparound = false;
    lapCount = 0;
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
    memset(lapTimes, 0, sizeof(lapTimes));
    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    lapAvailable = false;
    buz->beep(500);
    led->on(500);
}

void LapTimer::handleLapTimerUpdate(uint32_t currentTimeMs, uint8_t rssiValue) {
    // Apply Kalman filter to incoming RSSI
    filteredRssi = round(filter.filter(rssiValue, 0));

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
        case RUNNING: {
            // Holeshot (first crossing, lapCount==0 before any wraparound):
            // always capture — minLap guard does NOT apply.
            // Subsequent laps: enforce minLap.
            bool isHoleShot = (lapCount == 0 && !lapCountWraparound);
            if (isHoleShot || (currentTimeMs - startTimeMs) > conf->getMinLapMs(pilot)) {
                lapPeakCapture();
            }

            if (lapPeakCaptured()) {
                finishLap();
                startLap();
            }
            break;
        }
        default:
            break;
    }
}

void LapTimer::lapPeakCapture() {
    if (filteredRssi >= conf->getEnterRssi(pilot)) {
        if (filteredRssi > rssiPeak) {
            rssiPeak = filteredRssi;
            rssiPeakTimeMs = millis();
        }
    }
}

bool LapTimer::lapPeakCaptured() {
    return (filteredRssi < rssiPeak) && (filteredRssi < conf->getExitRssi(pilot));
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

void LapTimer::setRssiOnly(uint8_t rssiValue) {
    // Update filtered RSSI for display only — no lap detection.
    // Always reset rssiPeak: any call here means we are either in a settle window
    // after a frequency switch, or we were frozen during another pilot's priority mode.
    // In either case the peak value is stale/unreliable; resetting it unconditionally
    // prevents handleLapTimerUpdate() from triggering a false lap when detection resumes.
    filteredRssi = round(filter.filter(rssiValue, 0));
    rssiPeak = 0;
}

uint8_t LapTimer::getRssi() {
    return filteredRssi;
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
