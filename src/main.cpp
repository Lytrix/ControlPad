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

// Button colors array (24 buttons) - indexed by PHYSICAL button order (0-23)
static ControlPadColor buttonColors[24];

// Convert vertical software index to physical button number for display
uint8_t verticalToPhysicalButton(uint8_t verticalIndex) {
    if (verticalIndex >= 24) return 0;
    
    // Reverse the column-major mapping to get physical button (1-24)
    uint8_t col = verticalIndex / 5;  // Column (0-4)
    uint8_t row = verticalIndex % 5;  // Row (0-4)
    
    // Convert to physical button number: button = row * 5 + col + 1
    return (row * 5 + col) + 1;  // +1 for 1-based physical numbering
}

// Convert vertical software index to physical array index (0-23)
uint8_t verticalToPhysicalIndex(uint8_t verticalIndex) {
    if (verticalIndex >= 24) return 0;
    
    // Reverse the column-major mapping to get physical index (0-23)
    uint8_t col = verticalIndex / 5;  // Column (0-4)
    uint8_t row = verticalIndex % 5;  // Row (0-4)
    
    return row * 5 + col;  // Physical index (0-23)
}

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
    // NOTE: Everything uses PHYSICAL indexing (0-23) for intuitive horizontal behavior
    for (int physicalIndex = 0; physicalIndex < 24; physicalIndex++) {
        ControlPadColor newColor;
        
        if (buttonPressed[physicalIndex] && physicalIndex == currentHighlightButton && highlightState) {
            // Pressed AND highlighted has highest priority - special orange color
            newColor = PRESS_HIGHLIGHT_COLOR;
        } else if (buttonPressed[physicalIndex]) {
            // Pressed only - yellow
            newColor = PRESS_COLOR;
        } else if (physicalIndex == currentHighlightButton && highlightState) {
            // Highlighted only - white
            newColor = HIGHLIGHT_COLOR;
        } else {
            // Background - blue
            newColor = BACKGROUND_COLOR;
        }
        
        // Only update if color changed
        if (buttonColors[physicalIndex].r != newColor.r || 
            buttonColors[physicalIndex].g != newColor.g || 
            buttonColors[physicalIndex].b != newColor.b) {
            buttonColors[physicalIndex] = newColor;
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
    
    // Handle button press events - convert from vertical to physical indexing
    ControlPadEvent event;
    while (controlPad.pollEvent(event)) {
        if (event.type == ControlPadEventType::Button) {
            uint8_t verticalIndex = event.button.button;  // From hardware (vertical)
            uint8_t physicalIndex = verticalToPhysicalIndex(verticalIndex);  // Convert to physical
            uint8_t physicalButton = physicalIndex + 1;  // For display (1-24)
            
            if (event.button.pressed) {
                Serial.printf("🔵 Button %d (vertical 0x%X=%d, physical index %d) PRESSED\n", 
                             physicalButton, verticalIndex, verticalIndex, physicalIndex);
                Serial.printf("    Conversion: col=%d, row=%d, calculated physicalIndex=%d\n",
                             verticalIndex / 5, verticalIndex % 5, (verticalIndex % 5) * 5 + (verticalIndex / 5));
                buttonPressed[physicalIndex] = true;
            } else {
                Serial.printf("⚪ Button %d (vertical 0x%X=%d, physical index %d) RELEASED\n", 
                             physicalButton, verticalIndex, verticalIndex, physicalIndex);
                buttonPressed[physicalIndex] = false;
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