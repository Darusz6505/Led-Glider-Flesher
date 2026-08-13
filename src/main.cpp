#include <Arduino.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <Update.h> 

#define FLARM_RX_PIN 20 
#define FLARM_TX_PIN 21 
#define MOSFET_PIN 1 
#define BUILTIN_LED_PIN 8 

Preferences preferences;

// Zmienne konfiguracyjne (zapisywane w pamięci Flash)
String wifiSsid; 
String wifiPassword; 
int wifiTimeoutMinutes; 
int tempUpperLimit; 
int tempLowerLimit; 
int uartBaudRate; 
String btTrackerID;              
bool enableBT;                          
int blinksPerPacket;                       
unsigned long preventiveFlashInterval; 
bool disablePreventiveFlash;           

// Zmienne stanu systemu
bool wifiActive = true; 
bool thermalShutdown = false;

WebServer server(80);

int currentAlarmLevel = 0;
unsigned long lastMessageTime = 0;

// Zmienne obsługi Bluetooth (NimBLE)
bool btConnected = false;
bool doConnect = false;
bool btInitialized = false; 

// BEZPIECZNY WSKAŹNIK - Przechowuje GŁĘBOKĄ KOPIĘ urządzenia
NimBLEAdvertisedDevice* myDevice = nullptr; 
NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pRemoteCharacteristic = nullptr;

String discoveredDevices[30];
int discoveredCount = 0;

static NimBLEUUID serviceUUID_NUS("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static NimBLEUUID charUUID_NUS_TX("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
static NimBLEUUID serviceUUID_HM10("0000FFE0-0000-1000-8000-00805F9B34FB");
static NimBLEUUID charUUID_HM10("0000FFE1-0000-1000-8000-00805F9B34FB");

// Maszyna stanów świateł
enum State { IDLE, BLINK_ON, BLINK_OFF, INTER_PACKET_DELAY };
State flashState = IDLE;

int targetPackets = 0;          
int packetsCompleted = 0;       
int blinksInCurrentPacket = 0;  
bool sequenceActive = false;

unsigned long stateTimer = 0;
unsigned long lastSequenceStart = 0;
unsigned long lastPreventiveFlash = 0;

const int flashOnDuration = 50;       
const int flashOffDuration = 50;      
const int interPacketDelay = 200;     

// ROZDZIELONE BUFORY NMEA
String uartBuffer = "";
String bleBuffer = "";

String nmeaLogs[8];
int nmeaLogIndex = 0;

String generateRandomSuffix() {
  const char chars[] = "0123456789ABCDEF";
  String suffix = "";
  for (int i = 0; i < 3; i++) suffix += chars[esp_random() % 16];
  return suffix;
}

void addDiscoveredDevice(String nameOrMac) {
  if (nameOrMac.length() == 0) return;
  for (int i = 0; i < discoveredCount; i++) if (discoveredDevices[i].equalsIgnoreCase(nameOrMac)) return; 
  if (discoveredCount < 30) discoveredDevices[discoveredCount++] = nameOrMac;
}

void setLights(bool turnOn) {
  if (turnOn) { digitalWrite(MOSFET_PIN, HIGH); digitalWrite(BUILTIN_LED_PIN, LOW); } 
  else { digitalWrite(MOSFET_PIN, LOW); digitalWrite(BUILTIN_LED_PIN, HIGH); }
}

void bootBlinks(int blinks) {
  for (int i = 0; i < blinks; i++) {
    setLights(true); delay(50);
    setLights(false); delay(150);
  }
}

void startSequence(int packets) {
  targetPackets = packets;
  packetsCompleted = 0; 
  blinksInCurrentPacket = 0;
  sequenceActive = true;
  setLights(true); 
  flashState = BLINK_ON; 
  stateTimer = millis();
}

void scanCompleteCB(NimBLEScanResults results) {
  Serial.printf("[BLE] Skanowanie w tle zakończone. Wykryto %d unikalnych wpisów (RAM).\n", results.getCount());
}

// HYBRYDOWY SKANER NimBLE 2.0+
class MyAdvertisedDeviceCallbacks: public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
      
      String devMac = String(advertisedDevice->getAddress().toString().c_str());
      String devName = "";
      if (advertisedDevice->haveName()) {
        devName = String(advertisedDevice->getName().c_str());
        devName.trim();
      }

      Serial.print(F("[BLE Skaner] Złapano: "));
      Serial.print(devMac);
      if (devName.length() > 0) {
        Serial.print(F(" | Nazwa: "));
        Serial.print(devName);
      } else {
        Serial.print(F(" | <Brak nazwy>"));
      }
      Serial.println();

      // Dodajemy do listy rozwijanej dla WWW oba warianty identyfikacji
      addDiscoveredDevice(devMac);
      if (devName.length() > 0) addDiscoveredDevice(devName);
        
      if (enableBT && !btConnected && !doConnect && !thermalShutdown) {
        String target = btTrackerID;
        target.trim();

        if (devMac.equalsIgnoreCase(target) || (devName.length() > 0 && devName.equalsIgnoreCase(target))) {
          if (myDevice == nullptr) {
            Serial.println(F("  >>> Dopasowano docelowy tracker! Tworzę głęboką kopię obiektu w pamięci."));
            myDevice = new NimBLEAdvertisedDevice(*advertisedDevice); 
            doConnect = true;
          }
        }
      }
    }
};

