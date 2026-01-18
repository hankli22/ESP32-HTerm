/*
 * 项目：Esp32-sensor (Node 02 - 临时测试版)
 * 功能：去除深睡，串口输出详细电压调试信息，10秒一发
 */

#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <rom/crc.h>
#include <esp_wifi.h>

// --- 引脚定义 ---
const int S_VCC = 21; // D6 -> 传感器及分压网络正极
const int S_ADC = 3;  // D1 -> GPIO 3 (ADC)
const int S_SDA = 6;  // D4
const int S_SCL = 7;  // D5

const uint8_t RECEIVER_MAC[] = {0x54, 0x32, 0x04, 0x46, 0x37, 0x8C};

typedef struct struct_message {
    float t1, h1, t2, p2;
    uint8_t sen_typ, sen_num, status;
    uint32_t crc;
} struct_message;

struct_message myData;
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;

void setup() {
    Serial.begin(115200);
    delay(2000); // 给串口留出观察时间
    Serial.println("\n--- Node 02 Debug System Starting ---");

    // 1. 建立电源回路
    pinMode(S_VCC, OUTPUT);
    digitalWrite(S_VCC, HIGH); 
    Serial.println("Power (D6) set to HIGH.");
    delay(100); // 留出充足时间让电压稳定

    // 2. 传感器初始化
    Wire.begin(S_SDA, S_SCL);
    if (!aht.begin()) Serial.println("AHT20 not found!");
    if (!bmp.begin(0x77) && !bmp.begin(0x76)) Serial.println("BMP280 not found!");
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED);

    // 3. 通信初始化
    WiFi.mode(WIFI_STA);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
    }
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
    
    Serial.println("Init Done. Starting loop...");
}

void loop() {
    Serial.println("------------------------------------");

    // --- 步骤 1: 电压调试采集 ---
    analogRead(S_ADC); // 预读
    delay(10);
    int rawSum = 0;
    for(int i=0; i<10; i++) {
        rawSum += analogRead(S_ADC);
        delay(5);
    }
    int rawAvg = rawSum / 10;

    // ESP32-C3 ADC 公式 (11dB 衰减, 满量程约 3100mV)
    float v_pin = (rawAvg / 4095.0f) * 3.1f;
    float v_bat = v_pin * 1.4545f; // 1M+2.2M 还原

    // 映射电量: 3.4V (0%) -> 4.2V (100%)
    int pct = map((int)(v_bat * 100), 340, 420, 0, 100);
    myData.status = (uint8_t)constrain(pct, 0, 100);

    // 串口详细诊断信息输出
    Serial.printf("ADC Raw Avg: %d\n", rawAvg);
    Serial.printf("Pin Voltage: %.3f V\n", v_pin);
    Serial.printf("Calc Battery: %.3f V\n", v_bat);
    Serial.printf("Status Pct: %d%%\n", myData.status);

    // --- 步骤 2: 环境数据采集 ---
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    myData.t1 = temp.temperature;
    myData.h1 = humidity.relative_humidity;
    
    bmp.takeForcedMeasurement();
    myData.t2 = bmp.readTemperature();
    myData.p2 = bmp.readPressure() * 0.01f;

    myData.sen_typ = 1;
    myData.sen_num = 2;
    myData.crc = crc32_le(0, (uint8_t const *)&myData, sizeof(myData) - 4);

    // --- 步骤 3: 发送 ---
    esp_err_t result = esp_now_send(RECEIVER_MAC, (uint8_t *) &myData, sizeof(myData));
    
    if (result == ESP_OK) {
        Serial.println("ESP-NOW Send Success.");
    } else {
        Serial.println("ESP-NOW Send Failed.");
    }

    Serial.println("Wait 10 seconds...");
    delay(10000); 
}