/*
 * 项目：LR 环境监测系统 - 接收显示端 V8.0 (最终交互大成版)
 * 硬件：ESP32-C3, SH1106 OLED (128x64)
 * 特性：FreeRTOS(66FPS), 800kHz-I2C, 组合键逻辑, NVS存储, 三重巨浪, 动态图表
 */

#include <esp_now.h>
#include <WiFi.h>
#include <rom/crc.h> 
#include <esp_wifi.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <math.h> 
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ================= 1. 数据结构与全局定义 =================
typedef struct struct_message {
    float t1, h1, t2, p2;
    uint8_t sen_typ, sen_num, status;
    uint32_t crc;      
} struct_message;

uint8_t NEW_MAC[] = {0x54, 0x32, 0x04, 0x46, 0x37, 0x8C};
// 引脚配置: SDA=6, SCL=7 (AirM2M/Seeed C3)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 7, 6);

const int BTN_RIGHT = 18; 
const int BTN_LEFT  = 19; 
#define MAX_HIST 30 

// ================= 2. 逻辑控制类 =================
class LRReceiver {
public:
    // 节点数据
    struct_message nodeData[5]; 
    int8_t nodeRssi[5];
    unsigned long lastSeen[5] = {0};
    float nodeGap[5] = {0};
    bool nodeActive[5] = {false};
    
    // 历史数据库 (SRAM)
    float hT[5][MAX_HIST], hH[5][MAX_HIST], hP[5][MAX_HIST];
    int hCount[5] = {0};
    unsigned long lastLogTime[5] = {0};

    // UI 状态机
    int8_t currentPage = 1;      
    bool isDetailMode = false;   
    bool isSettingsMode = false; 
    int8_t detailSub = 0;        
    float scrollX = 128.0f, targetX = 128.0f; 
    float subScrollX = 0.0f, subTargetX = 0.0f;

    // 动画变量
    int16_t ping1 = 0, ping2 = 0, ping3 = 0;
    uint8_t heartbeat = 0;
    float smoothWidth = 0.0f;
    uint32_t lastPacketTick = 0;
    float cpuLoad = 0;
    uint32_t freeMem = 0;

    // 系统设置
    uint8_t setContrast = 128;
    uint8_t setScreenOff = 1;    // 0:永不, 1:30s, 2:1m
    int8_t setSelectIndex = 0;   
    unsigned long lastActivity = 0;
    bool isScreenPowerOff = false;

    Preferences prefs;
    static LRReceiver* instance;

    // --- 设置管理 ---
    void loadSettings() {
        prefs.begin("hank-os", false);
        setContrast = prefs.getUChar("contrast", 128);
        setScreenOff = prefs.getUChar("scroff", 1);
        prefs.end();
        u8g2.setContrast(setContrast);
    }

    void saveSettings() {
        prefs.begin("hank-os", false);
        prefs.putUChar("contrast", setContrast);
        prefs.putUChar("scroff", setScreenOff);
        prefs.end();
    }

    void wakeUp() {
        lastActivity = millis();
        if (isScreenPowerOff) { u8g2.setPowerSave(0); isScreenPowerOff = false; }
    }

    // --- HP 居中开机动画 ---
    void bootAnimation() {
        u8g2.begin(); u8g2.setContrast(setContrast); u8g2.setFontPosTop();
        u8g2.setFont(u8g2_font_6x10_tf);
        int strW = u8g2.getStrWidth("HANK-OS");
        int textX = (128 - strW) / 2;
        float angle = 0; uint32_t start = millis();
        while (millis() - start < 1800) {
            u8g2.clearBuffer();
            for (int i = 0; i < 6; i++) {
                float p_angle = angle - (i * 0.4f);
                int x = 64 + cos(p_angle) * 12; int y = 28 + sin(p_angle) * 12;
                u8g2.drawDisc(x, y, (i == 0) ? 2 : 1); 
            }
            u8g2.drawStr(textX, 48, "HANK-OS");
            u8g2.sendBuffer();
            angle += 0.22f; delay(10);
        }
        for (int i = 0; i < 128; i += 16) {
            u8g2.clearBuffer(); u8g2.drawBox(i, 0, 16, 64); u8g2.sendBuffer();
        }
    }

