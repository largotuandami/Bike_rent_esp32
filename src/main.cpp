#define TINY_GSM_MODEM_BG96

#include <Arduino.h>
#include <TinyGSM.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

class BleLogSerial {
 public:
  void begin(unsigned long) {
    NimBLEDevice::init("TDM2421-Logs");
    NimBLEServer *server = NimBLEDevice::createServer();
    NimBLEService *service = server->createService(
        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    logCharacteristic = service->createCharacteristic(
        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    recentLog = "TDM2421 BLE log ready\r\n";
    logCharacteristic->setValue(recentLog.c_str());
    service->start();
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->setName("TDM2421-Logs");
    advertising->addServiceUUID(service->getUUID());
    advertising->enableScanResponse(true);
    advertising->start();
  }

  void println(const char *message) {
    ::Serial.println(message);
    append(message);
    append("\r\n");
    publish();
  }

  void println(const String &message) {
    println(message.c_str());
  }

  void println(unsigned long value) {
    println(String(value));
  }

  void print(const char *message) {
    ::Serial.print(message);
    append(message);
    publish();
  }

  void heartbeat() {
    println("BLE alive");
  }

  void print(const String &message) {
    print(message.c_str());
  }

  void print(int value) {
    print(String(value));
  }

  void print(unsigned long value) {
    print(String(value));
  }

 private:
  NimBLECharacteristic *logCharacteristic = nullptr;
  String recentLog;

  void publish() {
    if (logCharacteristic == nullptr) {
      return;
    }
    logCharacteristic->setValue(recentLog.c_str());
    logCharacteristic->notify();
  }

  void append(const char *message) {
    recentLog += message;
    if (recentLog.length() > 320) {
      recentLog.remove(0, recentLog.length() - 320);
    }
  }
};

BleLogSerial BleSerial;

// --- CẤU HÌNH PHẦN CỨNG ---
#define simSerial Serial0
#define MCU_SIM_BAUDRATE 115200
#define MCU_SIM_TX_PIN 21
#define MCU_SIM_RX_PIN 20
#define MCU_SIM_EN_PIN 2
#define MCU_LED 10
#define MODEM_DTR 0

#define RELAY_POWER_PIN   4    // Relay 1: Đóng/ngắt điện xe
#define RELAY_LIGHT_PIN   5    // Relay 2: Nháy đèn
#define RELAY_WHEEL_PIN   6    // Relay 3: Khóa bánh sau
#define RELAY_SPEAKER_PIN 7    // Relay 4: Loa cảnh báo geofence
#define BATTERY_PIN        1   // ADC dự phòng theo bảng TDM2421

#define RELAY_ACTIVE      HIGH // Relay kích mức cao
#define RELAY_INACTIVE    LOW

// --- CẤU HÌNH MẠNG VIETTEL ---
const char apn[]      = "v-internet";
const char gprsUser[] = "";
const char gprsPass[] = "";

// --- CẤU HÌNH SERVER ---
const char* server_host = "www.tramsac.asia";
const int   server_port = 443;
const char* server_base_path = "/api/sync/";
const char* gps_base_path = "/api/gps/";
const char* bike_id     = "BIKE_01";

TinyGsm modem(simSerial);
TinyGsmClientSecure client(modem);

unsigned long lastSyncMillis = 0;
unsigned long lastBleHeartbeatMillis = 0;
bool isBikeUnlocked = false;
unsigned long rentalEndTime = 0;
bool warningTriggered = false;
volatile unsigned long flashNextMillis = 0;
volatile int flashesRemaining = 0;
volatile bool flashOn = false;
volatile bool speakerPending = false;
volatile unsigned long speakerOnMillis = 0;
TaskHandle_t relayEffectsTaskHandle = nullptr;
bool geofenceAlarmActive = false;
bool modemReady = false;
bool gpsAvailable = false;

void triggerRentalWarningIfDue();

void setLightRelay(bool on) {
  digitalWrite(RELAY_LIGHT_PIN, on ? RELAY_ACTIVE : RELAY_INACTIVE);
}

void stopFlashes();

void startFlashes(int count) {
  if (count <= 0) {
    stopFlashes();
    return;
  }
  flashOn = true;
  flashesRemaining = (count * 2) - 1;
  flashNextMillis = millis() + 500;
  setLightRelay(true);
}

void stopFlashes() {
  flashesRemaining = 0;
  flashOn = false;
  setLightRelay(false);
}

void scheduleSpeakerOn() {
  speakerPending = true;
  speakerOnMillis = millis() + 5000;
  digitalWrite(RELAY_SPEAKER_PIN, RELAY_INACTIVE);
}

void relayEffectsTask(void *) {
  for (;;) {
    unsigned long now = millis();
    if (flashesRemaining > 0 &&
        static_cast<long>(now - flashNextMillis) >= 0) {
      flashOn = !flashOn;
      setLightRelay(flashOn);
      flashNextMillis = now + 500;
      --flashesRemaining;
    } else if (flashesRemaining <= 0) {
      if (flashOn) {
        flashOn = false;
      }
      setLightRelay(false);
    }

    if (speakerPending &&
        static_cast<long>(now - speakerOnMillis) >= 0) {
      speakerPending = false;
      digitalWrite(RELAY_SPEAKER_PIN, RELAY_ACTIVE);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void applyLockedState() {
  stopFlashes();
  speakerPending = false;
  digitalWrite(RELAY_POWER_PIN, RELAY_INACTIVE);
  digitalWrite(RELAY_SPEAKER_PIN, RELAY_INACTIVE);
  digitalWrite(RELAY_WHEEL_PIN, RELAY_ACTIVE);
}

void setupModem() {
  BleSerial.println("Dang khoi dong Modem Quectel EG800K...");
  
  pinMode(MODEM_DTR, OUTPUT);
  digitalWrite(MODEM_DTR, LOW);
  pinMode(MCU_SIM_EN_PIN, OUTPUT);
  // GPIO2 được nối tới mạch bật modem; tạo một chu kỳ bật để modem khởi động.
  digitalWrite(MCU_SIM_EN_PIN, LOW);
  delay(1000);
  digitalWrite(MCU_SIM_EN_PIN, HIGH);
  delay(5000);

  BleSerial.println("Dang kiem tra giao tiep voi Modem...");
  const unsigned long baudRates[] = {115200, 9600, 57600, 230400};
  unsigned long detectedBaud = 0;
  for (unsigned long baudRate : baudRates) {
    simSerial.end();
    simSerial.begin(baudRate, SERIAL_8N1, MCU_SIM_RX_PIN, MCU_SIM_TX_PIN);
    delay(300);
    while (simSerial.available()) {
      simSerial.read();
    }
    String response;
    for (int attempt = 0; attempt < 3 && response.indexOf("OK") < 0; ++attempt) {
      simSerial.print("AT\r\n");
      unsigned long deadline = millis() + 1000;
      while (millis() < deadline) {
        while (simSerial.available()) {
          response += static_cast<char>(simSerial.read());
        }
      }
    }
    if (response.indexOf("OK") >= 0) {
      detectedBaud = baudRate;
      BleSerial.print("Da phat hien baud UART: ");
      BleSerial.println(static_cast<unsigned long>(baudRate));
      break;
    }
  }

  if (detectedBaud == 0) {
    BleSerial.println("Loi: Khong nhan duoc phan hoi AT qua UART (kiem tra TX/RX, baud rate, muc logic va modem da bat).");
    return;
  }
  simSerial.begin(detectedBaud, SERIAL_8N1, MCU_SIM_RX_PIN, MCU_SIM_TX_PIN);

  if (!modem.init()) {
    BleSerial.println("Loi: Modem co phan hoi AT nhung khoi tao that bai; kiem tra SIM va trang thai modem.");
    return;
  }

  BleSerial.println("Dang ket noi nha mang (co the mat 10-30s)...");
  if (!modem.waitForNetwork(60000L)) {
    BleSerial.println("Loi: Khong tim thay song!");
    return;
  }

  BleSerial.println("Da co song! Dang kich hoat GPRS...");
  
  // Cấu hình APN trực tiếp cho EG800K.
  modem.sendAT("+CGDCONT=1,\"IP\",\"v-internet\"");
  modem.waitResponse();

  if (!modem.gprsConnect("v-internet")) {
    BleSerial.println("Loi: Ket noi Data that bai! Thu lai...");
    delay(5000);
    if (!modem.gprsConnect("")) {
      BleSerial.println("Loi nghiem trong: Van khong the ket noi Data.");
      return;
    }
  }
  
  String gpsStartResponse;
  modem.sendAT("+QGPS=1");
  int gpsStartStatus = modem.waitResponse(5000L, gpsStartResponse);
  BleSerial.print("GPS start: ");
  BleSerial.print(gpsStartStatus);
  BleSerial.print(" ");
  BleSerial.println(gpsStartResponse);
  gpsAvailable = true;
  if (gpsStartStatus != 1) {
    BleSerial.println("Canh bao: QGPS tra ve status khac OK, van thu QGPSLOC");
  }
  modemReady = true;
  BleSerial.println(">> Da ket noi Internet 4G thanh cong!");
}

bool readServerResponse(String &response) {
  unsigned long deadline = millis() + 15000;
  unsigned long lastDataMillis = millis();
  while (millis() < deadline) {
    bool receivedData = false;
    while (client.available()) {
      response += static_cast<char>(client.read());
      receivedData = true;
    }
    if (receivedData) {
      lastDataMillis = millis();
    } else if (response.indexOf("\r\n\r\n") >= 0 &&
               millis() - lastDataMillis >= 500) {
      break;
    }
    triggerRentalWarningIfDue();
    delay(10);
  }
  client.stop();
  return response.indexOf("\r\n\r\n") >= 0;
}

bool parseGpsCoordinate(String token, double &coordinate) {
  token.trim();
  if (token.length() < 2) {
    return false;
  }

  char hemisphere = token.charAt(token.length() - 1);
  token.remove(token.length() - 1);
  double raw = token.toDouble();
  if (raw <= 0.0) {
    return false;
  }

  double degrees = floor(raw / 100.0);
  double minutes = raw - (degrees * 100.0);
  if (minutes < 0.0 || minutes >= 60.0) {
    return false;
  }

  coordinate = degrees + (minutes / 60.0);
  if (hemisphere == 'S' || hemisphere == 'W') {
    coordinate = -coordinate;
  } else if (hemisphere != 'N' && hemisphere != 'E') {
    return false;
  }
  return true;
}

bool syncWithServer(float batteryVolts) {
  String path = String(server_base_path) + bike_id + "/?batt=" + String(batteryVolts, 2);

  BleSerial.println("Dang dong bo voi server qua HTTPS...");
  if (!client.connect(server_host, server_port)) {
    BleSerial.println("Loi: Khong the ket noi HTTPS toi server");
    String sslState;
    modem.sendAT("+QSSLSTATE=1,0");
    modem.waitResponse(5000L, sslState);
    BleSerial.print("Trang thai SSL: ");
    BleSerial.println(sslState);
    return false;
  }

  client.print("GET ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(server_host);
  client.println("Connection: close");
  client.println();

  String response;
  if (!readServerResponse(response)) {
    BleSerial.println("Loi: Phan hoi server khong hop le");
    return false;
  }

  int bodyStart = response.indexOf("\r\n\r\n");
  String body = response.substring(bodyStart + 4);
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    BleSerial.println("Loi phan tich JSON tu server");
    return false;
  }

  const char* command = doc["command"];
  if (String(command) == "UNLOCK") {
    int duration = doc["duration"] | 60; // Mặc định mở 60 giây nếu không gửi thời gian
    unsigned long now = millis();
    unsigned long extension = static_cast<unsigned long>(duration) * 1000UL;
    rentalEndTime = isBikeUnlocked && static_cast<long>(now - rentalEndTime) < 0
      ? rentalEndTime + extension
      : now + extension;
    isBikeUnlocked = true;
    warningTriggered = false;
    geofenceAlarmActive = false;
    digitalWrite(RELAY_WHEEL_PIN, RELAY_INACTIVE);
    digitalWrite(RELAY_SPEAKER_PIN, RELAY_INACTIVE);
    
    digitalWrite(RELAY_POWER_PIN, RELAY_ACTIVE);
    startFlashes(2);
    scheduleSpeakerOn();
    BleSerial.println(">> LENH HTTPS: DA MO KHOA XE, NHAY DEN 2 LAN");
  } else if (String(command) == "LOCK") {
    isBikeUnlocked = false;
    warningTriggered = false;
    geofenceAlarmActive = false;
    applyLockedState();
    startFlashes(1);
    BleSerial.println(">> LENH HTTPS: DA KHOA XE");
  }
  return true;
}

void sendGpsUpdate() {
  String gpsResponse;
  modem.sendAT("+QGPSLOC=0");
  int gpsStatus = modem.waitResponse(5000L, gpsResponse);
  int locationStart = gpsResponse.indexOf("+QGPSLOC:");
  if (gpsStatus != 1 && locationStart < 0) {
    BleSerial.print("GPS loc status: ");
    BleSerial.print(gpsStatus);
    BleSerial.print(" ");
    BleSerial.println(gpsResponse);
    BleSerial.println("Loi: GPS chua co fix hoac modem khong phan hoi");
    return;
  }
  BleSerial.print("GPS raw: ");
  BleSerial.println(gpsResponse);
  if (locationStart < 0) {
    BleSerial.println("Loi: QGPSLOC khong co du lieu");
    return;
  }
  String location = gpsResponse.substring(locationStart + 9);
  String fields[12];
  int fieldCount = 0;
  int fieldStart = 0;
  while (fieldCount < 12) {
    int comma = location.indexOf(',', fieldStart);
    if (comma < 0) {
      fields[fieldCount++] = location.substring(fieldStart);
      break;
    }
    fields[fieldCount++] = location.substring(fieldStart, comma);
    fieldStart = comma + 1;
  }
  if (fieldCount < 3) {
    BleSerial.println("Loi: QGPSLOC thieu truong toa do");
    return;
  }

  fields[0].trim();
  fields[1].trim();
  fields[2].trim();

  double latitude = 0.0;
  double longitude = 0.0;
  if (!parseGpsCoordinate(fields[1], latitude) ||
      !parseGpsCoordinate(fields[2], longitude)) {
    BleSerial.println("Loi: Khong the phan tich toa do GPS");
    return;
  }
  if (latitude == 0.0 && longitude == 0.0) {
    return;
  }

  String path = String(gps_base_path) + bike_id + "/";
  if (!client.connect(server_host, server_port)) {
    BleSerial.println("Loi: Khong the gui GPS toi server");
    return;
  }

  StaticJsonDocument<160> request;
  request["latitude"] = latitude;
  request["longitude"] = longitude;
  String body;
  serializeJson(request, body);
  BleSerial.print("GPS: ");
  BleSerial.print(String(latitude, 6));
  BleSerial.print(",");
  BleSerial.println(String(longitude, 6));
  client.print("POST ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(server_host);
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(body.length());
  client.println("Connection: close");
  client.println();
  client.print(body);

  String response;
  if (!readServerResponse(response)) {
    BleSerial.println("Loi: Server khong tra ve phan hoi GPS");
    return;
  }
  int bodyStart = response.indexOf("\r\n\r\n");
  StaticJsonDocument<256> result;
  String responseBody = response.substring(bodyStart + 4);
  if (deserializeJson(result, responseBody)) {
    BleSerial.println("Loi: Phan tich phan hoi GPS that bai");
    return;
  }
  if (!result.containsKey("inside_geofence")) {
    BleSerial.print("Loi: API GPS khong co inside_geofence: ");
    BleSerial.println(responseBody);
    return;
  }
  bool inside = result["inside_geofence"].as<bool>();
  BleSerial.print("Geofence: ");
  BleSerial.println(inside ? "TRONG VUNG" : "NGOAI VUNG");
  if (!inside) {
    geofenceAlarmActive = true;
    digitalWrite(RELAY_SPEAKER_PIN, RELAY_ACTIVE);
    digitalWrite(RELAY_WHEEL_PIN, RELAY_ACTIVE);
    digitalWrite(RELAY_POWER_PIN, RELAY_INACTIVE);
    isBikeUnlocked = false;
    BleSerial.println(">> CANH BAO: XE RA NGOAI VUNG AN TOAN");
  }
}

void setup() {
  Serial.begin(115200);
  BleSerial.begin(115200);
  delay(10);

  // Khởi tạo trạng thái Relay
  pinMode(MCU_LED, OUTPUT);
  pinMode(RELAY_POWER_PIN, OUTPUT);
  pinMode(RELAY_LIGHT_PIN, OUTPUT);
  pinMode(RELAY_SPEAKER_PIN, OUTPUT);
  pinMode(RELAY_WHEEL_PIN, OUTPUT);
  digitalWrite(MCU_LED, HIGH);
  digitalWrite(RELAY_POWER_PIN, RELAY_INACTIVE);
  digitalWrite(RELAY_LIGHT_PIN, RELAY_INACTIVE);
  digitalWrite(RELAY_SPEAKER_PIN, RELAY_INACTIVE);
  digitalWrite(RELAY_WHEEL_PIN, RELAY_INACTIVE);
  BaseType_t taskCreated = xTaskCreate(
      relayEffectsTask, "relay-effects", 4096, nullptr, 2,
      &relayEffectsTaskHandle);
  if (taskCreated != pdPASS) {
    BleSerial.println("Loi: Khong tao duoc task dieu khien relay GPIO5/GPIO7");
  }

  // Kết nối 4G để gọi API HTTPS
  setupModem();
}

void triggerRentalWarningIfDue() {
  if (!isBikeUnlocked || warningTriggered) {
    return;
  }

  long remaining = static_cast<long>(rentalEndTime - millis());
  if (remaining <= 30000 && remaining > 0) {
    warningTriggered = true;
    startFlashes(5);
    BleSerial.println(">> CANH BAO: CON 30 GIAY, NHAY DEN 5 LAN");
  }
}

void loop() {
  if (millis() - lastBleHeartbeatMillis >= 5000) {
    lastBleHeartbeatMillis = millis();
    BleSerial.heartbeat();
  }

  // Tự động khóa xe khi hết thời gian thuê
  if (isBikeUnlocked && static_cast<long>(millis() - rentalEndTime) >= 0) {
    isBikeUnlocked = false;
    applyLockedState();
    BleSerial.println(">> DA HET GIO THUE: TU DONG KHOA XE");
  }
  triggerRentalWarningIfDue();

  // Lấy lệnh và gửi điện áp lên server mỗi 5 giây
  if (modemReady && millis() - lastSyncMillis > 5000) {
    lastSyncMillis = millis();
    int battRaw = analogRead(BATTERY_PIN);
    float battVolts = (battRaw / 4095.0) * 3.3 * 2; 
    syncWithServer(battVolts);
    if (gpsAvailable) {
      sendGpsUpdate();
    }
  }
}