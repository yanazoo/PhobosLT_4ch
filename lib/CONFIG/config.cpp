#include "config.h"

#include <EEPROM.h>

#include "debug.h"

void Config::init(void) {
    if (sizeof(laptimer_config_t) > EEPROM_RESERVED_SIZE) {
        DEBUG("Config size too big (%u > %u), adjust reserved EEPROM size\n",
              sizeof(laptimer_config_t), EEPROM_RESERVED_SIZE);
        return;
    }

    EEPROM.begin(EEPROM_RESERVED_SIZE);
    load();

    checkTimeMs = millis();
    DEBUG("EEPROM Init Successful, config size = %u bytes\n", sizeof(laptimer_config_t));
}

void Config::load(void) {
    modified = false;
    EEPROM.get(0, conf);

    uint32_t version = 0xFFFFFFFF;
    if ((conf.version & CONFIG_MAGIC_MASK) == CONFIG_MAGIC) {
        version = conf.version & ~CONFIG_MAGIC_MASK;
    }

    if (version != CONFIG_VERSION) {
        setDefaults();
    }
}

void Config::write(void) {
    if (!modified) return;

    DEBUG("Writing to EEPROM\n");
    EEPROM.put(0, conf);
    EEPROM.commit();
    DEBUG("Writing to EEPROM done\n");

    modified = false;
}

void Config::toJson(AsyncResponseStream& destination) {
    JsonDocument doc;

    JsonArray pilots = doc["pilots"].to<JsonArray>();
    for (uint8_t i = 0; i < NUM_PILOTS; i++) {
        JsonObject p = pilots.add<JsonObject>();
        p["freq"] = conf.pilots[i].frequency;
        p["minLap"] = conf.pilots[i].minLap;
        p["enterRssi"] = conf.pilots[i].enterRssi;
        p["exitRssi"] = conf.pilots[i].exitRssi;
        p["name"] = conf.pilots[i].pilotName;
    }

    doc["alarm"] = conf.alarm;
    doc["anType"] = conf.announcerType;
    doc["anRate"] = conf.announcerRate;
    doc["ssid"] = conf.ssid;
    doc["pwd"] = conf.password;

    serializeJson(doc, destination);
}

void Config::toJsonString(char* buf, size_t bufSize) {
    JsonDocument doc;

    JsonArray pilots = doc["pilots"].to<JsonArray>();
    for (uint8_t i = 0; i < NUM_PILOTS; i++) {
        JsonObject p = pilots.add<JsonObject>();
        p["freq"] = conf.pilots[i].frequency;
        p["minLap"] = conf.pilots[i].minLap;
        p["enterRssi"] = conf.pilots[i].enterRssi;
        p["exitRssi"] = conf.pilots[i].exitRssi;
        p["name"] = conf.pilots[i].pilotName;
    }

    doc["alarm"] = conf.alarm;
    doc["anType"] = conf.announcerType;
    doc["anRate"] = conf.announcerRate;

    serializeJsonPretty(doc, buf, bufSize);
}

void Config::pilotToJson(uint8_t pilot, AsyncResponseStream& destination) {
    if (pilot >= NUM_PILOTS) return;
    JsonDocument doc;
    doc["freq"] = conf.pilots[pilot].frequency;
    doc["minLap"] = conf.pilots[pilot].minLap;
    doc["enterRssi"] = conf.pilots[pilot].enterRssi;
    doc["exitRssi"] = conf.pilots[pilot].exitRssi;
    doc["name"] = conf.pilots[pilot].pilotName;
    serializeJson(doc, destination);
}

