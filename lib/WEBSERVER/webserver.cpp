#include "webserver.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>

#include "debug.h"

static const uint8_t DNS_PORT = 53;
static IPAddress netMsk(255, 255, 255, 0);
static DNSServer dnsServer;
static IPAddress ipAddress;
static AsyncWebServer server(80);
static AsyncEventSource events("/events");

static const char *wifi_hostname = "plt";
static const char *wifi_ap_ssid_prefix = "PhobosLT4ch";
static const char *wifi_ap_password = "phoboslt";
static const char *wifi_ap_address = "20.0.0.1";
String wifi_ap_ssid;

void Webserver::init(Config *config, LapTimer *lapTimers, BatteryMonitor *batMonitor, Buzzer *buzzer, Led *l) {
    ipAddress.fromString(wifi_ap_address);

    conf = config;
    timers = lapTimers;
    monitor = batMonitor;
    buz = buzzer;
    led = l;

    memset(currentRssi, 0, sizeof(currentRssi));

    wifi_ap_ssid = String(wifi_ap_ssid_prefix) + "_" + WiFi.macAddress().substring(WiFi.macAddress().length() - 6);
    wifi_ap_ssid.replace(":", "");

    WiFi.persistent(false);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    if (conf->getSsid()[0] == 0) {
        changeMode = WIFI_AP;
    } else {
        changeMode = WIFI_STA;
    }
    changeTimeMs = millis();
    lastStatus = WL_DISCONNECTED;
}

void Webserver::setRssi(uint8_t pilot, uint8_t rssi) {
    if (pilot < NUM_PILOTS) {
        currentRssi[pilot] = rssi;
    }
}

void Webserver::setCurrentSlot(uint8_t slot) {
    currentSlot = slot;
}

void Webserver::sendRssiEvent() {
    if (!servicesStarted) return;
    // Send all 4 pilot RSSI values + battery voltage in one event
    char buf[160];
    uint8_t batV = monitor->getBatteryVoltage();  // value * 10 = voltage * 10
    snprintf(buf, sizeof(buf), "{\"r\":[%u,%u,%u,%u],\"s\":%u,\"v\":%u}",
             currentRssi[0], currentRssi[1], currentRssi[2], currentRssi[3],
             currentSlot, batV);
    events.send(buf, "rssi");
}

void Webserver::sendLaptimeEvent(uint8_t pilot, uint32_t lapTime) {
    if (!servicesStarted) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"p\":%u,\"t\":%u}", pilot, lapTime);
    events.send(buf, "lap");
}

void Webserver::handleWebUpdate(uint32_t currentTimeMs) {
    // Countdown 3-2-1-GO! then start timers
    if (countdownActive) {
        uint32_t elapsed = currentTimeMs - countdownStartMs;
        if (countdownStep == 0) {
            buz->beep(120);                          // "3"
            countdownStep = 1;
        } else if (elapsed >= 1000 && countdownStep == 1) {
            buz->beep(120);                          // "2"
            countdownStep = 2;
        } else if (elapsed >= 2000 && countdownStep == 2) {
            buz->beep(120);                          // "1"
            countdownStep = 3;
        } else if (elapsed >= 3000 && countdownStep == 3) {
            buz->beep(600);                          // GO!
            for (uint8_t i = 0; i < NUM_PILOTS; i++) timers[i].start();
            countdownActive = false;
        }
    }

    // Check for lap events from all pilots
    for (uint8_t i = 0; i < NUM_PILOTS; i++) {
        if (timers[i].isLapAvailable()) {
            sendLaptimeEvent(i, timers[i].getLapTime());
        }
    }

    // Send RSSI periodically
    if (sendRssi_flag && ((currentTimeMs - rssiSentMs) > WEB_RSSI_SEND_TIMEOUT_MS)) {
        sendRssiEvent();
        rssiSentMs = currentTimeMs;
    }

    wl_status_t status = WiFi.status();

    if (status != lastStatus && wifiMode == WIFI_STA) {
        DEBUG("WiFi status = %u\n", status);
        switch (status) {
            case WL_NO_SSID_AVAIL:
            case WL_CONNECT_FAILED:
            case WL_CONNECTION_LOST:
                changeTimeMs = currentTimeMs;
                changeMode = WIFI_AP;
                break;
            case WL_DISCONNECTED:
                changeTimeMs = currentTimeMs;
                break;
            case WL_CONNECTED:
                buz->beep(200);
                led->off();
                wifiConnected = true;
                break;
            default:
                break;
        }
        lastStatus = status;
    }
    if (status != WL_CONNECTED && wifiMode == WIFI_STA && (currentTimeMs - changeTimeMs) > WIFI_CONNECTION_TIMEOUT_MS) {
        changeTimeMs = currentTimeMs;
        if (!wifiConnected) {
            changeMode = WIFI_AP;
        } else {
            DEBUG("WiFi Connection failed, reconnecting\n");
            WiFi.reconnect();
            startServices();
            buz->beep(100);
            led->blink(200);
        }
    }
    if (changeMode != wifiMode && changeMode != WIFI_OFF && (currentTimeMs - changeTimeMs) > WIFI_RECONNECT_TIMEOUT_MS) {
        switch (changeMode) {
            case WIFI_AP:
                DEBUG("Changing to WiFi AP mode\n");
                WiFi.disconnect();
                wifiMode = WIFI_AP;
                WiFi.setHostname(wifi_hostname);
                WiFi.mode(wifiMode);
                WiFi.setTxPower(WIFI_POWER_19_5dBm);
                changeTimeMs = currentTimeMs;
                WiFi.softAPConfig(ipAddress, ipAddress, netMsk);
                WiFi.softAP(wifi_ap_ssid.c_str(), wifi_ap_password);
                startServices();
                buz->beep(1000);
                led->on(1000);
                break;
            case WIFI_STA:
                DEBUG("Connecting to WiFi network\n");
                wifiMode = WIFI_STA;
                WiFi.setHostname(wifi_hostname);
                WiFi.mode(wifiMode);
                changeTimeMs = currentTimeMs;
                WiFi.begin(conf->getSsid(), conf->getPassword());
                startServices();
                led->blink(200);
            default:
                break;
        }
        changeMode = WIFI_OFF;
    }

    if (servicesStarted) {
        dnsServer.processNextRequest();
    }
}

