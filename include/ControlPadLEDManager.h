#pragma once

#include <Arduino.h>
#include <functional>

// Forward declare the USB driver
class USBControlPad;

// MIDI Looper Track States (adapt to match your TrackState enum)
enum class TrackState {
    EMPTY = 0,
    RECORDING,
    STOPPED_RECORDING,
    PLAYING,
    OVERDUBBING,
    MUTED
};

// LED Animation Types for different states
enum class LEDAnimation {
    SOLID,
    PULSE_FAST,      // Recording/measure sync
    PULSE_SLOW,      // Standby/ready
    BREATHE,         // Muted
    RAINBOW,         // Playing with effects
    STROBE,          // Error/warning
    FADE_IN,         // Transitioning in
    FADE_OUT         // Transitioning out
};

// Color definitions for MIDI looper states
struct LEDColor {
    uint8_t r, g, b;
    
    LEDColor(uint8_t red = 0, uint8_t green = 0, uint8_t blue = 0) 
        : r(red), g(green), b(blue) {}
    
    // Predefined colors for MIDI looper
    static const LEDColor BLACK;
    static const LEDColor WHITE;
    static const LEDColor RED;
    static const LEDColor GREEN;
    static const LEDColor BLUE;
    static const LEDColor YELLOW;
    static const LEDColor PURPLE;
    static const LEDColor ORANGE;
    static const LEDColor CYAN;
    
    // MIDI looper state colors
    static const LEDColor TRACK_EMPTY;
    static const LEDColor TRACK_RECORDING;
    static const LEDColor TRACK_PLAYING;
    static const LEDColor TRACK_OVERDUBBING;
    static const LEDColor TRACK_MUTED;
    static const LEDColor RECORD_BUTTON;
    static const LEDColor MEASURE_PULSE;
};

// Button layout for 5x5 ControlPad (matching your track system)
class ControlPadLayout {
public:
    // Track buttons (first 4 buttons for your 4 tracks)
    static constexpr uint8_t TRACK_1 = 1;
    static constexpr uint8_t TRACK_2 = 2;
    static constexpr uint8_t TRACK_3 = 3;
    static constexpr uint8_t TRACK_4 = 4;
    
    // Control buttons
    static constexpr uint8_t RECORD_BUTTON = 24;  // As requested
    static constexpr uint8_t PLAY_BUTTON = 25;    // Center button (no LED)
    static constexpr uint8_t STOP_BUTTON = 5;
    static constexpr uint8_t CLEAR_BUTTON = 10;
    
    // Future clip buttons (for multiple clips per track)
    static constexpr uint8_t TRACK_1_CLIPS[] = {6, 11, 16, 21};   // Column 1
    static constexpr uint8_t TRACK_2_CLIPS[] = {7, 12, 17, 22};   // Column 2
    static constexpr uint8_t TRACK_3_CLIPS[] = {8, 13, 18, 23};   // Column 3
    static constexpr uint8_t TRACK_4_CLIPS[] = {9, 14, 19, 24};   // Column 4
    
    // Utility functions
    static bool isTrackButton(uint8_t button) {
        return button >= 1 && button <= 4;
    }
    
    static uint8_t getTrackFromButton(uint8_t button) {
        return (button >= 1 && button <= 4) ? button - 1 : 255;
    }
};

// LED Animation State for timing-based effects
struct AnimationState {
    LEDAnimation type = LEDAnimation::SOLID;
    LEDColor baseColor;
    LEDColor pulseColor;
    uint32_t lastUpdate = 0;
    uint32_t interval = 500;     // Animation interval in ms
    uint8_t phase = 0;           // Animation phase (0-255)
    bool direction = true;       // For breathing/fade effects
    
    AnimationState(LEDAnimation anim = LEDAnimation::SOLID, 
                   LEDColor base = LEDColor::BLACK,
                   LEDColor pulse = LEDColor::WHITE)
        : type(anim), baseColor(base), pulseColor(pulse) {}
};

class ControlPadLEDManager {
private:
    USBControlPad* controlPad = nullptr;
    
    // Current LED state for all 24 buttons
    LEDColor currentState[24];
    AnimationState animations[24];
    
    // Timing for measure-sync pulsing
    uint32_t lastMeasurePulse = 0;
    uint32_t measureLength = 1000;  // ms per measure
    uint8_t measuresPerBar = 4;
    
    // Track states for the 4 MIDI tracks
    TrackState trackStates[4] = {TrackState::EMPTY, TrackState::EMPTY, 
                                 TrackState::EMPTY, TrackState::EMPTY};
    
    // Update timing
    uint32_t lastUpdate = 0;
    static constexpr uint32_t UPDATE_INTERVAL = 50; // 20 FPS
    
