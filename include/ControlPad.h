#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#define CONTROLPAD_NUM_BUTTONS 25

// Forward declaration
class ControlPadHardware;

struct ControlPadColor {
    uint8_t r, g, b;
};

// 1. Event type enum
enum class ControlPadEventType : uint8_t {
    Button,
    HallSensor,
    // Extend as needed
};

// 2. Event struct
struct ControlPadEvent {
    ControlPadEventType type;
    union {
        struct {
            uint8_t button;   // 0..24
            bool pressed;
        } button;
        struct {
            uint8_t sensor;   // 0..3
            int value;
        } hall;
    };
};

// 3. Callback type
typedef void (*ControlPadEventCallback)(const ControlPadEvent&);

class ControlPad {
public:
    ControlPad();
    ~ControlPad();

    bool begin();
    void poll();

    // Button and hall sensor state
    bool getButtonState(uint8_t button) const;
    int  getHallValue(uint8_t sensor) const;

    // LED control
    void setLed(uint8_t button, uint8_t r, uint8_t g, uint8_t b);
    void setAllLeds(const ControlPadColor colors[CONTROLPAD_NUM_BUTTONS]);
    void updateLeds();

    // Event API
    bool pollEvent(ControlPadEvent& event); // Polling
    void onEvent(ControlPadEventCallback cb); // Callback

    // (Optional) Event callbacks for button/hall changes
    // void onButtonChange(void (*callback)(uint8_t button, bool pressed));
    // void onHallChange(void (*callback)(uint8_t sensor, int value));

private:
    ControlPadColor ledState[CONTROLPAD_NUM_BUTTONS];
    bool buttonState[CONTROLPAD_NUM_BUTTONS];
    int hallValues[4];

    // Internal event queue
    static const size_t EVENT_QUEUE_SIZE = 16;
    ControlPadEvent eventQueue[EVENT_QUEUE_SIZE];
    size_t eventHead = 0, eventTail = 0;

    ControlPadEventCallback eventCallback = nullptr;

    // Hardware layer
    ControlPadHardware* hw = nullptr;

    // Internal: called by hardware layer
    void pushEvent(const ControlPadEvent& event);

    // Friend hardware layer if needed
    friend class ControlPadHardware;
}; 