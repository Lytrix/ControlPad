#include "ControlPadLEDManager.h"
#include "main.h" // For USBControlPad class

// === LED COLOR DEFINITIONS ===

const LEDColor LEDColor::BLACK(0, 0, 0);
const LEDColor LEDColor::WHITE(255, 255, 255);
const LEDColor LEDColor::RED(255, 0, 0);
const LEDColor LEDColor::GREEN(0, 255, 0);
const LEDColor LEDColor::BLUE(0, 0, 255);
const LEDColor LEDColor::YELLOW(255, 255, 0);
const LEDColor LEDColor::PURPLE(128, 0, 128);
const LEDColor LEDColor::ORANGE(255, 165, 0);
const LEDColor LEDColor::CYAN(0, 255, 255);

// MIDI Looper specific colors
const LEDColor LEDColor::TRACK_EMPTY(20, 20, 20);        // Dim white
const LEDColor LEDColor::TRACK_RECORDING(255, 0, 0);     // Bright red
const LEDColor LEDColor::TRACK_PLAYING(0, 255, 0);       // Bright green
const LEDColor LEDColor::TRACK_OVERDUBBING(255, 255, 0); // Yellow
const LEDColor LEDColor::TRACK_MUTED(128, 128, 128);     // Gray
const LEDColor LEDColor::RECORD_BUTTON(255, 100, 100);   // Pulsing red
const LEDColor LEDColor::MEASURE_PULSE(255, 255, 255);   // White flash

// === CONTROLPAD LED MANAGER IMPLEMENTATION ===

bool ControlPadLEDManager::initialize(USBControlPad* controlPadDriver) {
    if (!controlPadDriver) {
        Serial.println("❌ ControlPadLEDManager: No driver provided");
        return false;
    }
    
    controlPad = controlPadDriver;
    
    // Initialize all buttons to baseline pattern
    setBaselinePattern();
    
    Serial.println("✅ ControlPadLEDManager initialized successfully");
    return true;
}

void ControlPadLEDManager::update() {
    uint32_t now = millis();
    
    if (now - lastUpdate < UPDATE_INTERVAL) {
        return; // Not time for update yet
    }
    
    updateAnimations();
    applyCompleteState();
    
    lastUpdate = now;
}

void ControlPadLEDManager::setTrackState(uint8_t trackIndex, TrackState state) {
    if (trackIndex >= 4) return;
    
    trackStates[trackIndex] = state;
    
    // Update the track button LED based on state
    LEDColor color;
    LEDAnimation animation = LEDAnimation::SOLID;
    
    switch (state) {
        case TrackState::EMPTY:
            color = LEDColor::TRACK_EMPTY;
            animation = LEDAnimation::PULSE_SLOW;
            break;
        case TrackState::RECORDING:
            color = LEDColor::TRACK_RECORDING;
            animation = LEDAnimation::PULSE_FAST;
            break;
        case TrackState::PLAYING:
            color = LEDColor::TRACK_PLAYING;
            animation = LEDAnimation::SOLID;
            break;
        case TrackState::OVERDUBBING:
            color = LEDColor::TRACK_OVERDUBBING;
            animation = LEDAnimation::PULSE_FAST;
            break;
        case TrackState::MUTED:
            color = LEDColor::TRACK_MUTED;
            animation = LEDAnimation::BREATHE;
            break;
        default:
            color = LEDColor::BLACK;
            break;
    }
    
    uint8_t buttonIndex = trackIndex; // Buttons 1-4 map to tracks 0-3
    setButtonAnimation(buttonIndex, animation, color, LEDColor::WHITE);
    
    Serial.printf("🎵 Track %d state: %d, LED color: RGB(%d,%d,%d)\n", 
                  trackIndex, (int)state, color.r, color.g, color.b);
}

void ControlPadLEDManager::setAllTrackStates(TrackState states[4]) {
    for (uint8_t i = 0; i < 4; i++) {
        setTrackState(i, states[i]);
    }
}

void ControlPadLEDManager::setRecordButtonState(bool recording, uint32_t measureLengthMs) {
    measureLength = measureLengthMs;
    
    uint8_t recordBtn = ControlPadLayout::RECORD_BUTTON - 1; // Convert to 0-based
    
    if (recording) {
        // Pulsing red synchronized with measures
        setButtonAnimation(recordBtn, LEDAnimation::PULSE_FAST, 
                          LEDColor::RECORD_BUTTON, LEDColor::WHITE);
        animations[recordBtn].interval = measureLength / 4; // Pulse 4 times per measure
    } else {
        // Solid dim red when not recording
        setButtonColor(recordBtn, LEDColor(80, 20, 20));
    }
}

void ControlPadLEDManager::setButtonColor(uint8_t buttonIndex, LEDColor color, LEDAnimation animation) {
    if (buttonIndex >= 24) return;
    
    currentState[buttonIndex] = color;
    animations[buttonIndex].type = animation;
    animations[buttonIndex].baseColor = color;
    animations[buttonIndex].lastUpdate = millis();
}