void Config::fromJson(JsonObject source, uint8_t pilot) {
    if (pilot >= NUM_PILOTS) return;

    pilot_config_t &p = conf.pilots[pilot];
    bool changed = false;

    if (!source["freq"].isNull()) {
        p.frequency = source["freq"].as<uint16_t>();
        changed = true;
    }
    if (!source["minLap"].isNull()) {
        p.minLap = source["minLap"].as<uint8_t>();
        changed = true;
    }
    if (!source["enterRssi"].isNull()) {
        p.enterRssi = source["enterRssi"].as<uint8_t>();
        changed = true;
    }
    if (!source["exitRssi"].isNull()) {
        p.exitRssi = source["exitRssi"].as<uint8_t>();
        changed = true;
    }
    if (!source["name"].isNull()) {
        strlcpy(p.pilotName, source["name"] | "", sizeof(p.pilotName));
        changed = true;
    }
    if (changed) modified = true;
}

void Config::globalFromJson(JsonObject source) {
    bool changed = false;
    if (!source["alarm"].isNull())  { conf.alarm = source["alarm"].as<uint8_t>();          changed = true; }
    if (!source["anType"].isNull()) { conf.announcerType = source["anType"].as<uint8_t>(); changed = true; }
    if (!source["anRate"].isNull()) { conf.announcerRate = source["anRate"].as<uint8_t>(); changed = true; }
    if (!source["ssid"].isNull())   { strlcpy(conf.ssid, source["ssid"] | "", sizeof(conf.ssid));         changed = true; }
    if (!source["pwd"].isNull())    { strlcpy(conf.password, source["pwd"] | "", sizeof(conf.password));  changed = true; }
    if (changed) modified = true;
}

uint16_t Config::getFrequency(uint8_t pilot) {
    if (pilot >= NUM_PILOTS) return 0;
    return conf.pilots[pilot].frequency;
}

uint32_t Config::getMinLapMs(uint8_t pilot) {
    if (pilot >= NUM_PILOTS) return 0;
    return conf.pilots[pilot].minLap * 100;
}

uint8_t Config::getEnterRssi(uint8_t pilot) {
    if (pilot >= NUM_PILOTS) return 0;
    return conf.pilots[pilot].enterRssi;
}

uint8_t Config::getExitRssi(uint8_t pilot) {
    if (pilot >= NUM_PILOTS) return 0;
    return conf.pilots[pilot].exitRssi;
}

const char* Config::getPilotName(uint8_t pilot) {
    if (pilot >= NUM_PILOTS) return "";
    return conf.pilots[pilot].pilotName;
}

uint8_t Config::getAlarmThreshold() {
    return conf.alarm;
}

char* Config::getSsid() {
    return conf.ssid;
}

char* Config::getPassword() {
    return conf.password;
}

void Config::setDefaults(void) {
    DEBUG("Setting EEPROM defaults for 4-pilot config\n");
    memset(&conf, 0, sizeof(conf));
    conf.version = CONFIG_VERSION | CONFIG_MAGIC;

    // Default frequencies: R5, R4, R3, R2
    const uint16_t defaultFreqs[NUM_PILOTS] = {5806, 5769, 5732, 5695};
    const char* defaultNames[NUM_PILOTS] = {"Pilot 1", "Pilot 2", "Pilot 3", "Pilot 4"};

    for (uint8_t i = 0; i < NUM_PILOTS; i++) {
        conf.pilots[i].frequency = defaultFreqs[i];
        conf.pilots[i].minLap = 60;   // 6.0s
        conf.pilots[i].enterRssi = 115;
        conf.pilots[i].exitRssi = 112;
        strlcpy(conf.pilots[i].pilotName, defaultNames[i], sizeof(conf.pilots[i].pilotName));
    }

    conf.alarm = 32;          // 3.2V
    conf.announcerType = 0;   // laptime
    conf.announcerRate = 11;  // 1.1x
    strlcpy(conf.ssid, "", sizeof(conf.ssid));
    strlcpy(conf.password, "", sizeof(conf.password));

    modified = true;
    write();
}

void Config::handleEeprom(uint32_t currentTimeMs) {
    if (modified && ((currentTimeMs - checkTimeMs) > EEPROM_CHECK_TIME_MS)) {
        checkTimeMs = currentTimeMs;
        write();
    }
}
