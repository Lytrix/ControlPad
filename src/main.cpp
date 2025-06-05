#include <Arduino.h>
#include "ControlPad.h"

// Create a ControlPad instance
ControlPad controlPad;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🚀 ControlPad Combined Package Test - Maximum LED Speed!");
    
    if (!controlPad.begin()) {
        Serial.println("❌ Failed to initialize ControlPad");
        return;
    }
    
    Serial.println("✅ ControlPad initialized successfully!");
    
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
    
    Serial.println("✅ Initial colors set!");
    Serial.println("🎮 Ready for button events - press buttons to see WHITE highlighting!");
    Serial.println("📌 Using new combined LED package system for reduced flicker");
    Serial.println("💡 Buttons will highlight in WHITE when pressed, then return to original colors");
}
uint32_t now = 0;

void loop() {
    controlPad.poll();
    
    // Process all pending events
    ControlPadEvent event;
        while (controlPad.pollEvent(event)) {
            if (event.type == ControlPadEventType::Button) {
                Serial.printf("🎯 Button %d %s (instant highlight)\n", 
                            event.button.button + 1, 
                            event.button.pressed ? "PRESSED" : "RELEASED");
                
                // INSTANT highlight with combined package system
                // This should show WHITE highlighting over the rainbow colors
                controlPad.setButtonHighlight(event.button.button, event.button.pressed);
            }
        }
    // Small delay to prevent overwhelming the serial output
    //delay(1);
}