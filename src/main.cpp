/*
 * ControlPad Test Program - Static Background for Flicker Testing
 * 
 * This program tests the ControlPad hardware by:
 * 1. Displaying a static background pattern (to detect flickering)
 * 2. Cycling through button LED highlights (one at a time)
 * 3. Detecting and responding to button presses
 * 4. Using optimized USB host library for stable operation
 */

#include <Arduino.h>
#include "ControlPad.h"

// Global instances
ControlPad controlPad;

// LED cycling state
static uint8_t currentHighlightButton = 0;
static unsigned long lastHighlightTime = 0;
static bool highlightState = false;  // false = OFF, true = ON

// Hardware initialization state
static bool hardwareInitialized = false;
    
// Transfer tracking for stability monitoring
static uint32_t globalTransferCounter = 0;

// Button press tracking - real button states
static bool buttonPressed[24] = {false};

// Colors for button states
const ControlPadColor BACKGROUND_COLOR = {30, 30, 60};      // Static blue background
const ControlPadColor HIGHLIGHT_COLOR = {255, 255, 255};   // White highlight (animation only)
const ControlPadColor PRESS_COLOR = {255, 255, 0};         // Yellow when pressed
const ControlPadColor PRESS_HIGHLIGHT_COLOR = {255, 128, 0}; // Orange when pressed AND highlighted

// Button colors array (24 buttons)
static ControlPadColor buttonColors[24];

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🔍 ControlPad Flicker Test - Static Background");
    Serial.println("✨ Using custom USB library with reduced interrupt frequency");
    Serial.println("🎯 Static background + button highlights + press detection");
    Serial.println("👀 Watch for any flickering in the static background!\n");
    
    // Initialize all buttons to static background color
    for (int i = 0; i < 24; i++) {
        buttonColors[i] = BACKGROUND_COLOR;
        buttonPressed[i] = false;
    }
    
    // Initialize ControlPad (this creates and initializes the hardware internally)
    Serial.println("🔧 Initializing ControlPad system...");
    if (!controlPad.begin()) {
        Serial.println("❌ Failed to initialize ControlPad system");
        hardwareInitialized = false;
        while (1) {
            Serial.println("💀 ControlPad initialization failed - system halted");
            delay(5000);
        }
    }
    
    // Disable smart LED rate limiting for immediate responsiveness
    controlPad.enableInstantUpdates(true);  // Disable all rate limiting
    controlPad.enableSmartUpdates(false);   // Disable smart update system
    
    // Hardware is now properly initialized
    hardwareInitialized = true;
    Serial.println("✅ ControlPad system initialized successfully");
    Serial.println("🚀 Instant LED updates enabled for maximum responsiveness");
    Serial.println("🎯 Button priority: Background < Highlight < Press < Press+Highlight");
    Serial.println("🔍 Starting static background test - monitoring for flicker...\n");
    
    // Set initial static background
    controlPad.setAllButtonColors(buttonColors);
    controlPad.forceUpdate();  // Force immediate update
    globalTransferCounter++;
    
    Serial.println("🎮 System ready - press buttons to test!");
}

void updateButtonColors() {
    bool needsUpdate = false;
    
    // Set colors based on current state priority: Press+Highlight > Press > Highlight > Background
    for (int i = 0; i < 24; i++) {
        ControlPadColor newColor;
        
        if (buttonPressed[i] && i == currentHighlightButton && highlightState) {
            // Pressed AND highlighted has highest priority - special orange color
            newColor = PRESS_HIGHLIGHT_COLOR;
        } else if (buttonPressed[i]) {
            // Pressed only - yellow
            newColor = PRESS_COLOR;
        } else if (i == currentHighlightButton && highlightState) {
            // Highlighted only - white
            newColor = HIGHLIGHT_COLOR;
        } else {
            // Background - blue
            newColor = BACKGROUND_COLOR;
        }
        
        // Only update if color changed
        if (buttonColors[i].r != newColor.r || 
            buttonColors[i].g != newColor.g || 
            buttonColors[i].b != newColor.b) {
            buttonColors[i] = newColor;
            needsUpdate = true;
        }
    }
    
    if (needsUpdate) {
        controlPad.setAllButtonColors(buttonColors);
        controlPad.forceUpdate();  // Force immediate update
        globalTransferCounter++;
    }
}

void loop() {
    // Only proceed if hardware is properly initialized
    if (!hardwareInitialized) {
        delay(1000);
        return;
    }
    
    // Process any incoming button events
    controlPad.poll();
    
    // Handle button press events - track real button state
    ControlPadEvent event;
    while (controlPad.pollEvent(event)) {
        if (event.type == ControlPadEventType::Button) {
            if (event.button.pressed) {
                Serial.printf("🔵 Button %d PRESSED\n", event.button.button);
                buttonPressed[event.button.button] = true;
        } else {
                Serial.printf("⚪ Button %d RELEASED\n", event.button.button);
                buttonPressed[event.button.button] = false;
            }
        }
}

    // LED cycling - cycle every 300ms (slower to be less aggressive)
    if (millis() - lastHighlightTime >= 300) {
        lastHighlightTime = millis();
        
        if (!highlightState) {
            // Turn ON the current button's highlight
            highlightState = true;
        } else {
            // Turn OFF the current button's highlight and move to next
            highlightState = false;
            currentHighlightButton = (currentHighlightButton + 1) % 24;
        }
        
        // Status feedback every 100 cycles (much less frequent)
        static int cycleCount = 0;
        cycleCount++;
        if ((cycleCount % 100) == 0) {
            Serial.printf("🔍 Cycle #%d completed - Static background stable (transfers: %lu)\n", 
                         cycleCount, globalTransferCounter);
        }
    }
    
    // Update button colors based on current priorities
    updateButtonColors();
}