    // --- 动态缩放图表 ---
    void drawDynamicChart(float* data, int count, const char* label, const char* unit, int x, int y, int w, int h) {
        u8g2.drawFrame(x, y, w, h);
        u8g2.setFont(u8g2_font_4x6_tf);
        u8g2.setCursor(x + 2, y + 2); u8g2.printf("%s(%s)", label, unit);
        
        if (count < 2) { u8g2.drawStr(x + 35, y + h/2, "WAITING..."); return; }

        float minV = data[0], maxV = data[0];
        for (int i = 0; i < count; i++) {
            if (data[i] < minV) minV = data[i];
            if (data[i] > maxV) maxV = data[i];
        }
        if (maxV == minV) { maxV += 0.5f; minV -= 0.5f; }

        float xStep = (float)(w - 4) / (count - 1);
        for (int i = 0; i < count - 1; i++) {
            int x1 = x + 2 + (int)(i * xStep);
            int x2 = x + 2 + (int)((i + 1) * xStep);
            int y1 = (y + h - 3) - (int)((data[i] - minV) / (maxV - minV) * (h - 6));
            int y2 = (y + h - 3) - (int)((data[i+1] - minV) / (maxV - minV) * (h - 6));
            if (x1 > -128 && x1 < 128) u8g2.drawLine(x1, y1, x2, y2);
        }
        u8g2.setCursor(x + w - 32, y + 2); u8g2.printf("H:%.1f", maxV);
        u8g2.setCursor(x + w - 32, y + h - 7); u8g2.printf("L:%.1f", minV);
    }

    // --- 设置页面 UI ---
    void drawSettingsPage() {
        u8g2.setFont(u8g2_font_6x10_tf);
        int titleW = u8g2.getStrWidth("SYSTEM SETUP");
        u8g2.drawStr((128 - titleW)/2, 2, "SYSTEM SETUP");
        u8g2.drawHLine(0, 13, 128);
        
        const char* menu[] = {"Contrast", "Screen Off", "Save & Exit"};
        for (int i = 0; i < 3; i++) {
            int y = 18 + i * 14;
            if (setSelectIndex == i) {
                u8g2.setDrawColor(1);
                u8g2.drawBox(0, y - 1, 128, 11);
            }
            u8g2.setDrawColor(setSelectIndex == i ? 0 : 1);
            u8g2.setCursor(5, y); u8g2.print(menu[i]);
            
            if (i == 0) {
                u8g2.drawFrame(70, y + 2, 50, 5);
                u8g2.drawBox(70, y + 2, map(setContrast, 0, 255, 0, 50), 5);
            } else if (i == 1) {
                const char* offOpts[] = {"Never", "30s", "1m"};
                u8g2.setCursor(85, y); u8g2.print(offOpts[setScreenOff]);
            }
            u8g2.setDrawColor(1);
        }
    }

    // --- 节点页面 UI ---
    void drawNodePage(int id, int x) {
        if (!nodeActive[id]) {
            u8g2.drawStr(x + 35, 30, "OFFLINE"); u8g2.setCursor(x + 60, 42); u8g2.printf("#%d", id);
            return;
        }
        // 信号柱
        int bars = map(constrain(nodeRssi[id], -100, -30), -100, -30, 1, 5);
        for(int i=0; i<5; i++) {
            if(i < bars) u8g2.drawBox(x + 108 + (i*4), 8 - (i*2), 3, i*2 + 2);
            else u8g2.drawFrame(x + 108 + (i*4), 8 - (i*2), 3, i*2 + 2);
        }
        // 三重波纹
        if (id == currentPage && !isDetailMode) {
            if (ping1 > 0) u8g2.drawCircle(x+10, 10, ping1);
            if (ping2 > 0) u8g2.drawCircle(x+10, 10, ping2);
            if (ping3 > 0) u8g2.drawCircle(x+10, 10, ping3);
        }
        u8g2.setCursor(x + 2, 2); u8g2.printf("id:%02d rssi:%d", id, nodeRssi[id]);
        u8g2.drawHLine(x, 12, 128); 
        u8g2.setCursor(x + 2, 16); u8g2.printf("sht:%.2fC|%.2f%%rh", nodeData[id].t1, nodeData[id].h1);
        u8g2.setCursor(x + 2, 28); u8g2.printf("bmp:%.2fC|%.2fhPa", nodeData[id].t2, nodeData[id].p2);
        u8g2.setCursor(x + 2, 40);
        if (id == 1) u8g2.printf("st:ok g:%.1fs", nodeGap[id]); // 1号无电量
        else {
            if (nodeData[id].status == 255) u8g2.print("st:!lowpwr alert!");
            else u8g2.printf("st:ok b:%d%% g:%.1fs", nodeData[id].status, nodeGap[id]);
        }
        // 监控信息
        u8g2.setFont(u8g2_font_4x6_tf); u8g2.setCursor(x + 2, 52); u8g2.printf("m:%dk l:%.0f%% 66fps", freeMem/1024, cpuLoad); u8g2.setFont(u8g2_font_6x10_tf);
        for(int i=1; i<=4; i++) {
            if(i == id) u8g2.drawDisc(x + 110 + (i*4), 58, 1);
            else u8g2.drawCircle(x + 110 + (i*4), 58, 1);
        }
    }