void initBLE() {
  if (!btInitialized) {
    NimBLEDevice::init("FLARM_Strobe_Client");
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEScan* pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true); 
    pBLEScan->setInterval(97); 
    pBLEScan->setWindow(37);
    btInitialized = true;
  }
}

void deinitBLE() {
  if (btInitialized) {
    NimBLEDevice::deinit(true); 
    btInitialized = false;
    Serial.println(F("[BLE] Wygaszono sprzętowe radio Bluetooth."));
  }
}

// ZMINIFIKOWANY KOD HTML INTERFEJSU (Przywrócono klasyczny tag <select>)
const char index_html[] PROGMEM = R"rawliteral(<!DOCTYPE HTML><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Dariusz Fly Glider Flesher</title><style>body{font-family:Arial,sans-serif;padding:15px;background-color:#1e1e1e;color:#fff}.container{max-width:420px;margin:0 auto;background:#2d2d2d;padding:20px;border-radius:8px;box-shadow:0 4px 10px rgba(0,0,0,.5)}h2{color:#007BFF;text-align:center;margin-top:0;margin-bottom:15px;font-size:20px}label{font-size:14px;font-weight:bold}input[type=password],input[type=text],select{width:100%;padding:10px;margin:6px 0 12px;box-sizing:border-box;font-size:15px;background:#3a3a3a;color:#fff;border:1px solid #555;border-radius:4px}.password-wrapper{position:relative;width:100%}.password-wrapper input{padding-right:40px}.eye-icon{position:absolute;right:10px;top:10px;cursor:pointer;user-select:none;font-size:18px}input[type=checkbox]{transform:scale(1.4);margin-right:10px}input[type=submit]{width:100%;padding:12px;background-color:#28a745;color:#fff;border:none;cursor:pointer;font-weight:bold;font-size:16px;margin-top:10px;border-radius:5px}input[type=submit]:hover{background-color:#218838}.checkbox-container{margin:8px 0 12px;display:flex;align-items:center}.section-title{font-size:15px;color:#17a2b8;border-bottom:1px solid #444;padding-bottom:4px;margin-top:15px;margin-bottom:10px;font-weight:bold}.status-badge{display:inline-block;padding:4px 10px;border-radius:4px;font-size:13px;font-weight:bold;margin-bottom:10px}.status-connected{background-color:#28a745;color:#fff}.status-disconnected{background-color:#dc3545;color:#fff}.status-disabled{background-color:#6c757d;color:#fff}.status-scanning{background-color:#ffc107;color:#000}.info-box{background:#222;padding:8px 12px;border-radius:4px;margin-bottom:12px;border-left:3px solid #17a2b8;font-size:13px}textarea{width:100%;height:130px;background-color:#111;color:#00ff66;border:1px solid #444;font-family:monospace;font-size:11px;padding:8px;box-sizing:border-box;resize:none;border-radius:4px}</style></head><body><div class="container"><h2>Dariusz Fly Glider Flesher</h2><div class="section-title">Status Systemu</div><div>Status BLE: <span id="ble_status" class="status-badge status-disabled">Inicjalizacja...</span></div><div class="info-box">Temp. ESP32: <strong><span id="temp_val">--</span> °C</strong></div><form action="/set" method="GET"><div class="section-title">Ochrona Termiczna (ESP32)</div><label>Górny limit (°C):</label><select name="temp_upper">%TEMP_UPPER_OPTS%</select><label>Dolny limit (°C):</label><select name="temp_lower">%TEMP_LOWER_OPTS%</select><div class="section-title">Sieć Wi-Fi (AP)</div><label>SSID:</label><input type="text" name="wifi_ssid" value="%WIFI_SSID%" maxlength="32" required><label>Hasło (min. 8 znaków):</label><div class="password-wrapper"><input type="password" id="wifi_pass" name="wifi_pass" value="%WIFI_PASS%" minlength="8" maxlength="64" required><span class="eye-icon" onclick="togglePassword()">👁️</span></div><label>Wyłącz Wi-Fi po:</label><select name="wifi_to">%WIFI_TO_OPTS%</select><div class="section-title">Port Szeregowy (FLARM)</div><label>Prędkość UART:</label><select name="baud">%BAUD_OPTS%</select><div class="section-title">OGN Tracker BLE</div><label>Wybierz urządzenie BLE:</label><select name="bt_id">%BT_OPTIONS%</select><div class="checkbox-container"><input type="checkbox" name="bt_enable" value="1" %BT_CHECKED%><label>Włącz Bluetooth</label></div><div class="section-title">Błyski</div><label>Ilość w paczce:</label><select name="blinks_cnt">%BLINK_OPTIONS%</select><label>Prewencyjne:</label><select name="interval">%OPTIONS%</select><div class="checkbox-container"><input type="checkbox" name="disable" value="1" %CHECKED%><label>Błyski na postoju</label></div><input type="submit" value="Zapisz ustawienia"></form><div class="section-title">NMEA (Live)</div><textarea id="nmea_logs" readonly></textarea><br><br><center><a href="/update" style="color:#17a2b8;font-size:12px;text-decoration:none;">>>> Aktualizacja Firmware (OTA) <<<</a></center></div><script>function togglePassword(){const e=document.getElementById('wifi_pass');e.type='password'===e.type?'text':'password'}function updateLiveStatus(){fetch('/status').then(e=>e.json()).then(e=>{const t=document.getElementById('ble_status');t.innerText=e.ble_text,t.className='status-badge '+e.ble_class,document.getElementById('temp_val').innerText=e.temp,document.getElementById('nmea_logs').value=e.nmea_logs}).catch(e=>console.error(e))}setInterval(updateLiveStatus,1e3),updateLiveStatus();</script></body></html>)rawliteral";

