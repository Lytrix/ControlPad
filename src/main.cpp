#include <Arduino.h>
#include <USBHost_t36.h>  // For USBHost Task() processing
#include "ControlPad.h"
#include "ControlPadHardware.h"  // For USBControlPad class

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
    
    Serial.println("🚀 ControlPad Combined Package Test - Maximum LED Speed!");
    
    // ===== USB HOST INITIALIZATION (USBHost_t36 standard pattern) =====
    Serial.println("🔌 Starting USB Host (main.cpp pattern)...");
    globalUSBHost.begin();
    Serial.println("✅ USB Host started in main.cpp");
    
    // Standard driver callbacks removed - only our custom ControlPad driver is active
    
    // Give USB Host time to initialize and test basic detection
    Serial.println("🧪 Testing USB Host for 3 seconds...");
    for (int i = 0; i < 30; i++) {
        globalUSBHost.Task();
        delay(100);
        if (i % 10 == 0) {
            Serial.printf("   USB Host test %d/30\n", i + 1);
        }
    }
    Serial.println("✅ USB Host basic test completed");
    
    delay(500);
    
    if (!controlPad.begin()) {
        Serial.println("❌ Failed to initialize ControlPad");
        return;
    }
    
    Serial.println("✅ ControlPad initialized successfully!");
    
    // *** WAIT FOR DEVICE TO BE FULLY READY ***
    Serial.println("⏳ Waiting for device to be ready for LED commands...");
    delay(2000);  // Give device time to complete activation sequence
    
    // Disable smart updates - we want immediate LED updates for responsiveness
    controlPad.enableSmartUpdates(false);
    
    // Set instant updates for maximum responsiveness
    controlPad.enableInstantUpdates(true);
    
    // Set initial rainbow colors so we can see the highlighting effect
    Serial.println("🌈 Setting initial rainbow colors...");
    ControlPadColor rainbowColors[24] = {
        {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
        {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
        {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
        {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
        {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
    };
    
    // Set all colors at once using the smart LED system
    controlPad.setAllButtonColors(rainbowColors);
    
    // Force immediate update to show rainbow colors at startup
    Serial.println("🌈 Forcing immediate LED update for rainbow colors...");
    controlPad.forceUpdate();
    
    Serial.println("✅ Initial colors set and updated!");
    
    // *** START BUTTON ANIMATION ***
    Serial.println("🎭 Starting button animation loop...");
    controlPad.enableAnimation();
    
    Serial.println("🎮 Ready for button events - press buttons to see WHITE highlighting!");
    Serial.println("📌 Using optimized LED package system for reduced flicker");
    Serial.println("💡 Buttons will highlight in WHITE when pressed, then return to original colors");
    Serial.println("🎭 Animation: WHITE button cycling active (normal priority)");
    Serial.println("⚡ Button presses: WHITE highlighting (HIGH priority)");
    Serial.println("🔧 PROPER USB HOST PATTERN: Task() called every loop iteration (like official examples)");
}

void loop() {
    static uint32_t loopCounter = 0;
    static unsigned long lastDebug = 0;
    
    loopCounter++;
    
    // *** CRITICAL: ONLY USBHost_t36 Task() - This is the proper pattern ***
    // All events come through USB callbacks, no custom polling needed
    globalUSBHost.Task();
    
    // *** UPDATE ANIMATION (non-blocking) ***
    controlPad.updateAnimation();
    
    // *** ENHANCED DEBUG OUTPUT (temporarily for diagnostics) ***
    // Show status every 1 second to monitor loop performance
    if (millis() - lastDebug >= 1000) {  // Changed from 10000 to 1000
        lastDebug = millis();
        
        // Get LED queue status for monitoring
        size_t queueSize;
        bool isProcessing;
        getLEDQueueStatus(&queueSize, &isProcessing);
        
        Serial.printf("🎵 MIDI-Ready: Loop #%lu (1s) - Queue: %zu items, Processing: %s\n", 
                     loopCounter, queueSize, isProcessing ? "YES" : "NO");
    }
    
    // *** MAIN LOOP IS NOW OPTIMIZED FOR MIDI TIMING ***
    // No delays, no blocking calls, no custom polling
    // All button events come through USB callbacks in ControlPadHardware.cpp
}