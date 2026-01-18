# ESP32-HTERM

![GitHub release](https://img.shields.io/github/v/release/hankli22/ESP32-HTerm?color=blue&logo=github)
![License](https://img.shields.io/github/license/hankli22/ESP32-HTerm?color=green)
![Top Language](https://img.shields.io/github/languages/top/hankli22/ESP32-HTerm)

### **ESP32-HTERM v1.0.0 Changelog**

**Added:** 
*   **Hypixel API support** with real-time status tracking!
*   **WiFi connectivity** for cloud data fetching.
*   **UI Culling logic**: skips rendering for unused pages :D *fps++*

**Fixed:** 
*   WiFi / ESP-NOW hardware conflict via **Comm-Mode switching**.

**Optimized:** 
*   Real-time **Memory/CPU monitoring** overlay.

### **Features:**
1. **Dual-Mode Wireless**: Supports both WiFi and ESP-NOW LR.
2. **Environment Monitor**: Receives and displays data from remote sensors.
3. **Hypixel Integration**: Check your current game type and online status on the go.

### **Compatibility:**
*   **Displays**: Almost any **128x64** screen supported by the **U8g2** library.
*   **Hardware**: Any **ESP32** series chip with WiFi and ESP-NOW capabilities.
*   **Legacy Support**: Fully compatible with older sensor nodes! No need to dismantle or re-program your existing hardware. **It just works!**

### **Requirements:**
*   A display with **128x64 resolution** (U8g2 compatible).
*   **2x Push buttons** for navigation.
*   Some wires (obviously! :o ).
*   **TP4056** or similar LiPo charging modules.
*   **LiPo battery** (>=600 mAh recommended for decent endurance).
    *(Note: Optional if you intend to use USB power only).*

### **Usage:**
*   **Configuration**: You need to manually replace the placeholder values in `secret.cpp` with your actual credentials (WiFi SSID, Password, Hypixel API Key, and UUID) before flashing the firmware.