void ControlPadLEDManager::setButtonAnimation(uint8_t buttonIndex, LEDAnimation animation, 
                                             LEDColor baseColor, LEDColor pulseColor) {
    if (buttonIndex >= 24) return;
    
    animations[buttonIndex].type = animation;
    animations[buttonIndex].baseColor = baseColor;
    animations[buttonIndex].pulseColor = pulseColor;
    animations[buttonIndex].lastUpdate = millis();
    animations[buttonIndex].phase = 0;
    animations[buttonIndex].direction = true;
    
    // Set animation intervals based on type
    switch (animation) {
        case LEDAnimation::PULSE_FAST:
            animations[buttonIndex].interval = 250; // 4 Hz
            break;
        case LEDAnimation::PULSE_SLOW:
            animations[buttonIndex].interval = 1000; // 1 Hz
            break;
        case LEDAnimation::BREATHE:
            animations[buttonIndex].interval = 2000; // 0.5 Hz
            break;
        case LEDAnimation::STROBE:
            animations[buttonIndex].interval = 100; // 10 Hz
            break;
        default:
            animations[buttonIndex].interval = 500;
            break;
    }
}

void ControlPadLEDManager::setBaselinePattern() {
    // Beautiful baseline pattern based on your successful reverse engineering
    LEDColor baselineColors[5] = {
        LEDColor(251, 252, 253),  // fbfcfd
        LEDColor(201, 202, 203),  // c9cacb  
        LEDColor(151, 152, 153),  // 979899
        LEDColor(101, 102, 103),  // 656667
        LEDColor(51, 52, 53)      // 333435
    };
    
    // Apply gradient pattern across the 5x5 grid
    for (uint8_t i = 0; i < 24; i++) {
        uint8_t colorIndex = i % 5;
        currentState[i] = baselineColors[colorIndex];
        animations[i].type = LEDAnimation::SOLID;
    }
    
    Serial.println("🌈 Applied beautiful baseline LED pattern");
}

LEDColor ControlPadLEDManager::calculateAnimatedColor(uint8_t buttonIndex) {
    if (buttonIndex >= 24) return LEDColor::BLACK;
    
    AnimationState& anim = animations[buttonIndex];
    uint32_t now = millis();
    
    switch (anim.type) {
        case LEDAnimation::SOLID:
            return anim.baseColor;
            
        case LEDAnimation::PULSE_FAST:
        case LEDAnimation::PULSE_SLOW: {
            uint32_t elapsed = now - anim.lastUpdate;
            float phase = (float)(elapsed % anim.interval) / anim.interval;
            float intensity = (sin(phase * 2 * PI) + 1.0f) / 2.0f; // 0.0 to 1.0
            return ControlPadHelpers::blendColors(anim.baseColor, anim.pulseColor, intensity);
        }
        
        case LEDAnimation::BREATHE: {
            uint32_t elapsed = now - anim.lastUpdate;
            float phase = (float)(elapsed % anim.interval) / anim.interval;
            float intensity = (sin(phase * PI) + 1.0f) / 2.0f; // Smooth breathing
            return ControlPadHelpers::adjustBrightness(anim.baseColor, intensity);
        }
        
        case LEDAnimation::RAINBOW: {
            uint32_t elapsed = now - anim.lastUpdate;
            float hue = (float)(elapsed % anim.interval) / anim.interval * 360.0f;
            return ControlPadHelpers::hsvToRgb(hue, 1.0f, 1.0f);
        }
        
        case LEDAnimation::STROBE: {
            uint32_t elapsed = now - anim.lastUpdate;
            bool on = (elapsed % anim.interval) < (anim.interval / 2);
            return on ? anim.pulseColor : anim.baseColor;
        }
        
        case LEDAnimation::FADE_IN:
        case LEDAnimation::FADE_OUT:
        default:
            return anim.baseColor;
    }
}

void ControlPadLEDManager::updateAnimations() {
    for (uint8_t i = 0; i < 24; i++) {
        currentState[i] = calculateAnimatedColor(i);
    }
}

void ControlPadLEDManager::applyCompleteState() {
    if (!controlPad) return;
    
    // Use your proven 5-command LED protocol
    // This calls the working setCompleteButtonState method from your driver
    
    // Convert our currentState array to the format expected by your driver
    uint8_t colorArray[24][3];
    for (uint8_t i = 0; i < 24; i++) {
        colorArray[i][0] = currentState[i].r;
        colorArray[i][1] = currentState[i].g;
        colorArray[i][2] = currentState[i].b;
    }
    
    // Call your working LED update method
    controlPad->setCompleteButtonState(colorArray);
}

// === MIDI LOOPER INTEGRATION CALLBACKS ===