    void onDataReceived(const esp_now_recv_info *info, const uint8_t *raw_data, int len) {
        if (len != sizeof(struct_message)) return;
        struct_message temp; memcpy(&temp, raw_data, sizeof(struct_message));
        if (crc32_le(0, (uint8_t const *)&temp, sizeof(struct_message) - 4) != temp.crc) return;

        uint8_t id = temp.sen_num;
        if (id >= 1 && id <= 4) {
            unsigned long now = millis();
            if (lastSeen[id] > 0) nodeGap[id] = (now - lastSeen[id]) / 1000.0f;
            lastSeen[id] = now;
            nodeData[id] = temp; nodeRssi[id] = info->rx_ctrl->rssi; nodeActive[id] = true;

            if (now - lastLogTime[id] >= 60000 || lastLogTime[id] == 0) {
                if (hCount[id] < MAX_HIST) {
                    hT[id][hCount[id]] = temp.t1; hH[id][hCount[id]] = temp.h1; hP[id][hCount[id]] = temp.p2; hCount[id]++;
                } else {
                    for (int j = 0; j < MAX_HIST - 1; j++) { hT[id][j] = hT[id][j+1]; hH[id][j] = hH[id][j+1]; hP[id][j] = hP[id][j+1]; }
                    hT[id][MAX_HIST-1] = temp.t1; hH[id][MAX_HIST-1] = temp.h1; hP[id][MAX_HIST-1] = temp.p2;
                }
                lastLogTime[id] = now;
            }
            if (id == currentPage && !isDetailMode && !isSettingsMode) {
                this->ping1 = 1; this->ping2 = -12; this->ping3 = -24;
                this->lastPacketTick = xTaskGetTickCount();
            }
        }
    }
};

LRReceiver receiver;
LRReceiver* LRReceiver::instance = &receiver;

// ================= 3. 任务实现 =================

