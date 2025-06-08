#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#define CONTROLPAD_NUM_BUTTONS 24

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
    void setLed(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    void setLed(uint8_t index, const ControlPadColor& color);
    void updateLeds();
    
    // Smart LED management
    void setButtonHighlight(uint8_t buttonIndex, bool pressed);
    void setButtonColor(uint8_t buttonIndex, const ControlPadColor& color);
    void setAllButtonColors(const ControlPadColor* colors);
    void enableSmartUpdates(bool enable = true);
    void setUpdateInterval(unsigned long intervalMs);  // Configure update rate
    void enableInstantUpdates(bool instant = true);    // Disable all rate limiting
    void forceUpdate();
    
    // Convenience methods for common patterns
    void setAllLedsOff();
    void setAllLedsColor(uint8_t r, uint8_t g, uint8_t b);
    void setRainbowPattern();
    
    // Animation control
    void enableAnimation();
    void disableAnimation();
    void updateAnimation();
    void updateButtonHighlights();
    void updateUnifiedLEDs();
    bool isAnimationEnabled() const;
    
    // MIDI-timed LED system support
    ControlPadColor getCurrentLedColor(uint8_t index) const;
    bool isConnected() const;
    bool sendRawPacket(const uint8_t* data, size_t length);

    // Event API
    bool pollEvent(ControlPadEvent& event); // Polling
    void onEvent(ControlPadEventCallback cb); // Callback
    
    // Internal: called by hardware layer (made public for USB callbacks)
    void pushEvent(const ControlPadEvent& event);

    // (Optional) Event callbacks for button/hall changes
    // void onButtonChange(void (*callback)(uint8_t button, bool pressed));
    // void onHallChange(void (*callback)(uint8_t sensor, int value));

private:
    ControlPadColor ledState[CONTROLPAD_NUM_BUTTONS];
    bool buttonState[CONTROLPAD_NUM_BUTTONS];
    int hallValues[4];

    // Internal event queue
    static const size_t EVENT_QUEUE_SIZE = 16;  // Increased from 2 to handle rapid button events
    ControlPadEvent eventQueue[EVENT_QUEUE_SIZE];
    size_t eventHead = 0, eventTail = 0;
    
    // Smart LED management system
    ControlPadColor baseColors[CONTROLPAD_NUM_BUTTONS];    // Original button colors
    ControlPadColor currentColors[CONTROLPAD_NUM_BUTTONS]; // Current LED state
    bool buttonHighlighted[CONTROLPAD_NUM_BUTTONS];        // Which buttons are highlighted
    bool ledsDirty;                                        // Whether LEDs need updating
    bool ledDirtyFlags[CONTROLPAD_NUM_BUTTONS];           // Per-LED dirty tracking
    bool smartUpdatesEnabled;                              // Enable automatic smart updates
    bool ledUpdateInProgress;                              // Prevent concurrent updates
    unsigned long lastUpdateTime;                          // Last LED update timestamp
    unsigned long lastButtonTime[CONTROLPAD_NUM_BUTTONS]; // Debounce timing per button
    unsigned long updateInterval;                          // Minimum time between updates
    
    // Internal smart LED methods
    void updateSmartLeds();
    bool hasLedChanges();
    void markLedsClean();

    ControlPadEventCallback eventCallback = nullptr;

    // Hardware layer
    ControlPadHardware* hw = nullptr;

    // Friend hardware layer if needed
    friend class ControlPadHardware;
}; 