void ControlPadLEDManager::onTrackStartRecording(uint8_t trackIndex) {
    setTrackState(trackIndex, TrackState::RECORDING);
    flashButton(trackIndex, LEDColor::RED, 300);
}

void ControlPadLEDManager::onTrackStopRecording(uint8_t trackIndex) {
    setTrackState(trackIndex, TrackState::PLAYING);
    flashButton(trackIndex, LEDColor::GREEN, 300);
}

void ControlPadLEDManager::onTrackStartPlaying(uint8_t trackIndex) {
    setTrackState(trackIndex, TrackState::PLAYING);
}

void ControlPadLEDManager::onTrackStartOverdubbing(uint8_t trackIndex) {
    setTrackState(trackIndex, TrackState::OVERDUBBING);
    flashButton(trackIndex, LEDColor::YELLOW, 200);
}

void ControlPadLEDManager::onTrackMute(uint8_t trackIndex) {
    setTrackState(trackIndex, TrackState::MUTED);
}

void ControlPadLEDManager::onTrackClear(uint8_t trackIndex) {
    setTrackState(trackIndex, TrackState::EMPTY);
    flashButton(trackIndex, LEDColor::WHITE, 500);
}

void ControlPadLEDManager::onMeasureBeat(uint8_t beatNumber) {
    // Flash the record button on beat 1
    if (beatNumber == 0) {
        uint8_t recordBtn = ControlPadLayout::RECORD_BUTTON - 1;
        flashButton(recordBtn, LEDColor::MEASURE_PULSE, 100);
    }
}

void ControlPadLEDManager::flashButton(uint8_t buttonIndex, LEDColor color, uint32_t durationMs) {
    if (buttonIndex >= 24) return;
    
    // Store current state
    LEDColor originalColor = currentState[buttonIndex];
    LEDAnimation originalAnim = animations[buttonIndex].type;
    
    // Set flash color
    setButtonColor(buttonIndex, color, LEDAnimation::SOLID);
    
    // TODO: Implement timer-based restoration of original state
    // For now, this provides immediate visual feedback
}

void ControlPadLEDManager::showStartupAnimation() {
    Serial.println("🎬 Starting ControlPad startup animation...");
    
    // Sweep across all buttons
    for (uint8_t i = 0; i < 24; i++) {
        setButtonColor(i, LEDColor::CYAN);
        applyCompleteState();
        delay(50);
        setButtonColor(i, LEDColor::BLACK);
    }
    
    // Final pattern
    setBaselinePattern();
    applyCompleteState();
    
    Serial.println("✅ Startup animation complete");
}

// === HELPER FUNCTION IMPLEMENTATIONS ===

namespace ControlPadHelpers {
    
    LEDColor blendColors(const LEDColor& a, const LEDColor& b, float ratio) {
        ratio = constrain(ratio, 0.0f, 1.0f);
        return LEDColor(
            a.r + (b.r - a.r) * ratio,
            a.g + (b.g - a.g) * ratio,
            a.b + (b.b - a.b) * ratio
        );
    }
    
    LEDColor hsvToRgb(float h, float s, float v) {
        float c = v * s;
        float x = c * (1 - abs(fmod(h / 60.0f, 2) - 1));
        float m = v - c;
        
        float r, g, b;
        
        if (h >= 0 && h < 60) {
            r = c; g = x; b = 0;
        } else if (h >= 60 && h < 120) {
            r = x; g = c; b = 0;
        } else if (h >= 120 && h < 180) {
            r = 0; g = c; b = x;
        } else if (h >= 180 && h < 240) {
            r = 0; g = x; b = c;
        } else if (h >= 240 && h < 300) {
            r = x; g = 0; b = c;
        } else {
            r = c; g = 0; b = x;
        }
        
        return LEDColor(
            (r + m) * 255,
            (g + m) * 255,
            (b + m) * 255
        );
    }
    
    LEDColor adjustBrightness(const LEDColor& color, float brightness) {
        brightness = constrain(brightness, 0.0f, 1.0f);
        return LEDColor(
            color.r * brightness,
            color.g * brightness,
            color.b * brightness
        );
    }
    
    LEDColor colorWheel(uint8_t position) {
        if (position < 85) {
            return LEDColor(position * 3, 255 - position * 3, 0);
        } else if (position < 170) {
            position -= 85;
            return LEDColor(255 - position * 3, 0, position * 3);
        } else {
            position -= 170;
            return LEDColor(0, position * 3, 255 - position * 3);
        }
    }
    
    uint8_t calculatePulsePhase(uint32_t currentTime, uint32_t interval) {
        return (uint8_t)((currentTime % interval) * 255 / interval);
    }
    
    bool isTimeForUpdate(uint32_t lastUpdate, uint32_t interval) {
        return (millis() - lastUpdate) >= interval;
    }
} 