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

    if (source["freq"].is<uint16_t>() && source["freq"] != p.frequency) {
        p.frequency = source["freq"];
        modified = true;
    }
    if (source["minLap"].is<uint16_t>() && source["minLap"] != p.minLap) {
        p.minLap = source["minLap"];
        modified = true;
    }
    if (source["enterRssi"].is<uint8_t>() && source["enterRssi"] != p.enterRssi) {
        p.enterRssi = source["enterRssi"];
        modified = true;
    }
    if (source["exitRssi"].is<uint8_t>() && source["exitRssi"] != p.exitRssi) {
        p.exitRssi = source["exitRssi"];
        modified = true;
    }
    if (source["name"].is<const char*>() && source["name"] != p.pilotName) {
        strlcpy(p.pilotName, source["name"] | "", sizeof(p.pilotName));
        modified = true;
    }
}

void Config::globalFromJson(JsonObject source) {
    if (source["alarm"].is<uint8_t>() && source["alarm"] != conf.alarm) {
        conf.alarm = source["alarm"];
        modified = true;
    }
    if (source["anType"].is<uint8_t>() && source["anType"] != conf.announcerType) {
        conf.announcerType = source["anType"];
        modified = true;
    }
    if (source["anRate"].is<uint8_t>() && source["anRate"] != conf.announcerRate) {
        conf.announcerRate = source["anRate"];
        modified = true;
    }
    if (source["ssid"].is<const char*>() && source["ssid"] != conf.ssid) {
        strlcpy(conf.ssid, source["ssid"] | "", sizeof(conf.ssid));
        modified = true;
    }
    if (source["pwd"].is<const char*>() && source["pwd"] != conf.password) {
        strlcpy(conf.password, source["pwd"] | "", sizeof(conf.password));
        modified = true;
    }
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

    // Default frequencies: R1, R2, R4, R7 (common race frequencies)
    const uint16_t defaultFreqs[NUM_PILOTS] = {5658, 5695, 5769, 5880};
    const char* defaultNames[NUM_PILOTS] = {"Pilot 1", "Pilot 2", "Pilot 3", "Pilot 4"};

    for (uint8_t i = 0; i < NUM_PILOTS; i++) {
        conf.pilots[i].frequency = defaultFreqs[i];
        conf.pilots[i].minLap = 100;  // 10.0s
        conf.pilots[i].enterRssi = 120;
        conf.pilots[i].exitRssi = 100;
        strlcpy(conf.pilots[i].pilotName, defaultNames[i], sizeof(conf.pilots[i].pilotName));
    }

    conf.alarm = 36;
    conf.announcerType = 2;
    conf.announcerRate = 10;
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
