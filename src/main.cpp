#include <Arduino.h>
#include <USBHost_t36.h>  // For USBHost Task() processing
#include "ControlPadHardware.h"  // For USBControlPad class
#include "ControlPad.h"
#include "USBSynchronizedPacketController.h"

// ===== USB HOST SETUP (USBHost_t36 standard pattern) =====
// These must be declared at global scope for automatic driver discovery
extern USBHost globalUSBHost;
extern USBHub hub1, hub2;
extern USBHIDParser hid1, hid2, hid3;
extern USBControlPad globalControlPadDriver;

// Create a ControlPad instance
ControlPad controlPad;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🚀 ControlPad Starting...");
    
    // ===== USB HOST INITIALIZATION (USBHost_t36 standard pattern) =====
    globalUSBHost.begin();
    Serial.println("🔌 USB Host started");
    
    // Give USB Host time to initialize
    for (int i = 0; i < 30; i++) {
        globalUSBHost.Task();
        delay(100);
    }
    
    delay(500);
    
    if (!controlPad.begin()) {
        Serial.println("❌ Failed to initialize ControlPad");
        return;
    }
    
    Serial.println("✅ ControlPad initialized");
    
    delay(2000);  // Give device time to complete activation sequence
    
    // Disable smart updates - we want immediate LED updates for responsiveness
    controlPad.enableSmartUpdates(false);
    
    // Set instant updates for maximum responsiveness
    controlPad.enableInstantUpdates(true);
    
    // Set initial rainbow colors
    ControlPadColor rainbowColors[24] = {
        {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
        {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
        {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
        {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
        {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
    };
    
    controlPad.setAllButtonColors(rainbowColors);
    controlPad.forceUpdate();
    
    // Start button animation
    controlPad.enableAnimation();
    
    Serial.println("🎮 Ready - Animation active, press buttons for highlighting");
    
    // Initialize the USB-synchronized controller
    usbSyncController.initialize();
    
    Serial.println("🚀 ControlPad initialized with USB-synchronized packet timing");
    Serial.println("📋 Packet timing windows:");
    Serial.println("   📦 Critical packets 1-2: 0-10ms in USB cycle");  
    Serial.println("   📤 Normal packets 3-4: 15-25ms in USB cycle");
    Serial.println("   🔧 USB management: 25-32ms in USB cycle");
}

void loop() {
    static uint32_t loopCounter = 0;
    static unsigned long lastDebug = 0;
    static unsigned long lastAnimationUpdate = 0;
    
    loopCounter++;
    
    // *** HITCH HIKE ON USB HOST PROCESSES ***
    // Monitor actual USB Host activity patterns instead of predicting timing
    usbSyncController.monitorUSBActivity();
    
    // *** CRITICAL: USB Host Task() - This is what we're hitch hiking on ***
    // The USB activity monitor watches this process to detect when USB is busy
    globalUSBHost.Task();
    
    // *** UNIFIED LED MANAGER WITH USB CLEANUP PROTECTION ***
    // Check for LED updates every 50ms (fast MIDI looper feedback requirement)
    if (millis() - lastAnimationUpdate >= 50) {
        // *** CHECK USB CLEANUP PROTECTION BEFORE LED UPDATES ***
        if (!usbSyncController.isUSBCleanupActive()) {
            controlPad.updateUnifiedLEDs();
        } else {
            // USB cleanup in progress - skip LED update to prevent flickering
            Serial.println("🛡️ LED update SKIPPED - USB cleanup protection active");
        }
        lastAnimationUpdate = millis();
    }
    
    // Minimal status output every 10 seconds
    if (millis() - lastDebug >= 10000) {
        Serial.printf("⚡ Main Loop: %lu cycles, LED updates active\n", loopCounter);
        lastDebug = millis();
    }
}