// KOD HTML STRONY OTA
const char ota_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Aktualizacja OTA</title>
<style>body{font-family:Arial,sans-serif;padding:20px;background-color:#1e1e1e;color:#fff;text-align:center;}.box{max-width:400px;margin:0 auto;background:#2d2d2d;padding:20px;border-radius:8px;}input[type=submit]{margin-top:15px;padding:10px;background:#28a745;color:#fff;border:none;border-radius:5px;cursor:pointer;}</style></head>
<body><div class="box"><h2>Aktualizacja Firmware</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
  <input type='file' name='update' accept='.bin'><br><br>
  <input type='submit' value='Wgraj i Zaktualizuj'>
</form><br><a href="/" style="color:#17a2b8;">Powrót do menu</a></div></body></html>
)rawliteral";

void addNmeaToLog(String sentence) {
  nmeaLogs[nmeaLogIndex] = sentence;
  nmeaLogIndex = (nmeaLogIndex + 1) % 8;
}

String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;
  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void processIncomingChar(char c, String &buffer) {
  if (c == '\n') {
    buffer.trim(); 
    if (buffer.length() > 0) {
      bool hasAlarm = false;
      addNmeaToLog(buffer);

      if (buffer.startsWith("$PFLAU")) {
        String alarmStr = getValue(buffer, ',', 5);
        if (alarmStr.length() > 0) {
          int newAlarmLevel = alarmStr.toInt();
          if (newAlarmLevel > 0) hasAlarm = true;
          if (newAlarmLevel >= 0 && newAlarmLevel <= 3) {
            currentAlarmLevel = newAlarmLevel;
            lastMessageTime = millis();
          }
        }
      } 
      else if (buffer.startsWith("$PFLAA")) {
        String alarmStr = getValue(buffer, ',', 1);
        if (alarmStr.length() > 0 && alarmStr.toInt() > 0) hasAlarm = true;
      }
      
      if (hasAlarm) Serial.print("! ");
      Serial.println(buffer);
    }
    buffer = ""; 
  } 
  else if (c != '\r') buffer += c;
}

