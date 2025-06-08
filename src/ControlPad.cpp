#include "ControlPad.h"
#include "ControlPadHardware.h"
#include "USBSynchronizedPacketController.h"
#include <algorithm>

ControlPadHardware* hw;

// External reference to the global USB synchronization controller
extern USBSynchronizedPacketController usbSyncController;

ControlPad::ControlPad() {
    hw = nullptr;
    
    // Initialize event queue
    for (int i = 0; i < EVENT_QUEUE_SIZE; ++i) {
        eventQueue[i] = {};
    }
    eventHead = 0;
    eventTail = 0;
    
    // Initialize LED state
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledState[i] = {0, 0, 0};
        buttonState[i] = false;
        
        // Smart LED management initialization
        baseColors[i] = {0, 0, 0};
        currentColors[i] = {0, 0, 0};
        buttonHighlighted[i] = false;
        
        // Initialize rate limiting for each button
        lastButtonTime[i] = 0;
    }
    
    // Initialize hall sensors
    for (int i = 0; i < 4; ++i) {
        hallValues[i] = 0;
    }
    
    // Smart LED system defaults with proper USB timing
    ledsDirty = false;
    smartUpdatesEnabled = true;  // Enable by default
    lastUpdateTime = 0;
    updateInterval = 20;  // Use 20ms intervals for responsive LED feedback without USB conflicts
    ledUpdateInProgress = false;  // Initialize concurrency protection
    
    // Initialize per-LED dirty flags
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledDirtyFlags[i] = false;
    }
}

ControlPad::~ControlPad() { 
    delete hw; 
}

bool ControlPad::begin() { 
    hw = new ControlPadHardware();
    return hw->begin(*this); 
}

void ControlPad::poll() {
    static int pollCount = 0;
    pollCount++;
    
    if (hw) {
        hw->poll();
        
        // Auto-update LEDs if smart updates are enabled and changes are pending
        if (smartUpdatesEnabled && ledsDirty) {
            Serial.println("🎨 ControlPad::poll() - LED updates pending, calling updateSmartLeds()");
            updateSmartLeds();
        }
        
        // Debug: Show poll activity
        // if (pollCount % 10000 == 0) {
        //     Serial.printf("🔄 ControlPad::poll() #%d - Called hw->poll(), ledsDirty=%s\n", 
        //                  pollCount, ledsDirty ? "true" : "false");
        // }
    } else {
        Serial.println("🚫 ControlPad::poll() - No hardware instance!");
    }
}

bool ControlPad::getButtonState(uint8_t button) const {
    if (button < CONTROLPAD_NUM_BUTTONS) return buttonState[button];
    return false;
}

int ControlPad::getHallValue(uint8_t sensor) const {
    if (sensor < 4) return hallValues[sensor];
    return 0;
}

void ControlPad::setLed(uint8_t button, uint8_t r, uint8_t g, uint8_t b) {
    if (button < CONTROLPAD_NUM_BUTTONS) {
        ledState[button] = {r, g, b};
        // Serial.printf("💡 Set LED %d to RGB(%d,%d,%d)\n", button + 1, r, g, b);
    }
}

void ControlPad::setLed(uint8_t index, const ControlPadColor& color) {
    if (index < CONTROLPAD_NUM_BUTTONS) {
        ledState[index] = color;
    }
}

void ControlPad::updateLeds() {
    // Serial.println("🚀 ControlPad::updateLeds() called - triggering hardware LED update");
    
    // CRITICAL FIX: Don't bypass smart update system - use forceUpdate() instead
    // This ensures all LED updates go through proper USB serialization
    if (!hw) return;
    
    // Mark all LEDs as dirty and force immediate update through proper channels
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        currentColors[i] = ledState[i];  // Sync current colors with LED state
        ledDirtyFlags[i] = true;
    }
    ledsDirty = true;
    
    // Force update through proper mutex-protected path
    forceUpdate();
}

// Convenience methods for common patterns
void ControlPad::setAllLedsOff() {
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledState[i] = {0, 0, 0};
    }
}

void ControlPad::setAllLedsColor(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledState[i] = {r, g, b};
    }
}

void ControlPad::setRainbowPattern() {
    // Create a rainbow pattern across the 24 buttons
    const ControlPadColor rainbow[24] = {
        {255, 0, 0},   {255, 64, 0},  {255, 127, 0}, {255, 191, 0}, {255, 255, 0},  // Row 1: Red to Yellow
        {191, 255, 0}, {127, 255, 0}, {64, 255, 0},  {0, 255, 0},   {0, 255, 64},   // Row 2: Yellow to Green
        {0, 255, 127}, {0, 255, 191}, {0, 255, 255}, {0, 191, 255}, {0, 127, 255},  // Row 3: Green to Cyan
        {0, 64, 255},  {0, 0, 255},   {64, 0, 255},  {127, 0, 255}, {191, 0, 255},  // Row 4: Cyan to Blue
        {255, 0, 255}, {255, 0, 191}, {255, 0, 127}, {255, 0, 64}   // Row 5: Blue to Magenta (24 buttons)
    };
    
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledState[i] = rainbow[i];
    }
}

