/*
 * 项目：HANK-OS Ultimate Terminal (V9.8)
 * 授权：GNU GPL v3.0 | 作者：hankli22
 * 状态：逻辑完全展开版，解决字体继s承、UI错位、WiFi/LR冲突
 */

#include <esp_now.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <rom/crc.h> 
#include <esp_wifi.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secret.cpp"


// ============================================================
// 1. 全局定义与宏 (方便用户微调坐标)
// ============================================================

#define LINE1_Y 10
#define LINE2_Y 23
#define LINE3_Y 34
#define LINE4_Y 45
#define DEBUG_Y 57
#define MAX_HIST 30 

const int BTN_RIGHT = 18; 
const int BTN_LEFT  = 19; 

typedef struct {
    float t1, h1, t2, p2;
    uint8_t sen_typ, sen_num, status;
    uint32_t crc;      
} struct_message;

uint8_t NEW_MAC[] = {0x54, 0x32, 0x04, 0x46, 0x37, 0x8C};
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 7, 6);

// ============================================================
// 2. 核心逻辑类 LRReceiver
// ============================================================

class LRReceiver {
public:
    struct_message nodeData[5]; 
    int8_t nodeRssi[5];
    unsigned long lastSeen[5];
    bool nodeActive[5];
    float hT[5][MAX_HIST], hH[5][MAX_HIST], hP[5][MAX_HIST];
    int hCount[5];
    unsigned long lastLogTime[5];

    bool hypOnline;
    String hypGame, hypMode;
    int bwFinalKills, bwWins;

    int8_t currentPage;      
    bool isDetailMode;   
    bool isSettingsMode; 
    int8_t detailSub;        
    float scrollX, targetX; 
    float subScrollX, subTargetX;

    int16_t ping1, ping2, ping3;
    uint32_t lastPacketTick, lastApiTick;
    float cpuLoad;
    uint32_t freeMem;
    uint8_t heartbeat;
    float smoothWidth;

    uint8_t setContrast;
    uint8_t setScreenOff;    
    uint8_t commMode; 
    int8_t setSelectIndex;   
    unsigned long lastActivity;
    bool isScreenPowerOff;

    int16_t hypModeScrollX = 0; // 新增：Hypixel 模式的滚动 X 轴偏移

    Preferences prefs;
    static LRReceiver* instance;

    LRReceiver() {
        currentPage = 1;
        isDetailMode = false;
        isSettingsMode = false;
        detailSub = 0;
        scrollX = 128.0f;
        targetX = 128.0f;
        lastPacketTick = 0;
        lastApiTick = 0;
        for (int i = 0; i < 5; i++) {
            nodeActive[i] = false;
            lastSeen[i] = 0;
            hCount[i] = 0;
        }
    }

    void wakeUp() {
        lastActivity = millis();
        if (isScreenPowerOff) {
            u8g2.setPowerSave(0);
            isScreenPowerOff = false;
        }
    }

    void loadSettings() {
        prefs.begin("hank-os", false);
        setContrast = prefs.getUChar("contrast", 128);
        setScreenOff = prefs.getUChar("scroff", 0);
        commMode = prefs.getUChar("comm", 0);
        prefs.end();
        u8g2.setContrast(setContrast);
    }

    void saveSettings() {
        prefs.begin("hank-os", false);
        prefs.putUChar("contrast", setContrast);
        prefs.putUChar("scroff", setScreenOff);
        prefs.putUChar("comm", commMode);
        prefs.end();
    }

    void bootAnimation() {
        u8g2.begin();
        float angle = 0;
        uint32_t start = millis();
        while (millis() - start < 1500) {
            u8g2.clearBuffer();
            for (int i = 0; i < 6; i++) {
                float p_angle = angle - (i * 0.4f);
                int x = 64 + cos(p_angle) * 12;
                int y = 28 + sin(p_angle) * 12;
                u8g2.drawDisc(x, y, (i == 0) ? 2 : 1); // 审美回归
            }
            u8g2.setFont(u8g2_font_6x10_tf);
            u8g2.drawStr(45, 48, "HANK-OS");
            u8g2.sendBuffer();
            angle += 0.22f;
            delay(10);
        }
    }
    