static void notifyCallback(NimBLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    for (size_t i = 0; i < length; i++) processIncomingChar((char)pData[i], bleBuffer);
}

// POŁĄCZENIE Z TRACKEREM + ALGORYTM RF COEXISTENCE
bool connectToTracker(const NimBLEAdvertisedDevice* device) {
    if (device == nullptr) return false;
    
    Serial.print(F("[BLE] Próba nawiązania połączenia (MAC/ID: "));
    Serial.print(device->getAddress().toString().c_str());
    Serial.println(F(")..."));

    if (pClient == nullptr) {
        pClient = NimBLEDevice::createClient();
    }

    bool wifiWasActive = (wifiActive && !thermalShutdown && WiFi.getMode() == WIFI_AP);
    if (wifiWasActive) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(150); 
    }
    
    bool connected = pClient->connect(device);

    if (wifiWasActive) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(wifiSsid.c_str(), wifiPassword.c_str());
        delay(50);
    }

    if (!connected) {
        Serial.println(F("[BLE BŁĄD] Urządzenie odrzuciło połączenie."));
        return false;
    }
    
    NimBLERemoteService* pRemoteService = pClient->getService(serviceUUID_NUS);
    if (pRemoteService != nullptr) {
        pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID_NUS_TX);
    } else {
        pRemoteService = pClient->getService(serviceUUID_HM10);
        if (pRemoteService != nullptr) pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID_HM10);
    }
    
    if (pRemoteCharacteristic == nullptr) {
        Serial.println(F("[BLE BŁĄD] Brak usługi UART."));
        pClient->disconnect(); 
        return false; 
    }
    
    if (pRemoteCharacteristic->canNotify()) {
        pRemoteCharacteristic->subscribe(true, notifyCallback);
        Serial.println(F("[BLE SUKCES] Nasłuchuję danych NMEA!"));
    } else {
        pClient->disconnect(); 
        return false;
    }
    
    btConnected = true;
    return true;
}