void TaskKeys(void *pvParameters) {
    pinMode(BTN_LEFT, INPUT_PULLUP); pinMode(BTN_RIGHT, INPUT_PULLUP);
    unsigned long pressStart = 0; bool isChord = false;

    for (;;) {
        bool L = (digitalRead(BTN_LEFT) == LOW); bool R = (digitalRead(BTN_RIGHT) == LOW);
        if (L || R) receiver.wakeUp();

        if (L && R) {
            if (pressStart == 0) pressStart = millis();
            if (millis() - pressStart > 1000) { // 持续 1s 进入/退出设置
                receiver.isSettingsMode = !receiver.isSettingsMode;
                receiver.isDetailMode = false;
                receiver.setSelectIndex = 0;
                pressStart = 0; isChord = false; vTaskDelay(pdMS_TO_TICKS(500));
            }
            isChord = true;
        } 
        else if (!L && !R && isChord) { // 短合击释放 -> 切换图表
            if (pressStart > 0 && millis() - pressStart < 800) {
                if (!receiver.isSettingsMode) receiver.isDetailMode = !receiver.isDetailMode;
            }
            isChord = false; pressStart = 0;
        } 
        else if ((L || R) && !isChord) { // 单键逻辑 (100ms 判定)
            vTaskDelay(pdMS_TO_TICKS(100)); 
            if (digitalRead(BTN_LEFT) == LOW && digitalRead(BTN_RIGHT) == LOW) continue;

            if (receiver.isSettingsMode) {
                // --- 设置页逻辑：左选行，右改值 ---
                if (L) receiver.setSelectIndex = (receiver.setSelectIndex + 1) % 3;
                if (R) {
                    if (receiver.setSelectIndex == 0) {
                        receiver.setContrast = (receiver.setContrast >= 255) ? 0 : receiver.setContrast + 51;
                        u8g2.setContrast(receiver.setContrast);
                    } else if (receiver.setSelectIndex == 1) {
                        receiver.setScreenOff = (receiver.setScreenOff + 1) % 3;
                    } else if (receiver.setSelectIndex == 2) {
                        receiver.saveSettings(); receiver.isSettingsMode = false;
                    }
                }
            } else if (receiver.isDetailMode) {
                if (L) receiver.detailSub = (receiver.detailSub == 0) ? 2 : receiver.detailSub - 1;
                else receiver.detailSub = (receiver.detailSub + 1) % 3;
            } else {
                if (L) receiver.currentPage = (receiver.currentPage > 1) ? receiver.currentPage - 1 : 4;
                else receiver.currentPage = (receiver.currentPage < 4) ? receiver.currentPage + 1 : 1;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void TaskDisp(void *pvParameters) {
    uint32_t tS;
    for (;;) {
        uint32_t offTable[] = {0, 30000, 60000};
        if (receiver.setScreenOff > 0 && millis() - receiver.lastActivity > offTable[receiver.setScreenOff]) {
            if (!receiver.isScreenPowerOff) { u8g2.setPowerSave(1); receiver.isScreenPowerOff = true; }
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        tS = micros(); u8g2.clearBuffer(); receiver.heartbeat++;
        
        if (receiver.isSettingsMode) {
            receiver.drawSettingsPage();
        } else if (receiver.isDetailMode) {
            receiver.subTargetX = receiver.detailSub * 128.0f;
            receiver.subScrollX = (receiver.subScrollX * 0.82f) + (receiver.subTargetX * 0.18f);
            u8g2.drawRFrame(0, 0, 128, 64, 3);
            for(int s=0; s<3; s++) {
                int sx = (s * 128) - (int)receiver.subScrollX;
                if (sx > -128 && sx < 128) {
                    if (s == 0) receiver.drawDynamicChart(receiver.hT[receiver.currentPage], receiver.hCount[receiver.currentPage], "TEMP", "C", sx+4, 10, 120, 48);
                    else if (s == 1) receiver.drawDynamicChart(receiver.hH[receiver.currentPage], receiver.hCount[receiver.currentPage], "HUMID", "%", sx+4, 10, 120, 48);
                    else if (s == 2) receiver.drawDynamicChart(receiver.hP[receiver.currentPage], receiver.hCount[receiver.currentPage], "PRES", "hPa", sx+4, 10, 120, 48);
                }
            }
        } else {
            receiver.targetX = receiver.currentPage * 128.0f;
            receiver.scrollX = (receiver.scrollX * 0.82f) + (receiver.targetX * 0.18f);
            for (int i = 1; i <= 4; i++) {
                int x_off = (i * 128) - (int)receiver.scrollX;
                if (x_off > -128 && x_off < 128) receiver.drawNodePage(i, x_off);
            }
            uint32_t elap = xTaskGetTickCount() - receiver.lastPacketTick;
            float targetW = (1.0f - (constrain((float)elap, 0.0f, 35000.0f) / 35000.0f)) * 128.0f;
            receiver.smoothWidth = (receiver.smoothWidth * 0.85f) + (targetW * 0.15f);
            u8g2.drawBox(0, 62, (uint8_t)receiver.smoothWidth, 2);
        }

        // 动画步进
        if (receiver.ping1 > 0) { receiver.ping1 += 5; if (receiver.ping1 > 135) receiver.ping1 = 0; }
        if (receiver.ping2 > 0) { receiver.ping2 += 4; if (receiver.ping2 > 105) receiver.ping2 = 0; } 
        else if (receiver.ping2 < 0) { receiver.ping2++; if (receiver.ping2 == 0) receiver.ping2 = 1; }
        if (receiver.ping3 > 0) { receiver.ping3 += 3; if (receiver.ping3 > 85) receiver.ping3 = 0; }
        else if (receiver.ping3 < 0) { receiver.ping3++; if (receiver.ping3 == 0) receiver.ping3 = 1; }

        if ((receiver.heartbeat % 20) < 10) u8g2.drawPixel(127, 0);
        u8g2.sendBuffer();
        receiver.freeMem = esp_get_free_heap_size();
        receiver.cpuLoad = ((micros() - tS) * 100.0f) / 15151.0f; 
        vTaskDelay(pdMS_TO_TICKS(15)); 
    }
}

// ================= 4. 主程序 =================
void setup() {
    u8g2.begin(); Wire.setClock(800000); 
    receiver.loadSettings();
    receiver.bootAnimation();
    WiFi.mode(WIFI_STA); esp_wifi_set_mac(WIFI_IF_STA, NEW_MAC); esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    esp_now_init();
    esp_now_register_recv_cb([](const esp_now_recv_info *info, const uint8_t *data, int len) {
        LRReceiver::instance->onDataReceived(info, data, len);
    });
    receiver.lastActivity = millis();
    xTaskCreate(TaskDisp, "Disp", 4096, NULL, 1, NULL);
    xTaskCreate(TaskKeys, "Keys", 2048, NULL, 1, NULL);
}

void loop() { vTaskDelete(NULL); }