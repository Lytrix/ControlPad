#include <Arduino.h>
#include "ControlPad.h"

// Create a ControlPad instance
ControlPad controlPad;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    
    Serial.println("\n🚀 ControlPad Library Test 🚀");
    Serial.println("=============================");
    
    // Initialize the ControlPad
    if (controlPad.begin()) {
        Serial.println("✅ ControlPad initialized successfully");
        
        // Test setting a few LEDs (using individual r,g,b parameters)
        controlPad.setLed(1, 255, 0, 0);     // Button 1 = Red
        controlPad.setLed(6, 0, 255, 0);     // Button 6 = Green  
        controlPad.setLed(11, 0, 0, 255);    // Button 11 = Blue
        controlPad.updateLeds();
        
        Serial.println("✅ Test LED pattern set");
    } else {
        Serial.println("❌ Failed to initialize ControlPad");
    }
}

void loop() {
    // Poll for events
    ControlPadEvent event;
    if (controlPad.pollEvent(event)) {
        if (event.type == ControlPadEventType::Button) {
            Serial.printf("🔘 Button %d %s\n", 
                         event.button.button,
                         event.button.pressed ? "pressed" : "released");
            
            // Light up the pressed button
            if (event.button.pressed) {
                controlPad.setLed(event.button.button, 255, 255, 0); // Yellow
                controlPad.updateLeds();
            }
        } else if (event.type == ControlPadEventType::HallSensor) {
            Serial.printf("📊 Hall sensor %d: %d\n", event.hall.sensor, event.hall.value);
        }
    }
    
    delay(10);
} 