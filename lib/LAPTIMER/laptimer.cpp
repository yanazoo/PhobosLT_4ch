#include "laptimer.h"

#include "debug.h"

// EMA (Exponential Moving Average) inter-round smoothing.
// Fills the ~20 ms TDM gap between readings of the same pilot:
//   filteredRssi = α × rssiValue + (1-α) × filteredRssi
//
// EMA_ALPHA (0–10, representing α×10):
//   10 → α=1.0  no smoothing (current reading only)
//    7 → α=0.7  light smoothing — spike(130) from baseline(70) → 112, below enterRssi=120 ✓
//    5 → α=0.5  moderate smoothing
//    3 → α=0.3  heavy smoothing (slower to react)
//
// Start with 7. Increase toward 10 if fast drones are missed; decrease toward 5 if noise persists.
#define EMA_ALPHA  4   // α = EMA_ALPHA / 10

// Exit confirmation: require this many consecutive samples below exitRssi before counting a lap.
// At ~16 ms/pilot, EXIT_CONFIRM_SAMPLES=2 → ~32 ms sustained drop required.
// Prevents false laps from single TDM noise samples or RF saturation transients.
#define EXIT_CONFIRM_SAMPLES  2

// Duration guard: maximum time (ms) the filtered RSSI may stay above enterRssi in a single
// peak-capture event before the peak is invalidated as ambient RF.
// A genuine gate pass (even at walking speed through a 1 m gate) is done within ~300 ms.
// A drone flying in the air near the loop keeps the RSSI elevated for seconds.
// At 500 ms the gate-pass detection window covers speeds ≥ 7 km/h while rejecting
// all hovering / low-pass ambient signals.
#define MAX_PEAK_DURATION_MS  500

// (PEAK_VALID_MARGIN removed — see lapPeakCaptured for the noise guard)

// RF saturation invalidation threshold margin.
// The invalidation fires only when rawRssi >= enterRssi + SATURATION_RAW_MARGIN.
// This keeps the threshold ABOVE the "fast drone exiting" raw RSSI range (80–100)
// while still catching RF saturation from another pilot's VTX (raw ≈ 120).
//   enterRssi=80 → threshold=105 :  saturation(120)≥105 → invalidate  ✓
//                                    fast exit (80–100)<105 → keep peak  ✓
#define SATURATION_RAW_MARGIN  25

// Peak validation uses "retroactive invalidation" instead of a delayed-capture approach:
//   1. rssiPeak is set on the VERY FIRST dominant sample (enables fast-drone detection).
//   2. If the NEXT sample is no longer dominant but filteredRssi is still >= enterRssi,
//      it means another pilot's elevated getRssi() is suppressing isDominant → RF saturation.
//      In that case rssiPeak is cleared (retroactive invalidation).
//   3. If filteredRssi drops below enterRssi before a second dominant reading arrives,
//      the drone passed too fast to accumulate two samples — rssiPeak is kept as genuine.
//   Two consecutive dominant readings set peakValidated=true and end further invalidation checks.

void LapTimer::init(Config *config, uint8_t pilotIndex, Buzzer *buzzer, Led *l) {
    conf = config;
    pilot = pilotIndex;
    buz = buzzer;
    led = l;

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
    peakEntryTimeMs = 0;
    exitConfirmCount = 0;
    peakDominantCount = 0;
    peakValidated = false;
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
    peakEntryTimeMs = 0;
    exitConfirmCount = 0;
    peakDominantCount = 0;
    peakValidated = false;
    lapAvailable = false;
    buz->beep(500);
    led->on(500);
}