    // Internal helper methods
    LEDColor calculateAnimatedColor(uint8_t buttonIndex);
    void updateAnimations();
    void applyCompleteState();
    
public:
    ControlPadLEDManager() = default;
    
    // Initialization
    bool initialize(USBControlPad* controlPadDriver);
    void update(); // Call this in your main loop
    
    // === MIDI LOOPER INTEGRATION ===
    
    // Track state management
    void setTrackState(uint8_t trackIndex, TrackState state);
    void setAllTrackStates(TrackState states[4]);
    TrackState getTrackState(uint8_t trackIndex) const;
    
    // Record button with measure sync
    void setRecordButtonState(bool recording, uint32_t measureLengthMs = 1000);
    void updateMeasurePulse(); // Call on each measure beat
    
    // Individual button control
    void setButtonColor(uint8_t buttonIndex, LEDColor color, LEDAnimation animation = LEDAnimation::SOLID);
    void setButtonAnimation(uint8_t buttonIndex, LEDAnimation animation, LEDColor baseColor, LEDColor pulseColor = LEDColor::WHITE);
    
    // Bulk operations for performance
    void setTrackRowColors(uint8_t trackIndex, LEDColor colors[5]); // Track + 4 clips
    void clearAllButtons();
    void setBaselinePattern(); // Beautiful baseline when inactive
    
    // === ADVANCED FEATURES ===
    
    // Multi-clip support (future)
    void setClipState(uint8_t trackIndex, uint8_t clipIndex, TrackState state);
    void highlightActiveClips(uint8_t trackMask); // Bitmask of active tracks
    
    // Visual feedback
    void flashButton(uint8_t buttonIndex, LEDColor color, uint32_t durationMs = 200);
    void showStartupAnimation();
    void showErrorPattern();
    
    // Timing integration
    void setMeasureLength(uint32_t lengthMs) { measureLength = lengthMs; }
    void setBPM(float bpm) { measureLength = (uint32_t)(60000.0f / bpm); }
    
    // Manual LED control (bypass state machine)
    void setRawButtonColor(uint8_t buttonIndex, uint8_t r, uint8_t g, uint8_t b);
    void setRawPattern(LEDColor colors[24]);
    
    // State queries
    bool isRecording() const;
    uint8_t getActiveTrackCount() const;
    uint32_t getLastUpdateTime() const { return lastUpdate; }
    
    // === HELPER FUNCTIONS FOR INTEGRATION ===
    
    // State change callbacks (call these from your MIDI looper)
    void onTrackStartRecording(uint8_t trackIndex);
    void onTrackStopRecording(uint8_t trackIndex);
    void onTrackStartPlaying(uint8_t trackIndex);
    void onTrackStartOverdubbing(uint8_t trackIndex);
    void onTrackMute(uint8_t trackIndex);
    void onTrackClear(uint8_t trackIndex);
    
    // Measure/beat sync (call from your ClockManager)
    void onMeasureBeat(uint8_t beatNumber); // 0-3 for 4/4 time
    void onBarComplete();
    
    // Button press integration (call from your ButtonManager)
    void onButtonPressed(uint8_t buttonIndex);
    void onButtonReleased(uint8_t buttonIndex);
    
    // Pattern presets
    void applyLooperPattern();     // Standard 4-track layout
    void applyPerformancePattern(); // Optimized for live use
    void applyStudioPattern();     // All features visible
};

// === INTEGRATION MACROS ===

// Easy state updates from your MIDI looper
#define UPDATE_TRACK_LED(manager, track, state) \
    manager.setTrackState(track, TrackState::state)

#define PULSE_RECORD_BUTTON(manager, bpm) \
    manager.setBPM(bpm); \
    manager.setRecordButtonState(true)

#define FLASH_BUTTON_ON_ACTION(manager, button, color) \
    manager.flashButton(button, LEDColor::color)

// === STATIC HELPER FUNCTIONS ===

namespace ControlPadHelpers {
    // Color blending for smooth transitions
    LEDColor blendColors(const LEDColor& a, const LEDColor& b, float ratio);
    
    // HSV to RGB conversion for rainbow effects
    LEDColor hsvToRgb(float h, float s, float v);
    
    // Brightness adjustment
    LEDColor adjustBrightness(const LEDColor& color, float brightness);
    
    // Color wheel for dynamic effects
    LEDColor colorWheel(uint8_t position);
    
    // Timing utilities
    uint8_t calculatePulsePhase(uint32_t currentTime, uint32_t interval);
    bool isTimeForUpdate(uint32_t lastUpdate, uint32_t interval);
} 