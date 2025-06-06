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
    Serial.println("🎮 Ready for button events - press buttons to see WHITE highlighting!");
    Serial.println("📌 Using new combined LED package system for reduced flicker");
    Serial.println("💡 Buttons will highlight in WHITE when pressed, then return to original colors");
}

uint32_t now = micros();

void loop() {
    // Still process USB events to keep the connection alive
    controlPad.poll();
    
    // Process any real button events (optional - you can disable this if you want only the loop)
    ControlPadEvent event;
    while (controlPad.pollEvent(event)) {
        if (event.type == ControlPadEventType::Button) {
            // Optional: Handle real button events alongside the automated loop
            controlPad.setButtonHighlight(event.button.button, event.button.pressed);
        }
    }
    
    // *** AUTOMATED HIGHLIGHT LOOP FOR TESTING ALL 24 BUTTONS ***
    static unsigned long lastHighlightTime = 0;
    static uint8_t currentHighlightButton = 0;
    static bool highlightState = false; // false = unhighlighted, true = highlighted
    static uint8_t highlightCounter = 0; // Track how many highlights we've done
    static uint8_t passCounter = 0; // Track complete passes through all 24 buttons
    
    unsigned long currentTime = millis();
    
    // Change highlight every 500ms (adjust this timing as needed)
    if (currentTime - lastHighlightTime >= 100) {
        lastHighlightTime = currentTime;
        
        if (!highlightState) {
            // Turn OFF the previous button's highlight
            if (currentHighlightButton > 0) {
                controlPad.setButtonHighlight(currentHighlightButton - 1, false);
            } else {
                // Wrap around - turn off button 23 when starting from button 0
                controlPad.setButtonHighlight(23, false);
            }
            
            // Turn ON the current button's highlight
            controlPad.setButtonHighlight(currentHighlightButton, true);
            Serial.printf("🔥 Highlighting button %d (LED index %d) [Count: %d]\n", 
                         currentHighlightButton + 1, currentHighlightButton, highlightCounter);
            
            highlightState = true;
            highlightCounter++;
        } else {
            // Turn OFF the current button's highlight
            controlPad.setButtonHighlight(currentHighlightButton, false);
            Serial.printf("⚡ Un-highlighting button %d (LED index %d)\n", currentHighlightButton + 1, currentHighlightButton);
            
            // Move to the next button
            uint8_t nextButton = (currentHighlightButton + 1) % 24;
            
            // CYCLE RESET: Add pause when wrapping from button 23 back to button 0
            // This prevents timing drift accumulation between cycles
            if (currentHighlightButton == 23 && nextButton == 0) {
                passCounter++;
                Serial.printf("🔄 Pass %d completed: Wrapping from button 24 to button 1\n", passCounter);
                
                // SINGLE-PASS RESET: Reset after every pass since accumulation happens very quickly
                if (passCounter % 2 == 0) {
                    Serial.printf("🔄 EXTENDED RESET: Pass %d - Clearing accumulated state\n", passCounter);
                    delay(75); // Longer reset every 2nd pass to prevent button 15 Package 2 issues
                } else {
                    Serial.printf("🔄 STANDARD RESET: Pass %d - Normal cycle break\n", passCounter);
                    delay(35); // Slightly longer standard pause to prevent accumulation
                }
            }
            
            currentHighlightButton = nextButton;
            highlightState = false;
        }
    }
}