    // 修正后的 fetchHypixel：增加串口反馈和成功判定
    void fetchHypixel() {
        if (commMode == 0 || WiFi.status() != WL_CONNECTED) {
            return;
        }

        WiFiClientSecure client;
        client.setInsecure(); 
        HTTPClient http;

        String url = "https://api.hypixel.net/v2/status?key=";
        url.concat(YOUR_HYPIXEL_APIKEY);
        url.concat("&uuid=");
        url.concat(YOUR_MINECRAFT_UUID);

        
        if (http.begin(client, url)) {
            int httpCode = http.GET();
            if (httpCode == HTTP_CODE_OK) {
                JsonDocument doc;
                deserializeJson(doc, http.getString());
                hypOnline = doc["session"]["online"] | false;
                hypGame = doc["session"]["gameType"].as<String>();
                hypMode = doc["session"]["mode"].as<String>();
                
                lastApiTick = xTaskGetTickCount(); // 更新刷新时间戳
                ping1 = 1; // 成功后触发一次涟漪
            }
            http.end();
        }
    }

    void onDataReceived(const esp_now_recv_info *info, const uint8_t *raw_data, int len) {
        if (len != sizeof(struct_message)) {
            return;
        }
        struct_message temp;
        memcpy(&temp, raw_data, sizeof(struct_message));
        if (crc32_le(0, (uint8_t const *)&temp, sizeof(struct_message) - 4) != temp.crc) {
            return;
        }

        uint8_t id = temp.sen_num;
        if (id >= 1 && id <= 4) {
            unsigned long now = millis();
            lastSeen[id] = now;
            nodeData[id] = temp;
            nodeRssi[id] = info->rx_ctrl->rssi;
            nodeActive[id] = true;

            if (now - lastLogTime[id] >= 60000 || lastLogTime[id] == 0) {
                for (int j = 0; j < MAX_HIST - 1; j++) {
                    hT[id][j] = hT[id][j+1]; 
                    hH[id][j] = hH[id][j+1]; 
                    hP[id][j] = hP[id][j+1];
                }
                hT[id][MAX_HIST-1] = temp.t1;
                hH[id][MAX_HIST-1] = temp.h1;
                hP[id][MAX_HIST-1] = temp.p2;
                hCount[id] = min(MAX_HIST, hCount[id] + 1);
                lastLogTime[id] = now;
            }
            if (id == currentPage && !isDetailMode) {
                ping1 = 1; ping2 = -12; ping3 = -24;
                lastPacketTick = xTaskGetTickCount();
            }
        }
    }

    static void onDataReceivedStatic(const esp_now_recv_info *info, const uint8_t *data, int len) {
        instance->onDataReceived(info, data, len);
    }

    // --- 页面绘图子模块 ---
    // --- 方法: 绘制 Hypixel 页面 (带 Mode 滚动) ---
    void drawHypixelPage(int x) {
        // 1. 设置主字体
        u8g2.setFont(u8g2_font_6x10_tf);

        // 2. 绘制静态 Header
        u8g2.setCursor(x + 2, LINE1_Y);
        u8g2.print("HYPIXEL STATUS");
        u8g2.drawHLine(x, LINE1_Y + 1, 128);

        // 3. 绘制静态数据
        u8g2.setCursor(x + 2, LINE2_Y);
        u8g2.printf("Online:%s", hypOnline ? "YES" : "NO");
        
        u8g2.setCursor(x + 2, LINE3_Y);
        u8g2.printf("Game:%s", hypGame.c_str());

        // 4. 动态绘制 Mode (带滚动逻辑)
        u8g2.setCursor(x + 2, LINE4_Y);
        u8g2.print("Mode:");
        
        // 计算 "Mode:" 标签后的可用空间
        const int availableWidth = 128 - u8g2.getCursorX() - 2; 
        int modeWidth = u8g2.getStrWidth(hypMode.c_str());

        if (modeWidth > availableWidth) {
            // 如果 Mode 太长，启动跑马灯
            u8g2.setCursor(x + 40 + hypModeScrollX, LINE4_Y+10);
            u8g2.print(hypMode.c_str());
            
            // 动画步进：向左滚动
            hypModeScrollX--;
            
            // 当文字完全滚出左侧屏幕时，让它从右侧重新进入
            if (hypModeScrollX < -modeWidth) {
                hypModeScrollX = availableWidth;
            }
        } else {
            // 如果能放下，则静态显示
            u8g2.setCursor(x + 40, LINE4_Y);
            u8g2.print(hypMode.c_str());
            hypModeScrollX = 0; // 重置滚动
        }
    }