static boolean isIp(String str) {
    for (size_t i = 0; i < str.length(); i++) {
        int c = str.charAt(i);
        if (c != '.' && (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

static String toStringIp(IPAddress ip) {
    String res = "";
    for (int i = 0; i < 3; i++) {
        res += String((ip >> (8 * i)) & 0xFF) + ".";
    }
    res += String(((ip >> 8 * 3)) & 0xFF);
    return res;
}

static bool captivePortal(AsyncWebServerRequest *request) {
    extern const char *wifi_hostname;
    if (!isIp(request->host()) && request->host() != (String(wifi_hostname) + ".local")) {
        DEBUG("Request redirected to captive portal\n");
        request->redirect(String("http://") + toStringIp(request->client()->localIP()));
        return true;
    }
    return false;
}

static void handleRoot(AsyncWebServerRequest *request) {
    if (captivePortal(request)) return;
    request->send(LittleFS, "/index.html", "text/html");
}

static void handleNotFound(AsyncWebServerRequest *request) {
    if (captivePortal(request)) return;
    String message = F("File Not Found\n\n");
    message += F("URI: ");
    message += request->url();
    message += F("\nMethod: ");
    message += (request->method() == HTTP_GET) ? "GET" : "POST";
    message += F("\nArguments: ");
    message += request->args();
    message += F("\n");
    for (uint8_t i = 0; i < request->args(); i++) {
        message += String(F(" ")) + request->argName(i) + F(": ") + request->arg(i) + F("\n");
    }
    AsyncWebServerResponse *response = request->beginResponse(404, "text/plain", message);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

static bool startLittleFS() {
    if (!LittleFS.begin()) {
        DEBUG("LittleFS mount failed\n");
        return false;
    }
    DEBUG("LittleFS mounted successfully\n");
    return true;
}

static void startMDNS() {
    if (!MDNS.begin(wifi_hostname)) {
        DEBUG("Error starting mDNS\n");
        return;
    }
    String instance = String(wifi_hostname) + "_" + WiFi.macAddress();
    instance.replace(":", "");
    MDNS.setInstanceName(instance);
    MDNS.addService("http", "tcp", 80);
}

void Webserver::startServices() {
    if (servicesStarted) {
        MDNS.end();
        startMDNS();
        return;
    }

    startLittleFS();

    server.on("/", handleRoot);
    server.on("/generate_204", handleRoot);
    server.on("/gen_204", handleRoot);
    server.on("/library/test/success.html", handleRoot);
    server.on("/hotspot-detect.html", handleRoot);
    server.on("/connectivity-check.html", handleRoot);
    server.on("/check_network_status.txt", handleRoot);
    server.on("/ncsi.txt", handleRoot);
    server.on("/fwlink", handleRoot);

    server.on("/status", [this](AsyncWebServerRequest *request) {
        char buf[1024];
        char configBuf[512];
        conf->toJsonString(configBuf, sizeof(configBuf));
        float voltage = (float)monitor->getBatteryVoltage() / 10;
        const char *format =
            "Heap:\n\tFree:\t%i\n\tMin:\t%i\n\tSize:\t%i\n\tAlloc:\t%i\n"
            "LittleFS:\n\tUsed:\t%i\n\tTotal:\t%i\n"
            "Chip:\n\tModel:\t%s Rev %i, %i Cores, SDK %s\n\tFlashSize:\t%i\n\tFlashSpeed:\t%iMHz\n\tCPU Speed:\t%iMHz\n"
            "Network:\n\tIP:\t%s\n\tMAC:\t%s\n"
            "Config:\n%s\n"
            "Battery Voltage:\t%0.1fv";

        snprintf(buf, sizeof(buf), format,
                 ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getHeapSize(), ESP.getMaxAllocHeap(),
                 LittleFS.usedBytes(), LittleFS.totalBytes(),
                 ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getSdkVersion(),
                 ESP.getFlashChipSize(), ESP.getFlashChipSpeed() / 1000000, getCpuFrequencyMhz(),
                 WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), configBuf, voltage);
        request->send(200, "text/plain", buf);
        led->on(200);
    });

    // Start/stop all pilots
    server.on("/timer/startAll", HTTP_POST, [this](AsyncWebServerRequest *request) {
        for (uint8_t i = 0; i < NUM_PILOTS; i++) {
            timers[i].start();
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    server.on("/timer/stopAll", HTTP_POST, [this](AsyncWebServerRequest *request) {
        for (uint8_t i = 0; i < NUM_PILOTS; i++) {
            timers[i].stop();
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // Start/stop individual pilot: /timer/start?p=0
    server.on("/timer/start", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("p")) {
            uint8_t p = request->getParam("p")->value().toInt();
            if (p < NUM_PILOTS) {
                timers[p].start();
                request->send(200, "application/json", "{\"status\": \"OK\"}");
            } else {
                request->send(400, "application/json", "{\"error\": \"invalid pilot\"}");
            }
        } else {
            // Start all
            for (uint8_t i = 0; i < NUM_PILOTS; i++) {
                timers[i].start();
            }
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        }
    });

    server.on("/timer/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("p")) {
            uint8_t p = request->getParam("p")->value().toInt();
            if (p < NUM_PILOTS) {
                timers[p].stop();
                request->send(200, "application/json", "{\"status\": \"OK\"}");
            } else {
                request->send(400, "application/json", "{\"error\": \"invalid pilot\"}");
            }
        } else {
            for (uint8_t i = 0; i < NUM_PILOTS; i++) {
                timers[i].stop();
            }
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        }
    });

    // 3-2-1 countdown then start all timers
    server.on("/timer/countdown", HTTP_POST, [this](AsyncWebServerRequest *request) {
        countdownActive = true;
        countdownStartMs = millis();
        countdownStep = 0;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.on("/timer/rssiStart", HTTP_POST, [this](AsyncWebServerRequest *request) {
        sendRssi_flag = true;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.on("/timer/rssiStop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        sendRssi_flag = false;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    // Get all config
    server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        conf->toJson(*response);
        request->send(response);
        led->on(200);
    });

    // Set per-pilot config: POST /config/pilot?p=0
    AsyncCallbackJsonWebHandler *pilotConfigHandler = new AsyncCallbackJsonWebHandler("/config/pilot", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        uint8_t p = 0;
        if (request->hasParam("p")) {
            p = request->getParam("p")->value().toInt();
        }
        JsonObject jsonObj = json.as<JsonObject>();
        conf->fromJson(jsonObj, p);
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    // Set global config: POST /config/global
    AsyncCallbackJsonWebHandler *globalConfigHandler = new AsyncCallbackJsonWebHandler("/config/global", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        conf->globalFromJson(jsonObj);
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.serveStatic("/", LittleFS, "/").setCacheControl("max-age=600");

    events.onConnect([this](AsyncEventSourceClient *client) {
        if (client->lastId()) {
            DEBUG("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
        }
        client->send("start", NULL, millis(), 1000);
        led->on(200);
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Max-Age", "600");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "POST,GET,OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

    server.onNotFound(handleNotFound);

    server.addHandler(&events);
    server.addHandler(pilotConfigHandler);
    server.addHandler(globalConfigHandler);

    server.begin();

    dnsServer.start(DNS_PORT, "*", ipAddress);
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);

    startMDNS();

    servicesStarted = true;
}
