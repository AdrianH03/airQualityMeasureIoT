#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>

const char* WIFI_SSID = "WIFI_NEVE";
const char* WIFI_PASS = "WIFI_JELSZO";

const char* INFLUX_URL = ""; // élesben a szerver gép IP címét kell megadni
const char* INFLUX_TOKEN = "8c810f95291404189858d875aed8ec74292d76fc427edfee09396e69d31edbd9";
const char* INFLUX_ORG    = "iothw";
const char* INFLUX_BUCKET = "airquality";
const char* DEVICE_ID     = "aq-esp32-01";
const uint32_t SEND_INTERVAL_MS = 10000;


#define I2C_SDA 21
#define I2C_SCL 22
#define PMS_RX2 16
#define PMS_TX2 17

Adafruit_BME280 bme;
HardwareSerial pmsSerial(2);

struct AirQualityData {
  float temperature;
  float humidity;
  float pressure;
  uint16_t pm1_0;
  uint16_t pm2_5_raw;
  float pm2_5_corr;
  uint16_t pm10_raw;
  float pm10_corr;
  bool valid;
};

QueueHandle_t dataQueue;

bool readPMS7003(uint16_t &pm1, uint16_t &pm25, uint16_t &pm10){
  while(pmsSerial.available() >= 32){
    if(pmsSerial.peek() != 0x42){
      pmsSerial.read();
      continue;
    }
    uint8_t buf[32];
    pmsSerial.readBytes(buf, 32);

    if(buf[0] != 0x42 || buf[1] != 0x4D){
      continue;
    }

    uint16_t sum = 0;
    for(int i = 0; i < 30; i++){
      sum += buf[i];
    }
    uint16_t checksum = (buf[30] << 8) | buf[31];
    if(sum == checksum){
      pm1 = (buf[10] << 8) | buf[11];
      pm25 = (buf[12] << 8) | buf[13];
      pm10 = (buf[14] << 8) | buf[15];
      return true;
    }
  }
  return false;
}

float compensateHumidity(uint16_t pmRaw, float rh){
  if(rh <= 70.0f){
    return (float)pmRaw;
  }
  float clampedRh = (rh > 95.0f) ? 95.0f : rh;
  float rhFrac = clampedRh / 100.0f;
  float factor = 1.0f + 0.25f * ((rhFrac * rhFrac) / (1.0f - rhFrac));
  return (float)pmRaw / factor;
}

void sensorTask(void* pvParameters){
  uint16_t pm1 = 0, pm25 = 0, pm10 = 0;
  for(;;){
    if(readPMS7003(pm1, pm25, pm10)){
      AirQualityData sample;
      sample.temperature = bme.readTemperature();
      sample.humidity = bme.readHumidity();
      sample.pressure = bme.readPressure() / 100.0f;
      sample.pm1_0 = pm1;
      sample.pm2_5_raw = pm25;
      sample.pm2_5_corr = compensateHumidity(pm25, sample.humidity);
      sample.pm10_raw = pm10;
      sample.pm10_corr = compensateHumidity(pm10, sample.humidity);
      sample.valid = true;

      xQueueOverwrite(dataQueue, &sample);
      Serial.printf("[Mérés] T: %.1f C | RH: %.1f %% | P: %.1f hPa | PM2.5: %u (korr: %.1f) ug/m3 | PM10: %u ug/m3\n", sample.temperature, sample.humidity, sample.pressure, sample.pm2_5_raw, sample.pm2_5_corr, sample.pm10_raw);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

const char* wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL:  return "nem található a hálózat (rossz név? 5 GHz-es? túl messze van?)";
    case WL_CONNECT_FAILED: return "a csatlakozás elutasítva (rossz jelszó?)";
    case WL_CONNECTION_LOST:return "a kapcsolat megszakadt";
    case WL_DISCONNECTED:   return "nincs kapcsolat (gyakran rossz jelszó)";
    case WL_IDLE_STATUS:    return "várakozik";
    default:                return "ismeretlen állapot";
  }
}

void checkInfluxHealth() {
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(String(INFLUX_URL) + "/health");
  int code = http.GET();
  http.end();

  if (code == 200) {
    Serial.printf("[InfluxDB] Elérhető: %s\n", INFLUX_URL);
  } else {
    Serial.printf("[InfluxDB] NEM érhető el: %s (kód: %d)\n", INFLUX_URL, code);
    Serial.println("           Ellenőrizd: jó-e az IP cím, fut-e a Docker, ugyanazon a hálózaton");
    Serial.println("           van-e a gép, és engedi-e a tűzfal (és WSL esetén a hálózati mód) a 8086-os portot.");
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[WiFi] Kapcsolódás: %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] OK, IP: %s, jelerősség: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    checkInfluxHealth();
  } else {
    Serial.printf("[WiFi] Nem sikerült csatlakozni: %s\n", wifiStatusText(WiFi.status()));
    Serial.println("[WiFi] Újrapróbálom...");
    WiFi.disconnect();
  }
}

bool sendToInflux(const AirQualityData& s) {
  char line[300];
  snprintf(line, sizeof(line),
           "air_quality,device=%s "
           "temperature=%.2f,humidity=%.2f,pressure=%.2f,"
           "pm1_0=%u,pm2_5=%u,pm2_5_corr=%.1f,pm10=%u,pm10_corr=%.1f,rssi=%di",
           DEVICE_ID,
           s.temperature, s.humidity, s.pressure,
           s.pm1_0, s.pm2_5_raw, s.pm2_5_corr, s.pm10_raw, s.pm10_corr,
           (int)WiFi.RSSI());

  String url = String(INFLUX_URL) + "/api/v2/write?org=" + INFLUX_ORG +
               "&bucket=" + INFLUX_BUCKET;

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(url);
  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");

  int code = http.POST(String(line));
  if (code != 204) {
    Serial.printf("[HTTP] Hiba: %d  ", code);
    if (code == 401)     Serial.println("-> rossz a token (INFLUX_TOKEN)");
    else if (code == 404) Serial.println("-> rossz az org vagy a bucket neve");
    else if (code < 0)   Serial.println("-> nem érhető el a szerver (IP, tűzfal, Docker)");
    else                 Serial.println(http.getString().c_str());
  }
  http.end();
  return code == 204;
}

void networkTask(void* pvParameters) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);

  for (;;) {
    connectWiFi();

    AirQualityData sample;
    if (WiFi.status() == WL_CONNECTED &&
        xQueueReceive(dataQueue, &sample, 0) == pdTRUE) {
      if (sendToInflux(sample)) {
        Serial.printf("[HTTP] Elküldve: T=%.1f C, PM2.5=%u ug/m3\n",
                      sample.temperature, sample.pm2_5_raw);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Levegőminőség-mérő indul ===");
  dataQueue = xQueueCreate(1, sizeof(AirQualityData));
  Wire.begin(I2C_SDA, I2C_SCL);
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX2, PMS_TX2);
  if(!bme.begin(0x76, &Wire) && !bme.begin(0x77, &Wire)){
    Serial.println("[HIBA] BME280 nem található!");
    while(1) delay(10);
  }
  Serial.println("[OK] BME280 sikeresen inicializálva.");
  xTaskCreatePinnedToCore(sensorTask, "SensorTask", 4096, NULL, 2, NULL, 1);
  
  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelete(NULL);
}