    void drawNodePage(int id, int x) {
        if (!nodeActive[id]) {
            u8g2.drawStr(x + 35, 30, "OFFLINE");
            u8g2.setCursor(x + 60, 42);
            u8g2.printf("#%d", id);
            return;
        }
        // 信号强度
        int bars = map(constrain(nodeRssi[id], -100, -30), -100, -30, 1, 5);
        for (int i = 0; i < 5; i++) {
            if (i < bars) {
                u8g2.drawBox(x + 108 + (i * 4), 8 - (i * 2), 3, i * 2 + 2);
            } else {
                u8g2.drawFrame(x + 108 + (i * 4), 8 - (i * 2), 3, i * 2 + 2);
            }
        }
        // 涟漪动画
        if (id == currentPage && !isDetailMode) {
            if (ping1 > 0) { u8g2.drawCircle(x+10, 10, ping1); }
            if (ping2 > 0) { u8g2.drawCircle(x+10, 10, ping2); }
            if (ping3 > 0) { u8g2.drawCircle(x+10, 10, ping3); }
        }
        u8g2.setCursor(x + 2, LINE1_Y);
        u8g2.printf("id:%02d rssi:%d", id, nodeRssi[id]);
        u8g2.drawHLine(x, LINE1_Y + 1, 128); 
        u8g2.setCursor(x + 2, LINE2_Y);
        u8g2.printf("sht:%.2fC|%.2f%%rh", nodeData[id].t1, nodeData[id].h1);
        u8g2.setCursor(x + 2, LINE3_Y);
        u8g2.printf("bmp:%.2fC|%.2fhPa", nodeData[id].t2, nodeData[id].p2);
        u8g2.setCursor(x + 2, LINE4_Y);
        if (id == 1) {
            u8g2.printf("st:ok g:%.1fs", (millis() - lastSeen[1]) / 1000.0f);
        } else {
            u8g2.printf("st:ok|b:%d%%|g:%.1fs", nodeData[id].status, (millis() - lastSeen[id]) / 1000.0f);
        }
        u8g2.setFont(u8g2_font_4x6_tf);
        u8g2.setCursor(x + 2, DEBUG_Y);
        u8g2.printf("m:%uk l:%.0f%% 66fps", (unsigned int)(freeMem / 1024), cpuLoad);
        u8g2.setFont(u8g2_font_6x10_tf);
    }

    void drawSettingsPage(int x) {
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.setCursor(x + 32, LINE1_Y);
        u8g2.print("SYSTEM SETUP");
        u8g2.drawHLine(x, LINE1_Y + 1, 128);
        const char* menu[] = {"Contrast", "Screen Off", "Comm Mode", "Save & Exit"};
        for (int i = 0; i < 4; i++) {
            int y = 22 + i * 11;
            if (setSelectIndex == i) {
                u8g2.setDrawColor(1);
                u8g2.drawBox(x, y - 9, 128, 11); // 修复后的高亮框
            }
            u8g2.setDrawColor(setSelectIndex == i ? 0 : 1);
            u8g2.setCursor(x + 5, y);
            u8g2.print(menu[i]);
            if (i == 0) {
                u8g2.drawFrame(x + 75, y - 6, 40, 4);
                u8g2.drawBox(x + 75, y - 6, map(setContrast, 0, 255, 0, 40), 4);
            } else if (i == 1) {
                const char* os[] = {"OFF", "30s", "1m"};
                u8g2.setCursor(x + 85, y);
                u8g2.print(os[setScreenOff]);
            } else if (i == 2) {
                u8g2.setCursor(x + 85, y);
                u8g2.print(commMode == 0 ? "LR" : "WiFi");
            }
            u8g2.setDrawColor(1);
        }
    }