void handleRoot() {
  String html = FPSTR(index_html);
  html.replace("%WIFI_SSID%", wifiSsid);
  html.replace("%WIFI_PASS%", wifiPassword);

  String wifiToOpts = "";
  int toValues[] = {5, 10, 0};
  String toNames[] = {"5 minut", "10 minut", "Praca ciągła"};
  for (int i=0; i<3; i++) {
    wifiToOpts += "<option value=\"" + String(toValues[i]) + "\"";
    if (wifiTimeoutMinutes == toValues[i]) wifiToOpts += " selected";
    wifiToOpts += ">" + toNames[i] + "</option>";
  }
  html.replace("%WIFI_TO_OPTS%", wifiToOpts);

  String baudOpts = "";
  int baudRates[] = {4800, 19200, 38400, 115200};
  for (int i=0; i<4; i++) {
    baudOpts += "<option value=\"" + String(baudRates[i]) + "\"";
    if (uartBaudRate == baudRates[i]) baudOpts += " selected";
    baudOpts += ">" + String(baudRates[i]) + " baud</option>";
  }
  html.replace("%BAUD_OPTS%", baudOpts);

  String upperOpts = "";
  for (int i = 60; i <= 80; i += 5) {
    upperOpts += "<option value=\"" + String(i) + "\"";
    if (i == tempUpperLimit) upperOpts += " selected";
    upperOpts += ">" + String(i) + " &deg;C</option>\n";
  }
  html.replace("%TEMP_UPPER_OPTS%", upperOpts);

  String lowerOpts = "";
  for (int i = 50; i <= 70; i += 5) {
    lowerOpts += "<option value=\"" + String(i) + "\"";
    if (i == tempLowerLimit) lowerOpts += " selected";
    lowerOpts += ">" + String(i) + " &deg;C</option>\n";
  }
  html.replace("%TEMP_LOWER_OPTS%", lowerOpts);

  String blinkOptions = "";
  for (int i = 2; i <= 4; i++) {
    blinkOptions += "<option value=\"" + String(i) + "\"";
    if (i == blinksPerPacket) blinkOptions += " selected";
    blinkOptions += ">" + String(i) + "</option>\n";
  }
  html.replace("%BLINK_OPTIONS%", blinkOptions);

  // Generowanie klasycznej listy rozwijanej z inteligentnym doklejaniem nieznalezionego urządzenia
  String btOptions = "";
  bool currentInList = false;
  for (int i = 0; i < discoveredCount; i++) {
    btOptions += "<option value=\"" + discoveredDevices[i] + "\"";
    if (discoveredDevices[i].equalsIgnoreCase(btTrackerID)) { 
      btOptions += " selected"; 
      currentInList = true; 
    }
    btOptions += ">" + discoveredDevices[i] + "</option>\n";
  }
  
  if (!currentInList && btTrackerID.length() > 0) {
    btOptions = "<option value=\"" + btTrackerID + "\" selected>" + btTrackerID + "</option>\n" + btOptions;
  }
  if (discoveredCount == 0 && !currentInList) {
    btOptions = "<option value=\"\">Brak urządzeń w eterze...</option>\n";
  }
  html.replace("%BT_OPTIONS%", btOptions);

  String options = "<option value=\"0\"";
  if (preventiveFlashInterval == 0 || disablePreventiveFlash) options += " selected";
  options += ">Wyłączone</option>\n";

  int currentSec = preventiveFlashInterval / 1000;
  for (int i = 5; i <= 30; i += 5) {
    options += "<option value=\"" + String(i) + "\"";
    if (i == currentSec && !disablePreventiveFlash && preventiveFlashInterval > 0) options += " selected";
    options += ">" + String(i) + " s</option>\n";
  }
  
  html.replace("%BT_CHECKED%", enableBT ? "checked" : "");
  html.replace("%OPTIONS%", options);
  html.replace("%CHECKED%", disablePreventiveFlash ? "checked" : "");
  server.send(200, "text/html", html);
}

void handleStatus() {
  String bleText = "", bleClass = "";
  if (thermalShutdown) { bleText = "Ochrona termiczna"; bleClass = "status-disconnected"; } 
  else if (!enableBT) { bleText = "Wyłączony"; bleClass = "status-disabled"; } 
  else if (btConnected) { bleText = btTrackerID; bleClass = "status-connected"; } 
  else { bleText = "Skanowanie w tle..."; bleClass = "status-scanning"; }

  float tempC = temperatureRead();
  String nmeaText = "";
  for (int i = 0; i < 8; i++) {
    int idx = (nmeaLogIndex + i) % 8;
    if (nmeaLogs[idx].length() > 0) nmeaText += nmeaLogs[idx] + "\n";
  }
  nmeaText.replace("\"", "\\\"");
  nmeaText.replace("\n", "\\n");

  String json = "{\"ble_text\":\"" + bleText + "\",\"ble_class\":\"" + bleClass + "\",\"temp\":\"" + String(tempC, 1) + "\",\"nmea_logs\":\"" + nmeaText + "\"}";
  server.send(200, "application/json", json);
}

