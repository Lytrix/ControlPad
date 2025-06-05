#include "ControlPad.h"
#include "ControlPadHardware.h"

ControlPadHardware* hw;

ControlPad::ControlPad() {
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledState[i] = {0, 0, 0};
        buttonState[i] = false;
    }
    for (int i = 0; i < 4; ++i) {
        hallValues[i] = 0;
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
    hw->poll(); 
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

void ControlPad::setAllLeds(const ControlPadColor colors[CONTROLPAD_NUM_BUTTONS]) {
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledState[i] = colors[i];
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

