#include <Arduino.h>
#include "ControlPad.h"

// Create a ControlPad instance
ControlPad controlPad;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {
        // Wait for Serial
    }
    
    Serial.println("\n🚀 ControlPad Library Test with Smart LED Commands 🚀");
    Serial.println("====================================================");
    
    // Initialize the ControlPad
    if (controlPad.begin()) {
        Serial.println("✅ ControlPad initialized successfully");
        
        // Test the new LED system with a colorful pattern
        Serial.println("🌈 Testing 5-command LED sequence...");
        
        // Set a rainbow pattern across the buttons
        controlPad.setLed(0, 255, 0, 0);     // Button 1 = Red
        controlPad.setLed(1, 255, 127, 0);   // Button 2 = Orange
        controlPad.setLed(2, 255, 255, 0);   // Button 3 = Yellow
        controlPad.setLed(3, 0, 255, 0);     // Button 4 = Green
        controlPad.setLed(4, 0, 0, 255);     // Button 5 = Blue
        
        controlPad.setLed(5, 127, 0, 255);   // Button 6 = Purple
        controlPad.setLed(6, 255, 0, 127);   // Button 7 = Pink
        controlPad.setLed(7, 255, 255, 255); // Button 8 = White
        controlPad.setLed(8, 127, 127, 127); // Button 9 = Gray
        controlPad.setLed(9, 255, 64, 0);    // Button 10 = Red-Orange
        
        // Set remaining buttons to a dim cyan
        for (int i = 10; i < 24; i++) {
            controlPad.setLed(i, 0, 64, 64);
        }
        
        // Send all LED updates using the new 5-command sequence
        controlPad.updateLeds();
        
        Serial.println("✅ Rainbow LED pattern set using smart command system");
    } else {
        Serial.println("❌ Failed to initialize ControlPad");
    }
}

void loop() {
    static unsigned long lastUpdate = 0;
    static int patternStep = 0;
    
    // Poll for events
    ControlPadEvent event;
    if (controlPad.pollEvent(event)) {
        if (event.type == ControlPadEventType::Button) {
            Serial.printf("🔘 Button %d %s\n", 
                         event.button.button + 1,  // Display as 1-based
                         event.button.pressed ? "pressed" : "released");
            
            // Light up the pressed button with bright white
            if (event.button.pressed) {
                controlPad.setLed(event.button.button, 255, 255, 255);
                controlPad.updateLeds();
            } else {
                // Restore original pattern when released
                setup(); // Quick way to restore the rainbow pattern
            }
        } else if (event.type == ControlPadEventType::HallSensor) {
            Serial.printf("📊 Hall sensor %d: %d\n", event.hall.sensor, event.hall.value);
        }
    }
    
    // Create a slow breathing effect every 5 seconds
    // if (millis() - lastUpdate > 5000) {
    //     lastUpdate = millis();
    //     patternStep = (patternStep + 1) % 4;
        
    //     uint8_t brightness = 0;
    //     switch (patternStep) {
    //         case 0: brightness = 255; break;  // Bright
    //         case 1: brightness = 128; break;  // Medium
    //         case 2: brightness = 64; break;   // Dim
    //         case 3: brightness = 32; break;   // Very dim
    //     }
        
    //     // Update corner buttons with breathing effect
    //     controlPad.setLed(0, brightness, 0, 0);    // Top-left
    //     controlPad.setLed(4, 0, brightness, 0);    // Top-right
    //     controlPad.setLed(20, 0, 0, brightness);   // Bottom-left
    //     controlPad.setLed(24, brightness, brightness, 0); // Bottom-right
        
    //     controlPad.updateLeds();
    //     Serial.printf("💨 Breathing effect step %d (brightness: %d)\n", patternStep + 1, brightness);
    // }
    
    delay(10);
}