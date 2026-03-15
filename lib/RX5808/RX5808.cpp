#include "RX5808.h"

#include <Arduino.h>

#include "debug.h"

RX5808::RX5808(uint8_t _rssiInputPin, uint8_t _rx5808DataPin, uint8_t _rx5808SelPin, uint8_t _rx5808ClkPin) {
    rssiInputPin = _rssiInputPin;
    rx5808DataPin = _rx5808DataPin;
    rx5808SelPin = _rx5808SelPin;
    rx5808ClkPin = _rx5808ClkPin;
    lastSetFreqTimeMs = millis();
}

void RX5808::init() {
    pinMode(rssiInputPin, INPUT);
    pinMode(rx5808DataPin, OUTPUT);
    pinMode(rx5808SelPin, OUTPUT);
    pinMode(rx5808ClkPin, OUTPUT);
    digitalWrite(rx5808SelPin, HIGH);
    digitalWrite(rx5808ClkPin, LOW);
    digitalWrite(rx5808DataPin, LOW);
    resetRxModule();
    // Don't power down - scanner will set freq immediately
}

void RX5808::handleFrequencyChange(uint32_t currentTimeMs, uint16_t potentiallyNewFreq) {
    if ((currentFrequency != potentiallyNewFreq) && ((currentTimeMs - lastSetFreqTimeMs) > RX5808_MIN_BUSTIME)) {
        lastSetFreqTimeMs = currentTimeMs;
        setFrequency(potentiallyNewFreq);
    }

    if (recentSetFreqFlag && (currentTimeMs - lastSetFreqTimeMs) > RX5808_MIN_TUNETIME) {
        lastSetFreqTimeMs = currentTimeMs;
        DEBUG("RX5808 Tune done\n");
        verifyFrequency();
        recentSetFreqFlag = false;
    }
}

bool RX5808::verifyFrequency() {
    uint16_t vtxRegisterHex = 0;

    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit1();  // Register 0x1
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();

    rx5808SerialSendBit0();  // Read register r/w

    pinMode(rx5808DataPin, INPUT_PULLUP);
    for (uint8_t i = 0; i < 20; i++) {
        delayMicroseconds(10);
        if (i < 16) {
            if (digitalRead(rx5808DataPin)) {
                bitWrite(vtxRegisterHex, i, 1);
            } else {
                bitWrite(vtxRegisterHex, i, 0);
            }
        }
        if (i >= 16) {
            digitalRead(rx5808DataPin);
        }
        digitalWrite(rx5808ClkPin, HIGH);
        delayMicroseconds(10);
        digitalWrite(rx5808ClkPin, LOW);
        delayMicroseconds(10);
    }

    pinMode(rx5808DataPin, OUTPUT);
    rx5808SerialEnableHigh();

    digitalWrite(rx5808ClkPin, LOW);
    digitalWrite(rx5808DataPin, LOW);

    if (vtxRegisterHex != freqMhzToRegVal(currentFrequency)) {
        DEBUG("RX5808 frequency not matching, register = %u, currentFreq = %u\n", vtxRegisterHex, currentFrequency);
        return false;
    }
    DEBUG("RX5808 frequency verified properly\n");
    return true;
}

void RX5808::setFrequency(uint16_t vtxFreq) {
    DEBUG("Setting frequency to %u\n", vtxFreq);

    currentFrequency = vtxFreq;

    if (vtxFreq == POWER_DOWN_FREQ_MHZ) {
        powerDownRxModule();
        rxPoweredDown = true;
        return;
    }
    if (rxPoweredDown) {
        resetRxModule();
        rxPoweredDown = false;
    }

    uint16_t vtxHex = freqMhzToRegVal(vtxFreq);

    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit1();  // Register 0x1
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();

    rx5808SerialSendBit1();  // Write to register

    uint8_t i;
    for (i = 16; i > 0; i--) {
        if (vtxHex & 0x1) {
            rx5808SerialSendBit1();
        } else {
            rx5808SerialSendBit0();
        }
        vtxHex >>= 1;
    }

    for (i = 4; i > 0; i--)
        rx5808SerialSendBit0();

    rx5808SerialEnableHigh();

    digitalWrite(rx5808ClkPin, LOW);
    digitalWrite(rx5808DataPin, LOW);

    recentSetFreqFlag = true;
}

uint8_t RX5808::readRssi() {
    volatile uint16_t rssi = 0;

    if (recentSetFreqFlag) return rssi;  // RSSI is unstable

    rssi = analogRead(rssiInputPin);
    if (rssi > 2047) rssi = 2047;
    return rssi >> 3;
}

void RX5808::rx5808SerialSendBit1() {
    digitalWrite(rx5808DataPin, HIGH);
    delayMicroseconds(1);
    digitalWrite(rx5808ClkPin, HIGH);
    delayMicroseconds(1);
    digitalWrite(rx5808ClkPin, LOW);
    delayMicroseconds(1);
}

void RX5808::rx5808SerialSendBit0() {
    digitalWrite(rx5808DataPin, LOW);
    delayMicroseconds(1);
    digitalWrite(rx5808ClkPin, HIGH);
    delayMicroseconds(1);
    digitalWrite(rx5808ClkPin, LOW);
    delayMicroseconds(1);
}

void RX5808::rx5808SerialEnableLow() {
    digitalWrite(rx5808SelPin, LOW);
    delayMicroseconds(10);
}

void RX5808::rx5808SerialEnableHigh() {
    digitalWrite(rx5808SelPin, HIGH);
    delayMicroseconds(10);
}

void RX5808::resetRxModule() {
    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit1();  // Register 0xF
    rx5808SerialSendBit1();
    rx5808SerialSendBit1();
    rx5808SerialSendBit1();

    rx5808SerialSendBit1();  // Write to register

    for (uint8_t i = 20; i > 0; i--)
        rx5808SerialSendBit0();

    rx5808SerialEnableHigh();

    setupRxModule();
}

void RX5808::setRxModulePower(uint32_t options) {
    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit0();  // Register 0xA
    rx5808SerialSendBit1();
    rx5808SerialSendBit0();
    rx5808SerialSendBit1();

    rx5808SerialSendBit1();  // Write to register

    for (uint8_t i = 20; i > 0; i--) {
        if (options & 0x1) {
            rx5808SerialSendBit1();
        } else {
            rx5808SerialSendBit0();
        }
        options >>= 1;
    }

    rx5808SerialEnableHigh();

    digitalWrite(rx5808DataPin, LOW);
}

void RX5808::powerDownRxModule() {
    setRxModulePower(0b11111111111111111111);
}

void RX5808::setupRxModule() {
    setRxModulePower(0b11010000110111110011);
}

uint16_t RX5808::freqMhzToRegVal(uint16_t freqInMhz) {
    uint16_t tf, N, A;
    tf = (freqInMhz - 479) / 2;
    N = tf / 32;
    A = tf % 32;
    return (N << (uint16_t)7) + A;
}