// Event API implementation
bool ControlPad::pollEvent(ControlPadEvent& event) {
    // Check if queue has events
    if (eventHead != eventTail) {
        event = eventQueue[eventTail];
        eventTail = (eventTail + 1) % EVENT_QUEUE_SIZE;
        return true;
    }
    return false;
}

void ControlPad::onEvent(ControlPadEventCallback cb) {
    eventCallback = cb;
}

// Internal: called by hardware layer
void ControlPad::pushEvent(const ControlPadEvent& event) {
    // Add event to queue - NO direct LED processing to prevent flickering!
    // LED updates MUST happen in main thread context only
    size_t nextHead = (eventHead + 1) % EVENT_QUEUE_SIZE;
    if (nextHead != eventTail) { // Queue not full
        eventQueue[eventHead] = event;
        eventHead = nextHead;
        
        // Only update button state immediately - NO LED updates in interrupt context
        if (event.type == ControlPadEventType::Button && event.button.button < CONTROLPAD_NUM_BUTTONS) {
            buttonState[event.button.button] = event.button.pressed;
        }
        
        // If callback is set, call it immediately
        if (eventCallback) {
            eventCallback(event);
        }
    } else {
        Serial.printf("❌ ControlPad::pushEvent() - Queue FULL! Button %d %s event dropped!\n",
                     event.button.button + 1, event.button.pressed ? "PRESSED" : "RELEASED");
    }
}

// Smart LED management implementation - FLICKER-FREE DEFERRED HIGHLIGHTING
void ControlPad::setButtonHighlight(uint8_t buttonIndex, bool pressed) {
    if (buttonIndex >= CONTROLPAD_NUM_BUTTONS) return;
    
    // ALWAYS process button state changes - don't skip based on previous state
    // Update the state first
    buttonHighlighted[buttonIndex] = pressed;
    
    // DIRECT ASSIGNMENT - No intermediate calculations
    if (pressed) {
        // IMMEDIATE button press highlighting - no coordination delay
        currentColors[buttonIndex] = {255, 255, 255};
        Serial.printf("⚡ IMMEDIATE highlight for button %d press\n", buttonIndex + 1);
    } else {
        // Only coordinate button RELEASES to prevent flickering during restore
        if (!usbSyncController.isSafeToSendPacket()) {
            Serial.printf("🚫 Button %d release blocked by coordination - will retry\n", buttonIndex + 1);
            // Mark LED as dirty so it will be sent when coordination allows
            ledDirtyFlags[buttonIndex] = true;
            ledsDirty = true;
            return;
        }
        
        // Restore base color with coordination
        if (baseColors[buttonIndex].r == 0 && baseColors[buttonIndex].g == 0 && baseColors[buttonIndex].b == 0) {
            baseColors[buttonIndex] = {64, 64, 64};  // Default base color
        }
        currentColors[buttonIndex] = baseColors[buttonIndex];
        Serial.printf("⚡ COORDINATED restore for button %d release\n", buttonIndex + 1);
    }
    
    // ⚡ LED UPDATE - Immediate for press, coordinated for release
    if (hw) {
        bool success = hw->setAllLeds(currentColors, CONTROLPAD_NUM_BUTTONS);
        if (!success) {
            // Don't block the main loop with delays - just log and continue
            Serial.printf("⏭️ Button %d highlight deferred - async LED system busy (non-blocking)\n", buttonIndex + 1);
            return;  // Will try again on next button event or poll cycle
        }
        
        // Debug: Show first few colors being sent to hardware
        Serial.printf("📤 %s: [0]=RGB(%d,%d,%d), [%d]=RGB(%d,%d,%d), [23]=RGB(%d,%d,%d)\n",
                     pressed ? "Immediate press" : "Coordinated release",
                     currentColors[0].r, currentColors[0].g, currentColors[0].b,
                     buttonIndex, currentColors[buttonIndex].r, currentColors[buttonIndex].g, currentColors[buttonIndex].b,
                     currentColors[23].r, currentColors[23].g, currentColors[23].b);
    }
    
    // Mark as clean since update was successfully queued
    ledsDirty = false;
}