void handleSet() {
  bool wifiChanged = false;

  if (server.hasArg("wifi_ssid") && server.hasArg("wifi_pass")) {
    String newSsid = server.arg("wifi_ssid");
    String newPass = server.arg("wifi_pass");
    newSsid.trim(); newPass.trim();
    if (newSsid.length() > 0 && newPass.length() >= 8) {
      if (newSsid != wifiSsid || newPass != wifiPassword) {
        wifiSsid = newSsid; wifiPassword = newPass; wifiChanged = true;
      }
    }
  }

  if (server.hasArg("wifi_to")) wifiTimeoutMinutes = server.arg("wifi_to").toInt();
  if (server.hasArg("temp_upper")) tempUpperLimit = server.arg("temp_upper").toInt();
  if (server.hasArg("temp_lower")) tempLowerLimit = server.arg("temp_lower").toInt();

  if (server.hasArg("baud")) {
    int newBaud = server.arg("baud").toInt();
    if (newBaud == 4800 || newBaud == 19200 || newBaud == 38400 || newBaud == 115200) {
      if (newBaud != uartBaudRate) {
        uartBaudRate = newBaud;
        Serial1.updateBaudRate(uartBaudRate); 
      }
    }
  }

  if (server.hasArg("bt_id")) {
    String newID = server.arg("bt_id");
    newID.trim();
    if (newID.length() > 0 && newID != btTrackerID) {
      btTrackerID = newID;
      if (pClient && pClient->isConnected()) pClient->disconnect();
      btConnected = false;
    }
  }

  bool newEnableBT = server.hasArg("bt_enable");
  if (newEnableBT != enableBT) {
    enableBT = newEnableBT;
    if (!enableBT) {
      if (pClient && pClient->isConnected()) pClient->disconnect();
      btConnected = false; deinitBLE(); 
    } else initBLE(); 
  }

  if (server.hasArg("blinks_cnt")) {
    int cnt = server.arg("blinks_cnt").toInt();
    if (cnt >= 2 && cnt <= 4) blinksPerPacket = cnt;
  }

  if (server.hasArg("interval")) {
    int seconds = server.arg("interval").toInt();
    if (seconds == 0) { preventiveFlashInterval = 0; disablePreventiveFlash = true; } 
    else if (seconds >= 5 && seconds <= 30) {
      preventiveFlashInterval = seconds * 1000;
      disablePreventiveFlash = server.hasArg("disable");
    }
  } else disablePreventiveFlash = server.hasArg("disable");
  
  preferences.putString("ssid", wifiSsid);
  preferences.putString("pass", wifiPassword);
  preferences.putUInt("wifi_to", wifiTimeoutMinutes);
  preferences.putInt("baud", uartBaudRate);
  preferences.putString("bt_id", btTrackerID);
  preferences.putBool("bt_en", enableBT);
  preferences.putUInt("bl_cnt", blinksPerPacket);
  preferences.putUInt("intvl", preventiveFlashInterval);
  preferences.putBool("dis_prev", disablePreventiveFlash);
  preferences.putInt("t_up", tempUpperLimit);
  preferences.putInt("t_dn", tempLowerLimit);

  server.sendHeader("Location", "/");
  server.send(303);

  if (wifiChanged && !thermalShutdown) {
    delay(100);
    WiFi.softAPdisconnect(true);
    WiFi.softAP(wifiSsid.c_str(), wifiPassword.c_str());
  }
}

void setupOTA() {
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", ota_html);
  });
  
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      server.send(200, "text/plain", "Blad aktualizacji!");
    } else {
      String ip = WiFi.softAPIP().toString();
      String successHtml = "<html><head><meta charset=\"UTF-8\">";
      successHtml += "<meta http-equiv=\"refresh\" content=\"10;url=http://" + ip + "/\">";
      successHtml += "</head><body style=\"background-color:#1e1e1e;color:#fff;text-align:center;font-family:Arial;padding-top:50px;\">";
      successHtml += "<h2 style=\"color:#28a745;\">Aktualizacja wgrana pomyslnie!</h2>";
      successHtml += "<p>Trwa restartowanie urzadzenia (ok. 10 sekund)...<br>Zostaniesz automatycznie przekierowany na strone glowna.</p>";
      successHtml += "</body></html>";
      server.send(200, "text/html", successHtml);
    }
    delay(1000); ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA] Rozpoczeto wgrywanie pliku: %s\n", upload.filename.c_str());
      deinitBLE(); 
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!Update.end(true)) { Update.printError(Serial); }
    }
  });
}

