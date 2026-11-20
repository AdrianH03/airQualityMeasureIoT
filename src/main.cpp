#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>

const char* WIFI_SSID = "WIFI_NEVE";
const char* WIFI_PASS = "WIFI_JELSZO";

const char* INFLUX_URL = "URL";
const char* INFLUX_TOKEN = "TOKEN";

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

    if(buf[0] != 0x42 || buf[1] != 0x40){
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
      sample.pressure = bme.readPressure();
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

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX2, PMS_TX2);
  if(!bme.begin(0x76, &Wire) && !bme.begin(0x77, &Wire)){
    Serial.println("[HIBA] BME280 nem található!");
    while(1) delay(10);
  }
  Serial.println("[OK] BME280 sikeresen inicializálva.");
  dataQueue = xQueueCreate(1, sizeof(AirQualityData));
  xTaskCreatePinnedToCore(sensorTask, "SensorTask", 4096, NULL, 2, NULL, 1);
  
  // TODO: implementálni kell a networkTask függvényt
  //xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelete(NULL);
}