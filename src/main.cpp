#define TINY_GSM_MODEM_A7672X
#define SerialAT Serial2

#include <Arduino.h>
#include <TinyGSM.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- CẤU HÌNH PHẦN CỨNG ---
#define MODEM_RX          16
#define MODEM_TX          17

#define RELAY_POWER_PIN   4    // Relay 1: Đóng/ngắt điện xe
#define RELAY_ALARM_PIN   5    // Relay 2: Chống trộm / còi
#define BATTERY_PIN       34   // Chân đọc Analog điện áp ắc quy

#define RELAY_ACTIVE      LOW  // Relay kích mức thấp
#define RELAY_INACTIVE    HIGH

// --- CẤU HÌNH MẠNG VIETTEL ---
const char apn[]      = "v-internet";
const char gprsUser[] = "";
const char gprsPass[] = "";

// --- CẤU HÌNH MQTT ---
const char* mqtt_server = "broker.emqx.io";
const int   mqtt_port   = 1883;
const char* bike_id     = "BIKE_01";

char topic_control[50];
char topic_telemetry[50];

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
PubSubClient mqttClient(client);

unsigned long lastMillis = 0;
bool isBikeUnlocked = false;
unsigned long rentalEndTime = 0;

void setupModem() {
  Serial.println("Đang khởi động Modem A7670C...");
  
  // Vẫn giữ 115200 baud như bạn đã dùng thành công
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  Serial.println("Đang kiểm tra giao tiếp với Modem...");
  if (!modem.testAT(2000)) {
    Serial.println("Lỗi: Không nhận được phản hồi AT qua UART (kiểm tra TX/RX, baud rate, mức logic và modem đã bật).");
    delay(10000);
    ESP.restart();
  }

  if (!modem.init()) {
    Serial.println("Lỗi: Modem có phản hồi AT nhưng khởi tạo thất bại; kiểm tra SIM và trạng thái modem.");
    delay(10000);
    ESP.restart();
  }

  Serial.println("Đang kết nối nhà mạng (có thể mất 10-30s)...");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println("Lỗi: Không tìm thấy sóng!");
    delay(10000);
    ESP.restart();
  }

  Serial.println("Đã có sóng! Đang kích hoạt GPRS...");
  
  // Cấu hình APN trực tiếp bằng lệnh AT (Đây là mấu chốt giúp bạn vượt qua lỗi Data)
  modem.sendAT("+CGDCONT=1,\"IP\",\"v-internet\"");
  modem.waitResponse();

  if (!modem.gprsConnect("v-internet")) {
    Serial.println("Lỗi: Kết nối Data thất bại! Thử lại...");
    delay(5000);
    if (!modem.gprsConnect("")) {
      Serial.println("Lỗi nghiêm trọng: Vẫn không thể kết nối Data. Khởi động lại.");
      delay(10000);
      ESP.restart();
    }
  }
  
  Serial.println(">> Đã kết nối Internet 4G thành công!");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  
  if (error) {
    Serial.println("Lỗi phân tích JSON từ MQTT");
    return;
  }

  const char* command = doc["command"];
  if (String(command) == "UNLOCK") {
    int duration = doc["duration"] | 60; // Mặc định mở 60 giây nếu không gửi thời gian
    isBikeUnlocked = true;
    rentalEndTime = millis() + (duration * 1000UL);
    
    digitalWrite(RELAY_POWER_PIN, RELAY_ACTIVE);
    Serial.println(">> LỆNH MQTT: ĐÃ MỞ KHÓA XE");
  }
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Đang kết nối MQTT Broker...");
    String clientId = "ESP32_Bike_" + String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("thành công!");
      mqttClient.subscribe(topic_control);
    } else {
      Serial.print("lỗi, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" - Thử lại sau 5 giây");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(10);

  // Khởi tạo trạng thái Relay
  pinMode(RELAY_POWER_PIN, OUTPUT);
  pinMode(RELAY_ALARM_PIN, OUTPUT);
  digitalWrite(RELAY_POWER_PIN, RELAY_INACTIVE);
  digitalWrite(RELAY_ALARM_PIN, RELAY_INACTIVE);

  // Khởi tạo Topic MQTT
  sprintf(topic_control, "bike/%s/control", bike_id);
  sprintf(topic_telemetry, "bike/%s/telemetry", bike_id);

  // Kết nối 4G và MQTT
  setupModem();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  // Giữ kết nối MQTT
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // Tự động khóa xe khi hết thời gian thuê
  if (isBikeUnlocked && millis() > rentalEndTime) {
    isBikeUnlocked = false;
    digitalWrite(RELAY_POWER_PIN, RELAY_INACTIVE);
    Serial.println(">> ĐÃ HẾT GIỜ THUÊ: TỰ ĐỘNG KHÓA XE");
  }

  // Gửi thông số trạng thái xe (Telemetry) lên server mỗi 10 giây
  if (millis() - lastMillis > 10000) {
    lastMillis = millis();
    
    // Đọc điện áp ắc quy (Mô phỏng)
    int battRaw = analogRead(BATTERY_PIN);
    float battVolts = (battRaw / 4095.0) * 3.3 * 2; 

    StaticJsonDocument<200> doc;
    doc["status"] = isBikeUnlocked ? "UNLOCKED" : "LOCKED";
    doc["battery"] = battVolts;
    
    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);
    mqttClient.publish(topic_telemetry, jsonBuffer);
  }
}