void checkThermalProtection(unsigned long currentMillis) {
  static unsigned long lastTempCheck = 0;
  if (currentMillis - lastTempCheck < 1500) return;
  lastTempCheck = currentMillis;

  float tempC = temperatureRead();

  if (!thermalShutdown && tempC > tempUpperLimit) {
    thermalShutdown = true;
    Serial.println(F("[SYSTEM] Ochrona termiczna: WYŁ Wi-Fi/BLE"));
    if (pClient && pClient->isConnected()) pClient->disconnect();
    btConnected = false;
    deinitBLE(); 
    if (wifiActive) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); }
  } 
  else if (thermalShutdown && tempC <= tempLowerLimit) {
    thermalShutdown = false;
    Serial.println(F("[SYSTEM] Ochrona termiczna: WŁ Wi-Fi/BLE"));
    if (wifiActive && (wifiTimeoutMinutes == 0 || currentMillis < (wifiTimeoutMinutes * 60000UL))) {
      WiFi.mode(WIFI_AP); WiFi.softAP(wifiSsid.c_str(), wifiPassword.c_str()); server.begin();
    }
    if (enableBT) initBLE(); 
  }
}

// ==========================================
// SEKWENCYJNY START
// ==========================================
void setup() {
  Serial.begin(115200); 
  Serial.println(F("\n--- DARIUSZ FLY GLIDER FLASHER START ---"));
  
  pinMode(MOSFET_PIN, OUTPUT); pinMode(BUILTIN_LED_PIN, OUTPUT);
  setLights(false);

  Serial.println(F("[1] Sprzetowy test LED..."));
  bootBlinks(4);

  Serial.println(F("[2] Odczyt pamieci Flash NVM..."));
  preferences.begin("strobe_cfg", false);
  if (!preferences.isKey("ssid")) {
    wifiSsid = "FLARM_Strobe_" + generateRandomSuffix();
    preferences.putString("ssid", wifiSsid);
  } else wifiSsid = preferences.getString("ssid", "FLARM_Strobe");
  
  wifiPassword = preferences.getString("pass", "szybowiec");
  wifiTimeoutMinutes = preferences.getUInt("wifi_to", 10);
  uartBaudRate = preferences.getInt("baud", 19200);
  btTrackerID = preferences.getString("bt_id", "OGN604044"); 
  enableBT = preferences.getBool("bt_en", true);
  blinksPerPacket = preferences.getUInt("bl_cnt", 3);
  preventiveFlashInterval = preferences.getUInt("intvl", 15000);
  disablePreventiveFlash = preferences.getBool("dis_prev", false);
  tempUpperLimit = preferences.getInt("t_up", 75);
  tempLowerLimit = preferences.getInt("t_dn", 60);

  Serial1.begin(uartBaudRate, SERIAL_8N1, FLARM_RX_PIN, FLARM_TX_PIN);
  uartBuffer.reserve(100); 
  bleBuffer.reserve(100);

  if (enableBT) {
    Serial.println(F("[3] Uruchamianie Bluetooth (NimBLE)..."));
    initBLE();
    
    Serial.println(F("[4] Skanowanie poczatkowe (zamrożenie systemu na 10 sekund)..."));
    
    // Uruchomienie ciągłego skanowania w tle
    discoveredCount = 0;
    NimBLEDevice::getScan()->start(0, false);
    
    // Twarde, manualne zamrożenie procesora, aby umożliwić nasłuch na równe 10 sekund
    unsigned long startM = millis();
    while(millis() - startM < 10000) {
      delay(50); // Odciąża FreeRTOS pozwalając na pracę radia
    }
    
    // Ręczne ubicie skanera
    NimBLEDevice::getScan()->stop();
    Serial.println(F("[BLE] Koniec twardego skanowania startowego."));

    if (myDevice != nullptr) {
        Serial.println(F("[BLE] Tracker zdefiniowany w ustawieniach został odnaleziony. Łączę..."));
        connectToTracker(myDevice);
        delete myDevice;
        myDevice = nullptr; 
    } else {
        Serial.println(F("[BLE] Nie znaleziono zdefiniowanego trackera. Skanowanie dziala w tle."));
    }
  }

  Serial.println(F("[5] Startowanie sieci Wi-Fi i panelu WWW..."));
  WiFi.softAP(wifiSsid.c_str(), wifiPassword.c_str());
  
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/set", handleSet);
  setupOTA();
  server.begin();
  
  Serial.println(F("--- SYSTEM GOTOWY ---"));
}

