#include "ControlPad.h"
#include "ControlPadHardware.h"

ControlPadHardware* hw;

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
    updateInterval = 50;  // Use 50ms intervals to match USB ACK timing and prevent overflow
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
    if (hw) {
        hw->poll();
        
        // Auto-update LEDs if smart updates are enabled and changes are pending
        if (smartUpdatesEnabled && ledsDirty) {
            updateSmartLeds();
        }
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
    hw->setAllLeds(ledState, CONTROLPAD_NUM_BUTTONS);
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
    // Serial.printf("🔍 ControlPad::pollEvent() called - Head=%zu, Tail=%zu\n", eventHead, eventTail);
    
    // Check if there are events in the queue
    if (eventHead != eventTail) {
        event = eventQueue[eventTail];
        eventTail = (eventTail + 1) % EVENT_QUEUE_SIZE;
        // Serial.printf("📥 ControlPad::pollEvent() - Found event! Head=%zu, Tail=%zu\n", eventHead, eventTail);
        return true;
    } else {
        // Serial.printf("📭 ControlPad::pollEvent() - No events (Head=%zu == Tail=%zu)\n", eventHead, eventTail);
    }
    return false;
}

void ControlPad::onEvent(ControlPadEventCallback cb) {
    eventCallback = cb;
}

// Internal: called by hardware layer
void ControlPad::pushEvent(const ControlPadEvent& event) {
    // Add event to queue
    size_t nextHead = (eventHead + 1) % EVENT_QUEUE_SIZE;
    if (nextHead != eventTail) { // Queue not full
        eventQueue[eventHead] = event;
        eventHead = nextHead;
        // Serial.printf("📤 ControlPad::pushEvent() - Event added! Head=%zu, Tail=%zu, Type=%d\n", 
        //              eventHead, eventTail, (int)event.type);
        
        // If callback is set, call it immediately
        if (eventCallback) {
            eventCallback(event);
        }
    } else {
        // Serial.println("❌ ControlPad::pushEvent() - Queue FULL! Event dropped!");
    }
    // If queue is full, event is dropped (could add logging here)
}

// Smart LED management implementation
void ControlPad::setButtonHighlight(uint8_t buttonIndex, bool pressed) {
    if (buttonIndex >= CONTROLPAD_NUM_BUTTONS) return;
    
    // SIMPLE DEBOUNCING: Prevent button bounce
    unsigned long currentTime = millis();
    if (currentTime - lastButtonTime[buttonIndex] < 50) {  // 50ms debounce
        return; // Skip bounce events
    }
    lastButtonTime[buttonIndex] = currentTime;
    
    if (buttonHighlighted[buttonIndex] != pressed) {
        buttonHighlighted[buttonIndex] = pressed;
        
        // DIRECT ASSIGNMENT - No intermediate calculations
        if (pressed) {
            // Direct white highlight assignment
            currentColors[buttonIndex] = {255, 255, 255};
        } else {
            // Direct base color restore assignment  
            currentColors[buttonIndex] = baseColors[buttonIndex];
        }
        
        // Mark only this specific LED as dirty
        ledDirtyFlags[buttonIndex] = true;
        ledsDirty = true;
        
        // IMMEDIATE UPDATE: Update LEDs right away for responsive feedback
        if (smartUpdatesEnabled) {
            updateSmartLeds();
        }
    }
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
    
    // Auto-update if enabled - use force update to bypass settle time for single color changes
    if (smartUpdatesEnabled) {
        forceUpdate();  // Single color changes should be immediate
    }
}

void ControlPad::setAllButtonColors(const ControlPadColor* colors) {
    bool anyChanged = false;
    
    for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        if (baseColors[i].r != colors[i].r || 
            baseColors[i].g != colors[i].g || 
            baseColors[i].b != colors[i].b) {
            
            // Direct assignment to base color
            baseColors[i] = colors[i];
            
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
        
        // Auto-update if enabled - use force update to bypass settle time for bulk changes
        if (smartUpdatesEnabled) {
            forceUpdate();  // Bulk color changes should be immediate
        }
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
        updateInterval = 25; // Faster updates but still safe for USB ACK timing
    } else {
        updateInterval = 50; // Standard safe rate limiting
    }
}

void ControlPad::forceUpdate() {
    if (!hw || ledUpdateInProgress) return;
    
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
    
    // Send complete state to hardware atomically
    if (hasChanges) {
        hw->setAllLeds(tempState, CONTROLPAD_NUM_BUTTONS);
        
        // Only clear flags AFTER successful hardware update
        for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
            if (ledDirtyFlags[i]) {
                ledState[i] = tempState[i];  // Update our state record
                ledDirtyFlags[i] = false;     // Clear dirty flag
            }
        }
        lastUpdateTime = millis();
    }
    
    ledsDirty = false;
    ledUpdateInProgress = false;  // Release the lock
}

void ControlPad::updateSmartLeds() {
    if (!ledsDirty || !hw || ledUpdateInProgress) {
        return;
    }
    
    // Standard rate limiting
    unsigned long currentTime = millis();
    if (currentTime - lastUpdateTime < updateInterval) {
        return; // Too soon since last update - KEEP dirty flags intact
    }
    
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
    
    // Send complete state to hardware atomically
    if (hasChanges) {
        hw->setAllLeds(tempState, CONTROLPAD_NUM_BUTTONS);
        
        // Only clear flags AFTER successful hardware update
        for (uint8_t i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
            if (ledDirtyFlags[i]) {
                ledState[i] = tempState[i];  // Update our state record
                ledDirtyFlags[i] = false;     // Clear dirty flag
            }
        }
        lastUpdateTime = currentTime;
        ledsDirty = false;  // Only clear after successful update
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