void LapTimer::handleLapTimerUpdate(uint32_t currentTimeMs, uint8_t rssiValue, bool isDominant) {
    // EMA: blend new reading with previous value to smooth across the 20 ms TDM gap.
    filteredRssi = ((uint16_t)rssiValue * EMA_ALPHA + (uint16_t)filteredRssi * (10 - EMA_ALPHA)) / 10;

    switch (state) {
        case STOPPED:
            break;
        case WAITING:
            lapPeakCapture(isDominant, rssiValue);
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
                lapPeakCapture(isDominant, rssiValue);
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

void LapTimer::lapPeakCapture(bool isDominant, uint8_t rawRssi) {
    uint8_t enterRssi = conf->getEnterRssi(pilot);

    if (isDominant && filteredRssi >= enterRssi) {
        // filteredRssi is the 3-read average from main.cpp — no Kalman lag.
        // Record the moment filteredRssi first crossed enterRssi (for duration guard).
        if (peakEntryTimeMs == 0) peakEntryTimeMs = millis();

        // Duration guard: if signal has been above enterRssi longer than MAX_PEAK_DURATION_MS
        // this is ambient RF (hovering drone nearby) rather than a gate pass.
        // A 7 km/h walk-speed gate pass through 1 m takes ~500 ms — anything slower is ambient.
        // Reset the peak and restart the entry timer so the loop continues to be rejected
        // as long as the ambient signal persists.
        if ((millis() - peakEntryTimeMs) > MAX_PEAK_DURATION_MS) {
            rssiPeak       = 0;
            rssiPeakTimeMs = 0;
            peakDominantCount = 0;
            peakValidated  = false;
            peakEntryTimeMs = millis();  // restart timer — still elevated, keep rejecting
            return;
        }

        // Dominant and above threshold: set/update rssiPeak immediately.
        // Setting on the first sample allows detection of fast drones (≥150 km/h)
        // that may only produce 1–2 samples above threshold per TDM cycle.
        if (filteredRssi > rssiPeak) {
            rssiPeak       = filteredRssi;
            rssiPeakTimeMs = millis();
        }
        // Accumulate consecutive dominant readings.
        // After two in a row the peak is validated; no further invalidation is attempted.
        peakDominantCount++;
        if (peakDominantCount >= 2) peakValidated = true;

    } else if (!isDominant && rssiPeak > 0 && !peakValidated
               && rawRssi >= (uint16_t)enterRssi + SATURATION_RAW_MARGIN
               && filteredRssi >= enterRssi) {
        // Lost dominance but raw RSSI is still WELL above threshold → RF saturation.
        //
        // SATURATION_RAW_MARGIN raises the invalidation bar above the "fast drone exiting"
        // range so that a 150 km/h drone whose rawRssi has fallen to 80–100 (still above
        // enterRssi) is NOT incorrectly invalidated:
        //
        //   Situation                  rawRssi   threshold(enterRssi=80+25=105)  Result
        //   RF saturation (≈120 raw)     120           ≥ 105                    → invalidate ✓
        //   Fast drone exiting (80–100)  80–100        < 105                    → keep peak  ✓
        //   Very fast drone gone  (≈17)   17           < 105                    → keep peak  ✓
        rssiPeak       = 0;
        rssiPeakTimeMs = 0;
        peakDominantCount = 0;
        peakEntryTimeMs = 0;

    } else {
        // Below threshold or already validated: reset counter and entry timer for next approach.
        if (filteredRssi < enterRssi) {
            peakDominantCount = 0;
            peakEntryTimeMs   = 0;
        }
    }
}

bool LapTimer::lapPeakCaptured() {
    // Noise guard: rssiPeak must strictly exceed exitRssi.
    // exitRssi is calibrated above the noise floor, so any peak that never cleared it is noise.
    //   Self-noise:  rssiPeak ≈ 70,  exitRssi = 80  → 70 ≤ 80  → ignored        ✓
    //   Drone pass:  rssiPeak = 110, exitRssi = 80  → 110 > 80 → allowed         ✓
    // This avoids the fragility of an enterRssi-relative margin that can exceed the
    // drone's filtered peak when enterRssi is calibrated close to the signal level.
    if (rssiPeak <= conf->getExitRssi(pilot)) { exitConfirmCount = 0; return false; }

    // Require EXIT_CONFIRM_SAMPLES consecutive readings below both rssiPeak and exitRssi.
    // At ~16 ms/pilot, EXIT_CONFIRM_SAMPLES=2 means ~32 ms sustained drop is required.
    // This prevents single TDM noise samples or brief RF saturation from triggering false laps.
    if (filteredRssi < rssiPeak && filteredRssi < conf->getExitRssi(pilot)) {
        exitConfirmCount++;
        return exitConfirmCount >= EXIT_CONFIRM_SAMPLES;
    }
    exitConfirmCount = 0;
    return false;
}

void LapTimer::startLap() {
    DEBUG("LapTimer[%u] Lap started\n", pilot);
    startTimeMs = rssiPeakTimeMs;
    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    peakEntryTimeMs = 0;
    exitConfirmCount = 0;
    peakDominantCount = 0;
    peakValidated = false;
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
    // Update filtered RSSI for display only — no lap detection, no rssiPeak reset.
    filteredRssi = round(filter.filter(rssiValue, 0));
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