void loop() {
  unsigned long currentMillis = millis();

  checkThermalProtection(currentMillis);

  // Wygaszacz punktu dostępowego
  if (wifiActive && wifiTimeoutMinutes > 0 && currentMillis > (wifiTimeoutMinutes * 60000UL)) {
    Serial.println(F("[SYSTEM] Koniec czasu konfiguracji. Wylaczam siec Wi-Fi."));
    WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
    wifiActive = false;
  }

  if (wifiActive && !thermalShutdown) server.handleClient();

  if (enableBT && btInitialized && !thermalShutdown) {
    
    // Jeżeli w locie skaner wyłapie tracker, bezpiecznie się połącz
    if (doConnect && myDevice != nullptr) {
      if (NimBLEDevice::getScan()->isScanning()) NimBLEDevice::getScan()->stop();
      
      connectToTracker(myDevice);
      doConnect = false;
      delete myDevice;
      myDevice = nullptr; 
    }
    
    // Asynchroniczne skanowanie w tle (nie wiesza WWW)
    if (!btConnected && !doConnect) {
      static unsigned long bgScanTimer = 0;
      static bool isBgScanning = false;
      
      if (!NimBLEDevice::getScan()->isScanning()) isBgScanning = false;

      // Co 30 sekund uruchom skanowanie
      if (!isBgScanning && (currentMillis - bgScanTimer > 30000)) { 
        bgScanTimer = currentMillis; 
        discoveredCount = 0;
        NimBLEDevice::getScan()->clearResults(); 
        NimBLEDevice::getScan()->start(0, false); 
        isBgScanning = true;
        Serial.println(F("[BLE TŁO] Startuję poszukiwania utraconego trackera... (10s)"));
      }
      
      // Po 10 sekundach ręcznie zamknij skaner
      if (isBgScanning && (currentMillis - bgScanTimer > 10000)) {
        NimBLEDevice::getScan()->stop(); 
        isBgScanning = false;
        Serial.println(F("[BLE TŁO] Przerwa. Kolejna próba za ok. 20s."));
      }
    }
    
    if (pClient != nullptr && !pClient->isConnected()) {
      if (btConnected) { 
        Serial.println(F("[BLE BŁĄD] Utracono fizyczne polaczenie z Trackerem!"));
        btConnected = false; 
      }
    }
  }

  while (Serial1.available()) {
    processIncomingChar(Serial1.read(), uartBuffer);
  }

  if (currentAlarmLevel > 0 && (currentMillis - lastMessageTime > 3000)) currentAlarmLevel = 0;

  if (!sequenceActive) {
    if (currentAlarmLevel > 0) {
      if (currentMillis - lastSequenceStart >= 1000) {
        startSequence(currentAlarmLevel); lastSequenceStart = currentMillis; lastPreventiveFlash = currentMillis;
      }
    } else {
      if (!disablePreventiveFlash && preventiveFlashInterval > 0) {
        if (currentMillis - lastPreventiveFlash >= preventiveFlashInterval) {
          startSequence(1); lastPreventiveFlash = currentMillis;
        }
      }
    }
  }

  if (sequenceActive) {
    unsigned long elapsed = currentMillis - stateTimer;
    switch (flashState) {
      case BLINK_ON:
        if (elapsed >= flashOnDuration) { setLights(false); stateTimer = currentMillis; flashState = BLINK_OFF; }
        break;
      case BLINK_OFF:
        if (elapsed >= flashOffDuration) {
          blinksInCurrentPacket++;
          if (blinksInCurrentPacket >= blinksPerPacket) {
            packetsCompleted++;
            if (packetsCompleted >= targetPackets) { sequenceActive = false; flashState = IDLE; setLights(false); } 
            else { stateTimer = currentMillis; flashState = INTER_PACKET_DELAY; }
          } else { setLights(true); stateTimer = currentMillis; flashState = BLINK_ON; }
        }
        break;
      case INTER_PACKET_DELAY:
        if (elapsed >= interPacketDelay) { blinksInCurrentPacket = 0; setLights(true); stateTimer = currentMillis; flashState = BLINK_ON; }
        break;
      case IDLE: break;
    }
  }
}