void ControlPad::setButtonColor(uint8_t buttonIndex, const ControlPadColor& color) {
    if (buttonIndex >= CONTROLPAD_NUM_BUTTONS) return;
    
    // Direct assignment to base color
    baseColors[buttonIndex] = color;
    
    // Direct assignment to current color if not highlighted
    if (!buttonHighlighted[buttonIndex]) {
        currentColors[buttonIndex] = color;
        ledDirtyFlags[buttonIndex] = true;
        ledsDirty = true;
    }
    
    // Auto-update if enabled - but defer to prevent USB conflicts during button events
    // The update will happen during the next poll() cycle
}

void ControlPad::setAllButtonColors(const ControlPadColor* colors) {
    bool anyChanged = false;
    
    Serial.println("🌈 setAllButtonColors() called - updating base colors:");
    
    for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        if (baseColors[i].r != colors[i].r || 
            baseColors[i].g != colors[i].g || 
            baseColors[i].b != colors[i].b) {
            
            // Direct assignment to base color
            baseColors[i] = colors[i];
            
            Serial.printf("  Button %d: RGB(%d,%d,%d)\n", i + 1, colors[i].r, colors[i].g, colors[i].b);
            
            // Direct assignment to current color if not highlighted
            if (!buttonHighlighted[i]) {
                currentColors[i] = colors[i];
            }
            
            // Mark this specific LED as dirty
            ledDirtyFlags[i] = true;
            anyChanged = true;
        }
    }
    
    if (anyChanged) {
        ledsDirty = true;
        Serial.printf("✅ Updated %d button base colors\n", 
                     (int)std::count(ledDirtyFlags, ledDirtyFlags + CONTROLPAD_NUM_BUTTONS, true));
        
        // Auto-update if enabled - but defer to prevent USB conflicts during button events
        // The update will happen during the next poll() cycle
    } else {
        Serial.println("⚠️ No base color changes detected");
    }
}

void ControlPad::enableSmartUpdates(bool enable) {
    smartUpdatesEnabled = enable;
}

void ControlPad::setUpdateInterval(unsigned long intervalMs) {
    updateInterval = intervalMs;
}

void ControlPad::enableInstantUpdates(bool instant) {
    if (instant) {
        updateInterval = 10; // Very fast updates with new package structure
    } else {
        updateInterval = 30; // Standard safe rate limiting
    }
}

void ControlPad::forceUpdate() {
    Serial.printf("🚀 forceUpdate: Called - hw=%p, ledUpdateInProgress=%s\n", hw, ledUpdateInProgress ? "true" : "false");
    
    if (!hw || ledUpdateInProgress) {
        Serial.println("🚫 forceUpdate: Skipped (no hardware or update in progress)");
        return;
    }
    
    Serial.println("🚀 forceUpdate: Starting LED update...");
    
    // Prevent concurrent updates
    ledUpdateInProgress = true;
    
    // ATOMIC UPDATE: Build complete state first, then send all at once
    bool hasChanges = false;
    ControlPadColor tempState[CONTROLPAD_NUM_BUTTONS];
    
    // Copy current ledState as baseline
    for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        tempState[i] = ledState[i];
    }
    
    // Apply all dirty changes to temp state
    for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        if (ledDirtyFlags[i]) {
            tempState[i] = currentColors[i];
            hasChanges = true;
        }
    }
    
    Serial.printf("🎨 forceUpdate: hasChanges=%s, dirty count=%d\n", 
                 hasChanges ? "true" : "false",
                 (int)std::count(ledDirtyFlags, ledDirtyFlags + CONTROLPAD_NUM_BUTTONS, true));
    
    // DEBUG: Show first few colors being sent
    Serial.printf("🎨 forceUpdate: Colors[0]=RGB(%d,%d,%d), Colors[1]=RGB(%d,%d,%d)\n",
                 tempState[0].r, tempState[0].g, tempState[0].b,
                 tempState[1].r, tempState[1].g, tempState[1].b);
    
    // Send complete state to hardware atomically
    if (hasChanges) {
        Serial.printf("🎨 forceUpdate: Sending %d LED changes to hardware\n", 
                     (int)std::count(ledDirtyFlags, ledDirtyFlags + CONTROLPAD_NUM_BUTTONS, true));
        
        bool hwResult = hw->setAllLeds(tempState, CONTROLPAD_NUM_BUTTONS);
        Serial.printf("🎨 forceUpdate: Hardware setAllLeds result: %s\n", hwResult ? "SUCCESS" : "FAILED");
        
        // Only clear flags AFTER successful hardware update
        for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
            if (ledDirtyFlags[i]) {
                ledState[i] = tempState[i];  // Update our state record
                ledDirtyFlags[i] = false;     // Clear dirty flag
            }
        }
        lastUpdateTime = millis();
        Serial.println("✅ forceUpdate: LED update completed");
    } else {
        Serial.println("⚠️ forceUpdate: No changes to send");
    }
    
    ledsDirty = false;
    ledUpdateInProgress = false;  // Release the lock
}