    void drawChart(float* data, int count, const char* label, const char* unit, int x, int y, int w, int h) {
        u8g2.drawFrame(x, y, w, h);
        u8g2.setFont(u8g2_font_4x6_tf);
        u8g2.setCursor(x + 2, y + 2);
        u8g2.printf("%s(%s)", label, unit);
        if (count < 2) {
            return;
        }
        float minV = data[MAX_HIST - count], maxV = data[MAX_HIST - count];
        for (int i = MAX_HIST - count; i < MAX_HIST; i++) {
            if (data[i] < minV) { minV = data[i]; }
            if (data[i] > maxV) { maxV = data[i]; }
        }
        if (maxV == minV) {
            maxV += 0.5f;
            minV -= 0.5f;
        }
        float xStep = (float)(w - 4) / (count - 1);
        for (int i = 0; i < count - 1; i++) {
            int x1 = x + 2 + (int)(i * xStep);
            int x2 = x + 2 + (int)((i + 1) * xStep);
            if (x2 < 0 || x1 > 128) {
                continue;
            }
            int y1 = (y + h - 3) - (int)((data[MAX_HIST - count + i] - minV) / (maxV - minV) * (h - 6));
            int y2 = (y + h - 3) - (int)((data[MAX_HIST - count + i + 1] - minV) / (maxV - minV) * (h - 6));
            u8g2.drawLine(x1, y1, x2, y2);
        }
    }
};

LRReceiver receiver;
LRReceiver* LRReceiver::instance = &receiver;

// ============================================================
// 3. FreeRTOS 任务
// ============================================================

