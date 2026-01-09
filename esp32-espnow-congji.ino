/*
 * 项目：LR 环境监测系统 - 传感器端 (Node 02 太阳能稳定版)
 * 硬件：Seeed XIAO ESP32-C3, AHT20, BMP280, 1M+2.2M 分压网络
 * 引脚：D6(VCC), D8(ADC 采样), D4/D5(I2C)
 */

#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <rom/crc.h>
#include <esp_wifi.h>

// --- 引脚配置 ---
const int S_VCC = 21; // D6 -> 传感器及分压网络正极
const int S_ADC = 8;  // D8 -> GPIO 8 (分压中心点)
const int S_SDA = 6;  // D4
const int S_SCL = 7;  // D5

// --- 通信与阈值 ---
const uint8_t RECEIVER_MAC[] = {0x54, 0x32, 0x04, 0x46, 0x37, 0x8C};
const float BATT_LOW_LIMIT = 3.50f; // 1500mAh 锂电池建议低压阈值
const uint64_t NORMAL_SLEEP = 10 * 1000000ULL; // 10秒
const uint64_t DEFENSE_SLEEP = 600 * 1000000ULL; // 低电量防守睡眠 10分钟

typedef struct struct_message {
    float t1, h1, t2, p2;
    uint8_t sen_typ, sen_num, status;
    uint32_t crc;
} struct_message;

struct_message myData;
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;

// 记录低电量状态的 RTC 变量
RTC_DATA_ATTR bool low_bat_flag = false;

void setup() {
    // 1. 启动硬件并给传感层供电
    setCpuFrequencyMhz(80); 
    pinMode(S_VCC, OUTPUT);
    digitalWrite(S_VCC, HIGH);
    delay(15); // 给 AHT20 启动及分压网络稳定留出时间

    // 2. 电池电压采样与低功耗决策
    // 分压计算：V_pin = V_bat * (2.2M / (1M + 2.2M)) -> V_bat = V_pin * 1.4545
    analogRead(S_ADC); delay(5); // 预热采样
    int raw = analogRead(S_ADC);
    float v_pin = (raw / 4095.0f) * 3.1f; // 假设 11dB 衰减满量程 3.1V
    float v_bat = v_pin * 1.4545f;

    // 如果电压过低，进入保护模式
    if (v_bat < BATT_LOW_LIMIT && v_bat > 2.0f) { // 2.0V以下视为未接电池
        low_bat_flag = true;
        digitalWrite(S_VCC, LOW);
        esp_sleep_enable_timer_wakeup(DEFENSE_SLEEP);
        esp_deep_sleep_start();
    }

    // 3. 传感器初始化
    Wire.begin(S_SDA, S_SCL);
    if (!aht.begin() || (!bmp.begin(0x76) && !bmp.begin(0x77))) {
        digitalWrite(S_VCC, LOW);
        esp_sleep_enable_timer_wakeup(NORMAL_SLEEP);
        esp_deep_sleep_start();
    }
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED);

    // 4. 数据采集
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    myData.t1 = temp.temperature;
    myData.h1 = humidity.relative_humidity;
    bmp.takeForcedMeasurement();
    myData.t2 = bmp.readTemperature();
    myData.p2 = bmp.readPressure() * 0.01f;

    myData.sen_typ = 1;
    myData.sen_num = 2; // Node 02
    
    // 状态位逻辑：如果是从低电量恢复后的首包，标记为 1
    myData.status = low_bat_flag ? 1 : 0;
    if (low_bat_flag) low_bat_flag = false;

    myData.crc = crc32_le(0, (uint8_t const *)&myData, sizeof(myData) - 4);

    // 5. 开启 Wi-Fi 并发送
    WiFi.mode(WIFI_STA);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    esp_wifi_set_max_tx_power(80); // 20dBm 满功率穿墙
    
    if (esp_now_init() == ESP_OK) {
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);
        esp_now_add_peer(&peerInfo);
        esp_now_send(RECEIVER_MAC, (uint8_t *) &myData, sizeof(myData));
        delay(20); // 给 LR 模式留出物理层传输延迟
    }

    // 6. 清场并深睡
    esp_wifi_stop();
    digitalWrite(S_VCC, LOW); // 切断传感器及分压网络电源
    esp_sleep_enable_timer_wakeup(NORMAL_SLEEP);
    esp_deep_sleep_start();
}

void loop() {}