#include <Arduino.h>
#include "ControlPad.h"

// Create a ControlPad instance
ControlPad controlPad;

// Store original colors so we can restore them
ControlPadColor originalColors[24];

// Track currently pressed buttons to prevent flickering
bool buttonPressed[24] = {false};
bool stateChanged = false;

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
        
        // Set a rainbow pattern across the buttons and store the colors
        originalColors[0] = {255, 0, 0};     // Button 1 = Red
        originalColors[1] = {255, 127, 0};   // Button 2 = Orange
        originalColors[2] = {255, 255, 0};   // Button 3 = Yellow
        originalColors[3] = {0, 255, 0};     // Button 4 = Green
        originalColors[4] = {0, 0, 255};     // Button 5 = Blue
        
        originalColors[5] = {127, 0, 255};   // Button 6 = Purple
        originalColors[6] = {255, 0, 127};   // Button 7 = Pink
        originalColors[7] = {255, 255, 255}; // Button 8 = White
        originalColors[8] = {127, 127, 127}; // Button 9 = Gray
        originalColors[9] = {255, 64, 0};    // Button 10 = Red-Orange
        
        // Set remaining buttons to a dim cyan
        for (int i = 10; i < 24; i++) {
            originalColors[i] = {0, 64, 64};
        }
        
        // Apply the colors to the ControlPad
        for (int i = 0; i < 24; i++) {
            controlPad.setLed(i, originalColors[i].r, originalColors[i].g, originalColors[i].b);
        }
        
        // Send all LED updates using the new 5-command sequence
        controlPad.updateLeds();
        
        Serial.println("✅ Rainbow LED pattern set using smart command system");
          } else {
        Serial.println("❌ Failed to initialize ControlPad");
    }
}

void loop() {
    controlPad.poll();
    
    // PROCESS MULTIPLE EVENTS PER CYCLE to reduce lag
    ControlPadEvent event;
    int eventsProcessed = 0;
    const int MAX_EVENTS_PER_MAIN_LOOP = 5;  // Process up to 5 events per cycle
    bool ledUpdateNeeded = false;  // Track if we need to update LEDs
    
    while (eventsProcessed < MAX_EVENTS_PER_MAIN_LOOP && controlPad.pollEvent(event)) {
        eventsProcessed++;
        
        if (event.type == ControlPadEventType::Button) {
            // STATE TRACKING: Only change LED state if button state actually changed
            if (event.button.pressed && !buttonPressed[event.button.button]) {
                // Button newly pressed
                buttonPressed[event.button.button] = true;
                ledUpdateNeeded = true;
            } else if (!event.button.pressed && buttonPressed[event.button.button]) {
                // Button newly released
                buttonPressed[event.button.button] = false;
                ledUpdateNeeded = true;
            }
            // Ignore duplicate press/release events
        }
    }
    
    // OPTIMIZED: Only update LEDs once per main loop cycle if state actually changed
    if (ledUpdateNeeded) {
        // Set LED colors based on current button states
        for (int i = 0; i < 24; i++) {
            if (buttonPressed[i]) {
                // Button is pressed - highlight with white
                controlPad.setLed(i, 255, 255, 255);
    } else {
                // Button is not pressed - restore original color
                ControlPadColor& orig = originalColors[i];
                controlPad.setLed(i, orig.r, orig.g, orig.b);
  }
} 
  
        controlPad.updateLeds();
}
  
    delay(5);  // Reduced delay for better responsiveness
}