void TaskKeys(void *pvParameters) {
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);
    unsigned long pressStart = 0;
    bool isChord = false;

    for (;;) {
        bool L = (digitalRead(BTN_LEFT) == LOW);
        bool R = (digitalRead(BTN_RIGHT) == LOW);

        if (L || R) {
            receiver.wakeUp();
        }

        // --- 组合键判定 ---
        if (L && R) {
            if (pressStart == 0) {
                pressStart = millis();
            }
            if (millis() - pressStart > 1000) {
                receiver.isSettingsMode = !receiver.isSettingsMode;
                receiver.isDetailMode = false;
                receiver.setSelectIndex = 0;
                pressStart = 0;
                isChord = false;
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            isChord = true;
        } 
        else if (!L && !R && isChord) {
            if (millis() - pressStart < 800) {
                if (receiver.isSettingsMode == false) {
                    receiver.isDetailMode = !receiver.isDetailMode;
                }
            }
            isChord = false;
            pressStart = 0;
        } 
        // --- 单键导航裁剪逻辑 ---
        else if ((L || R) && !isChord) {
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms 宽容窗
            if (digitalRead(BTN_LEFT) == LOW && digitalRead(BTN_RIGHT) == LOW) {
                continue;
            }

            if (receiver.isSettingsMode) {
                // 设置页面内的操作逻辑 (保持你之前的成功版本)
                if (L) {
                    receiver.setSelectIndex = (receiver.setSelectIndex + 3) % 4;
                }
                if (R) {
                    if (receiver.setSelectIndex == 0) {
                        receiver.setContrast += 51;
                        u8g2.setContrast(receiver.setContrast);
                    } else if (receiver.setSelectIndex == 1) {
                        receiver.setScreenOff = (receiver.setScreenOff + 1) % 3;
                    } else if (receiver.setSelectIndex == 2) {
                        receiver.commMode = 1 - receiver.commMode;
                    } else if (receiver.setSelectIndex == 3) {
                        receiver.saveSettings();
                        ESP.restart();
                    }
                }
            } 
            else if (receiver.isDetailMode) {
                // 详情模式下的左右切换
                if (R) {
                    receiver.detailSub = (receiver.detailSub + 1) % 3;
                } else {
                    receiver.detailSub = (receiver.detailSub == 0) ? 2 : receiver.detailSub - 1;
                }
            } 
            else {
                // --- 重点：根据通信模式进行页面裁剪导航 ---
                if (receiver.commMode == 1) { 
                    // WiFi 模式：只允许在 0 和 5 之间跳转
                    if (receiver.currentPage == 0) {
                        receiver.currentPage = 5;
                    } else {
                        receiver.currentPage = 0;
                        receiver.fetchHypixel(); // 回到首页触发刷新
                    }
                } 
                else { 
                    // LR 模式：只允许在 1, 2, 3, 4, 5 之间跳转，跳过 0
                    if (R) {
                        // 1->2->3->4->5->1
                        receiver.currentPage = (receiver.currentPage % 5) + 1;
                    } else {
                        // 1->5->4->3->2->1
                        if (receiver.currentPage <= 1) {
                            receiver.currentPage = 5;
                        } else {
                            receiver.currentPage--;
                        }
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(250)); // 全局去抖
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void TaskDisp(void *pvParameters) {
    uint32_t frameStartTime;
    uint32_t frameWorkTime;

    for (;;) {
        // --- 1. 自动息屏管理逻辑 ---
        uint32_t offTimeTable[] = {0, 30000, 60000}; 
        if (receiver.setScreenOff > 0) {
            uint32_t inactiveMs = millis() - receiver.lastActivity;
            if (inactiveMs > offTimeTable[receiver.setScreenOff]) {
                if (receiver.isScreenPowerOff == false) {
                    u8g2.setPowerSave(1); // 物理关闭屏幕显示
                    receiver.isScreenPowerOff = true;
                }
                vTaskDelay(pdMS_TO_TICKS(100)); // 息屏状态下降低检测频率
                continue; 
            }
        }

        // --- 2. WiFi 模式下的 Hypixel 自动刷新定时器 ---
        if (receiver.commMode == 1) { // 仅在 WiFi 模式
            if (receiver.currentPage == 0) { // 仅在首页
                if (receiver.isSettingsMode == false) { // 且不在设置界面
                    uint32_t nowTick = xTaskGetTickCount();
                    uint32_t elapsedMs = (nowTick - receiver.lastApiTick) * portTICK_PERIOD_MS;
                    
                    // 每 30 秒自动抓取一次数据
                    if (elapsedMs > 30000 || receiver.lastApiTick == 0) {
                        receiver.fetchHypixel();
                    }
                }
            }
        }

        // --- 3. 准备渲染新的一帧 ---
        frameStartTime = micros();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.setDrawColor(1);
        receiver.heartbeat++;

        // 3.1 计算主页面滑动偏移 (0.82/0.18 一阶滤波)
        receiver.targetX = receiver.currentPage * 128.0f;
        receiver.scrollX = (receiver.scrollX * 0.82f) + (receiver.targetX * 0.18f);

        // 3.2 计算详情子页面滑动偏移 (温/湿/压 切换动画)
        receiver.subTargetX = receiver.detailSub * 128.0f;
        receiver.subScrollX = (receiver.subScrollX * 0.82f) + (receiver.subTargetX * 0.18f);

        // --- 4. 页面分流渲染引擎 ---
        
        if (receiver.isSettingsMode == true) {
            // A 分支: 浮窗式设置界面 (无视背景滚动)
            receiver.drawSettingsPage(0);
        } 
        else if (receiver.isDetailMode == true) {
            // B 分支: 详情图表/战绩展示模式
            u8g2.drawRFrame(0, 0, 128, 64, 3); // 绘制模式外框
            
            if (receiver.currentPage == 0) {
                // 情况 1: Hypixel 战绩详情页
                receiver.drawHypixelPage(0); // 复用逻辑
            } 
            else {
                // 情况 2: 传感器温/湿/压滑动折线图
                for (int s = 0; s < 3; s++) {
                    int sx = (s * 128) - (int)receiver.subScrollX;
                    // 仅绘制在 128 像素视口内的图表
                    if (sx > -128 && sx < 128) {
                        if (s == 0) {
                            receiver.drawChart(receiver.hT[receiver.currentPage], receiver.hCount[receiver.currentPage], "TEMP", "C", sx + 4, 10, 120, 48);
                        } 
                        else if (s == 1) {
                            receiver.drawChart(receiver.hH[receiver.currentPage], receiver.hCount[receiver.currentPage], "HUMID", "%", sx + 4, 10, 120, 48);
                        } 
                        else if (s == 2) {
                            receiver.drawChart(receiver.hP[receiver.currentPage], receiver.hCount[receiver.currentPage], "PRES", "hPa", sx + 4, 10, 120, 48);
                        }
                    }
                }
            }
        } 
        else {
            // C 分支: 正常滑动模式 (实施 Culling 裁剪)
            
            if (receiver.commMode == 1) {
                // --- WiFi 模式裁剪：仅渲染 Page 0 和 Page 5 ---
                int pagesWiFi[] = {0, 5};
                for (int j = 0; j < 2; j++) {
                    int pIdx = pagesWiFi[j];
                    int xo = (pIdx * 128) - (int)receiver.scrollX;
                    if (xo > -128 && xo < 128) {
                        if (pIdx == 0) {
                            receiver.drawHypixelPage(xo);
                        } else {
                            receiver.drawSettingsPage(xo);
                        }
                    }
                }
            } 
            else {
                // --- LR 模式裁剪：渲染 Page 1 到 Page 5 ---
                for (int i = 1; i <= 5; i++) {
                    int xo = (i * 128) - (int)receiver.scrollX;
                    if (xo > -128 && xo < 128) {
                        if (i == 5) {
                            receiver.drawSettingsPage(xo);
                        } else {
                            receiver.drawNodePage(i, xo);
                        }
                    }
                }
            }

            // 绘制底部新鲜度进度条
            uint32_t refTick = (receiver.currentPage == 0) ? receiver.lastApiTick : receiver.lastPacketTick;
            uint32_t deltaTick = xTaskGetTickCount() - refTick;
            // 倒计时映射 (60秒刷新周期)
            float barTargetW = (1.0f - (constrain((float)deltaTick * portTICK_PERIOD_MS, 0.0f, 60000.0f) / 60000.0f)) * 128.0f;
            receiver.smoothWidth = (receiver.smoothWidth * 0.85f) + (barTargetW * 0.15f);
            u8g2.drawBox(0, 62, (uint8_t)receiver.smoothWidth, 2);
        }

        // --- 5. 核心动画：三重巨浪 (速率 5-4-3) ---
        
        // 第一道波
        if (receiver.ping1 > 0) { 
            u8g2.drawCircle(10, 10, receiver.ping1);
            receiver.ping1 += 5; 
            if (receiver.ping1 > 135) {
                receiver.ping1 = 0; 
            }
        }

        // 第二道波
        if (receiver.ping2 > 0) { 
            u8g2.drawCircle(10, 10, receiver.ping2);
            receiver.ping2 += 4; 
            if (receiver.ping2 > 105) {
                receiver.ping2 = 0; 
            }
        } 
        else if (receiver.ping2 < 0) { 
            receiver.ping2++; // 独立倒计时
            if (receiver.ping2 == 0) {
                receiver.ping2 = 1;
            }
        }

        // 第三道波
        if (receiver.ping3 > 0) { 
            u8g2.drawCircle(10, 10, receiver.ping3);
            receiver.ping3 += 3; 
            if (receiver.ping3 > 85) {
                receiver.ping3 = 0; 
            }
        } 
        else if (receiver.ping3 < 0) { 
            receiver.ping3++; // 独立倒计时
            if (receiver.ping3 == 0) {
                receiver.ping3 = 1;
            }
        }

        // --- 6. 完成绘图并进行系统统计 ---
        
        // 右上角心跳点
        if ((receiver.heartbeat % 20) < 10) {
            u8g2.drawPixel(127, 0);
        }

        u8g2.sendBuffer();

        // 性能指标计算
        frameWorkTime = micros() - frameStartTime;
        receiver.freeMem = esp_get_free_heap_size();
        // 目标周期为 15.15ms (66FPS)
        receiver.cpuLoad = (frameWorkTime * 100.0f) / 15151.0f; 

        // 维持 66 FPS
        vTaskDelay(pdMS_TO_TICKS(15)); 
    }
}



void setup() {
    u8g2.begin(); 
    Wire.setClock(800000); 
    receiver.loadSettings();
    receiver.bootAnimation();
    if (receiver.commMode == 0) {
        WiFi.mode(WIFI_STA); 
        esp_wifi_set_mac(WIFI_IF_STA, NEW_MAC);
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
        esp_now_init(); 
        esp_now_register_recv_cb(receiver.onDataReceivedStatic);
        receiver.currentPage = 1;
    } else {
        WiFi.mode(WIFI_STA); 
        WiFi.begin(YOUR_WIFI_NAME, YOUR_WIFI_PASSWORD);
        receiver.currentPage = 0;
    }
    receiver.lastActivity = millis();
    xTaskCreate(TaskDisp, "Disp", 8192, NULL, 1, NULL);
    xTaskCreate(TaskKeys, "Keys", 4096, NULL, 1, NULL);
}

void loop() { 
    vTaskDelete(NULL); 
}