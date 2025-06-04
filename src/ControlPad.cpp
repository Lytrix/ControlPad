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
    }
}

void ControlPad::setAllLeds(const ControlPadColor colors[CONTROLPAD_NUM_BUTTONS]) {
    for (int i = 0; i < CONTROLPAD_NUM_BUTTONS; ++i) {
        ledState[i] = colors[i];
    }
}

void ControlPad::updateLeds() {
    hw->setAllLeds(ledState, CONTROLPAD_NUM_BUTTONS);
}

// Event API implementation
bool ControlPad::pollEvent(ControlPadEvent& event) {
    // Check if there are events in the queue
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
    // Add event to queue
    size_t nextHead = (eventHead + 1) % EVENT_QUEUE_SIZE;
    if (nextHead != eventTail) { // Queue not full
        eventQueue[eventHead] = event;
        eventHead = nextHead;
        
        // If callback is set, call it immediately
        if (eventCallback) {
            eventCallback(event);
        }
    }
    // If queue is full, event is dropped (could add logging here)
}