void ControlPad::updateSmartLeds() {
    if (!ledsDirty || !hw || ledUpdateInProgress) {
        return;
    }
    
    // Enhanced rate limiting with USB activity detection
    unsigned long currentTime = millis();
    if (currentTime - lastUpdateTime < updateInterval) {
        return; // Too soon since last update - KEEP dirty flags intact
    }
    
    // ⚡ CRITICAL: Global USB quiet time tracking
    // Use a global static variable that persists across calls
    static unsigned long lastUSBActivityTime = 0;
    static bool needsQuietTime = false;
    
    // Require quiet period after button events before LED updates
    const unsigned long USB_QUIET_TIME_REQUIRED = 50; // Reduced to 50ms with new package structure
    
    // Only set quiet time requirement on FIRST call with dirty LEDs
    if (ledsDirty && !needsQuietTime) {
        lastUSBActivityTime = currentTime; // Mark when we first detected dirty LEDs
        needsQuietTime = true; // Flag that we need to wait for quiet time
        Serial.printf("🎯 First dirty LED detected - starting %lums quiet time countdown\n", (unsigned long)USB_QUIET_TIME_REQUIRED);
    }
    unsigned long timeSinceUSBActivity = currentTime - lastUSBActivityTime;
    
    // Check if we still need to wait for quiet time
    if (needsQuietTime && timeSinceUSBActivity < USB_QUIET_TIME_REQUIRED) {
        Serial.printf("⏳ LED update deferred: USB quiet time %lums/%lums\n", 
                     timeSinceUSBActivity, USB_QUIET_TIME_REQUIRED);
        return; // Keep dirty flags - will retry next poll cycle
    }
    
    // Quiet time elapsed - proceed with LED update
    needsQuietTime = false; // Reset the flag
    
    // Prevent concurrent updates
    ledUpdateInProgress = true;
    
    Serial.println("🎨 updateSmartLeds: Starting LED update after USB quiet period");
    
    // Brief pre-delay to ensure USB is quiet
    delay(10);
    
    // ATOMIC UPDATE: Build complete state first, then send all at once
    bool hasChanges = false;
    ControlPadColor tempState[CONTROLPAD_NUM_BUTTONS];
    
    // Copy current ledState as baseline
    for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        tempState[i] = ledState[i];
    }
    
    // Apply all dirty changes to temp state
    for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        if (ledDirtyFlags[i]) {
            tempState[i] = currentColors[i];
            hasChanges = true;
        }
    }
    
    // Send complete state to hardware atomically
    if (hasChanges) {
        Serial.printf("💡 Sending %d LED changes after quiet period\n", 
                     (int)std::count(ledDirtyFlags, ledDirtyFlags + CONTROLPAD_NUM_BUTTONS, true));
        hw->setAllLeds(tempState, CONTROLPAD_NUM_BUTTONS);
        
        // Brief post-delay to let LED communication settle
        delay(25);
        
        // Only clear flags AFTER successful hardware update
        for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
            if (ledDirtyFlags[i]) {
                ledState[i] = tempState[i];  // Update our state record
                ledDirtyFlags[i] = false;     // Clear dirty flag
            }
        }
        lastUpdateTime = currentTime;
        ledsDirty = false;  // Only clear after successful update
        Serial.println("✅ LED update completed after quiet period");
    }
    
    ledUpdateInProgress = false;  // Release the lock
}

bool ControlPad::hasLedChanges() {
    for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        if (ledState[i].r != currentColors[i].r ||
            ledState[i].g != currentColors[i].g ||
            ledState[i].b != currentColors[i].b) {
            return true;
        }
    }
    return false;
}

void ControlPad::markLedsClean() {
    ledsDirty = false;
}

// *** ANIMATION CONTROL METHODS ***
void ControlPad::enableAnimation() {
    if (hw) {
        hw->enableAnimation();
    }
}

void ControlPad::disableAnimation() {
    if (hw) {
        hw->disableAnimation();
    }
}

void ControlPad::updateAnimation() {
    if (hw) {
        hw->updateAnimation();
    }
}

void ControlPad::updateButtonHighlights() {
    if (hw) {
        hw->updateButtonHighlights();
    }
}

void ControlPad::updateUnifiedLEDs() {
    if (hw) {
        hw->updateUnifiedLEDs();
    }
}

bool ControlPad::isAnimationEnabled() const {
    if (hw) {
        return hw->isAnimationEnabled();
    }
    return false;
}

