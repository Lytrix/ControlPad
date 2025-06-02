/*
 * MIDI Looper ControlPad Integration Example
 * 
 * This example shows how to integrate the ControlPad LED control system
 * with your sophisticated MIDI looper project.
 * 
 * Based on: https://github.com/lytrix/midilooper
 */

#include <Arduino.h>
#include "ControlPadLEDManager.h"
#include "main.h" // Your existing ControlPad driver

// Global instances (adapt to your existing architecture)
extern USBControlPad* controlPadDriver;  // Your existing driver
ControlPadLEDManager ledManager;

// MIDI Looper state simulation (replace with your actual classes)
struct MidiLooperState {
    TrackState tracks[4] = {TrackState::EMPTY, TrackState::EMPTY, 
                           TrackState::EMPTY, TrackState::EMPTY};
    bool isRecording = false;
    float currentBPM = 120.0f;
    uint8_t currentMeasure = 0;
    uint8_t currentBeat = 0;
    uint32_t lastBeatTime = 0;
    
    // Button states from your ButtonManager
    bool buttonPressed[25] = {false};
    uint32_t buttonPressTime[25] = {0};
};

MidiLooperState looperState;

// === INTEGRATION SETUP ===

void setupControlPadIntegration() {
    Serial.println("🎛️ Setting up ControlPad LED integration for MIDI Looper...");
    
    // Wait for your existing ControlPad driver to initialize
    while (!controlPadDriver || !controlPadDriver->isInitialized()) {
        delay(100);
        Serial.println("⏳ Waiting for ControlPad driver...");
    }
    
    // Initialize the LED manager
    if (!ledManager.initialize(controlPadDriver)) {
        Serial.println("❌ Failed to initialize LED Manager");
        return;
    }
    
    // Show startup animation
    ledManager.showStartupAnimation();
    
    // Set initial BPM for record button pulsing
    ledManager.setBPM(looperState.currentBPM);
    
    // Apply the baseline looper pattern
    ledManager.applyLooperPattern();
    
    Serial.println("✅ ControlPad LED integration setup complete!");
}

// === MIDI LOOPER STATE INTEGRATION ===

void updateLooperLEDs() {
    // Update track states
    for (uint8_t i = 0; i < 4; i++) {
        ledManager.setTrackState(i, looperState.tracks[i]);
    }
    
    // Update record button based on recording state
    ledManager.setRecordButtonState(looperState.isRecording, 
                                   (uint32_t)(60000.0f / looperState.currentBPM));
    
    // Call update to handle animations
    ledManager.update();
}

// === BUTTON PRESS INTEGRATION ===

void handleControlPadButtonPress(uint8_t buttonIndex) {
    Serial.printf("🎹 ControlPad Button %d pressed\n", buttonIndex + 1);
    
    // Convert to 1-based button number for layout
    uint8_t buttonNumber = buttonIndex + 1;
    
    // Handle track buttons (1-4)
    if (ControlPadLayout::isTrackButton(buttonNumber)) {
        uint8_t trackIndex = ControlPadLayout::getTrackFromButton(buttonNumber);
        handleTrackButtonPress(trackIndex);
        return;
    }
    
    // Handle control buttons
    switch (buttonNumber) {
        case ControlPadLayout::RECORD_BUTTON: // Button 24
            handleRecordButtonPress();
            break;
            
        case ControlPadLayout::STOP_BUTTON: // Button 5
            handleStopButtonPress();
            break;
            
        case ControlPadLayout::CLEAR_BUTTON: // Button 10
            handleClearButtonPress();
            break;
            
        default:
            // Future clip buttons or other controls
            Serial.printf("🔄 Button %d - Future feature\n", buttonNumber);
            ledManager.flashButton(buttonIndex, LEDColor::BLUE, 200);
            break;
    }
}

void handleTrackButtonPress(uint8_t trackIndex) {
    if (trackIndex >= 4) return;
    
    TrackState currentState = looperState.tracks[trackIndex];
    
    switch (currentState) {
        case TrackState::EMPTY:
            // Start recording on empty track
            startTrackRecording(trackIndex);
            break;
            
        case TrackState::PLAYING:
            // Start overdubbing
            startTrackOverdubbing(trackIndex);
            break;
            
        case TrackState::OVERDUBBING:
            // Stop overdubbing, back to playing
            stopTrackOverdubbing(trackIndex);
            break;
            
        case TrackState::RECORDING:
            // Can't press track button while recording (wait for quantization)
            ledManager.flashButton(trackIndex, LEDColor::ORANGE, 100);
            break;
            
        case TrackState::MUTED:
            // Unmute track
            unmuteTrack(trackIndex);
            break;
            
        default:
            break;
    }
}

// === MIDI LOOPER ACTION HANDLERS ===

void startTrackRecording(uint8_t trackIndex) {
    Serial.printf("🔴 Starting recording on track %d\n", trackIndex);
    
    looperState.tracks[trackIndex] = TrackState::RECORDING;
    looperState.isRecording = true;
    
    // Update LEDs with callback
    ledManager.onTrackStartRecording(trackIndex);
    
    // TODO: Start your actual MIDI recording logic here
    // trackManager.startRecording(trackIndex);
}

void stopTrackRecording(uint8_t trackIndex) {
    Serial.printf("⏹️ Stopping recording on track %d\n", trackIndex);
    
    looperState.tracks[trackIndex] = TrackState::PLAYING;
    looperState.isRecording = false;
    
    // Update LEDs with callback
    ledManager.onTrackStopRecording(trackIndex);
    
    // TODO: Stop your actual MIDI recording logic here
    // trackManager.stopRecording(trackIndex);
}

void startTrackOverdubbing(uint8_t trackIndex) {
    Serial.printf("🟡 Starting overdub on track %d\n", trackIndex);
    
    looperState.tracks[trackIndex] = TrackState::OVERDUBBING;
    
    // Update LEDs with callback
    ledManager.onTrackStartOverdubbing(trackIndex);
    
    // TODO: Start your actual overdub logic here
    // trackManager.startOverdubbing(trackIndex);
}

void stopTrackOverdubbing(uint8_t trackIndex) {
    Serial.printf("🟢 Stopping overdub on track %d\n", trackIndex);
    
    looperState.tracks[trackIndex] = TrackState::PLAYING;
    
    // Update LEDs
    ledManager.onTrackStartPlaying(trackIndex);
    
    // TODO: Stop your actual overdub logic here
    // trackManager.stopOverdubbing(trackIndex);
}

void muteTrack(uint8_t trackIndex) {
    Serial.printf("🔇 Muting track %d\n", trackIndex);
    
    looperState.tracks[trackIndex] = TrackState::MUTED;
    ledManager.onTrackMute(trackIndex);
    
    // TODO: Mute track in your audio engine
    // trackManager.muteTrack(trackIndex);
}

void unmuteTrack(uint8_t trackIndex) {
    Serial.printf("🔊 Unmuting track %d\n", trackIndex);
    
    looperState.tracks[trackIndex] = TrackState::PLAYING;
    ledManager.onTrackStartPlaying(trackIndex);
    
    // TODO: Unmute track in your audio engine
    // trackManager.unmuteTrack(trackIndex);
}

void clearTrack(uint8_t trackIndex) {
    Serial.printf("🗑️ Clearing track %d\n", trackIndex);
    
    looperState.tracks[trackIndex] = TrackState::EMPTY;
    ledManager.onTrackClear(trackIndex);
    
    // TODO: Clear track data in your looper
    // trackManager.clearTrack(trackIndex);
}

void handleRecordButtonPress() {
    Serial.println("🔴 Record button pressed");
    
    // Find first empty track to start recording
    for (uint8_t i = 0; i < 4; i++) {
        if (looperState.tracks[i] == TrackState::EMPTY) {
            startTrackRecording(i);
            return;
        }
    }
    
    // No empty tracks - flash error
    ledManager.flashButton(ControlPadLayout::RECORD_BUTTON - 1, LEDColor::RED, 500);
    Serial.println("⚠️ No empty tracks available for recording");
}

void handleStopButtonPress() {
    Serial.println("⏹️ Stop button pressed");
    
    // Stop all recording/overdubbing
    for (uint8_t i = 0; i < 4; i++) {
        if (looperState.tracks[i] == TrackState::RECORDING) {
            stopTrackRecording(i);
        } else if (looperState.tracks[i] == TrackState::OVERDUBBING) {
            stopTrackOverdubbing(i);
        }
    }
    
    looperState.isRecording = false;
    ledManager.setRecordButtonState(false);
}

void handleClearButtonPress() {
    Serial.println("🗑️ Clear button pressed");
    
    // Clear all tracks (you might want to add confirmation)
    for (uint8_t i = 0; i < 4; i++) {
        if (looperState.tracks[i] != TrackState::EMPTY) {
            clearTrack(i);
        }
    }
    
    // Flash clear button to confirm
    ledManager.flashButton(ControlPadLayout::CLEAR_BUTTON - 1, LEDColor::WHITE, 300);
}

// === TIMING INTEGRATION ===

void updateMusicTiming() {
    uint32_t now = millis();
    uint32_t beatInterval = (uint32_t)(60000.0f / looperState.currentBPM);
    
    // Check if it's time for next beat
    if (now - looperState.lastBeatTime >= beatInterval) {
        looperState.lastBeatTime = now;
        looperState.currentBeat = (looperState.currentBeat + 1) % 4;
        
        // New measure?
        if (looperState.currentBeat == 0) {
            looperState.currentMeasure++;
            ledManager.onBarComplete();
        }
        
        // Update LED timing
        ledManager.onMeasureBeat(looperState.currentBeat);
        
        Serial.printf("♪ Beat %d, Measure %d\n", looperState.currentBeat + 1, looperState.currentMeasure);
        
        // TODO: Sync with your ClockManager
        // clockManager.onBeat(currentBeat);
    }
}

// === MAIN LOOP INTEGRATION ===

void loopWithControlPadLEDs() {
    // Update timing (integrate with your ClockManager)
    updateMusicTiming();
    
    // Update LED animations and states
    updateLooperLEDs();
    
    // TODO: Process your existing MIDI looper logic here
    // trackManager.update();
    // buttonManager.update();
    // displayManager.update();
    
    // Handle any new button presses (integrate with your ButtonManager)
    // This would normally come from your button event queue
    static uint32_t lastButtonCheck = 0;
    if (millis() - lastButtonCheck > 10) { // 100Hz button checking
        // checkForButtonPresses(); // Your existing button logic
        lastButtonCheck = millis();
    }
}

// === INTEGRATION HELPERS ===

// Call this from your existing button event handler
void onControlPadButton(uint8_t buttonIndex, bool pressed) {
    if (pressed) {
        looperState.buttonPressed[buttonIndex] = true;
        looperState.buttonPressTime[buttonIndex] = millis();
        handleControlPadButtonPress(buttonIndex);
        
        // Visual feedback
        ledManager.onButtonPressed(buttonIndex);
    } else {
        looperState.buttonPressed[buttonIndex] = false;
        ledManager.onButtonReleased(buttonIndex);
    }
}

// Call this when BPM changes in your looper
void onBPMChange(float newBPM) {
    looperState.currentBPM = newBPM;
    ledManager.setBPM(newBPM);
    Serial.printf("🎵 BPM changed to %.1f\n", newBPM);
}

// Call this for special LED effects during performance
void triggerPerformanceMode() {
    Serial.println("🎪 Activating performance mode LEDs");
    ledManager.applyPerformancePattern();
    
    // Add rainbow effects to playing tracks
    for (uint8_t i = 0; i < 4; i++) {
        if (looperState.tracks[i] == TrackState::PLAYING) {
            ledManager.setButtonAnimation(i, LEDAnimation::RAINBOW, 
                                        LEDColor::GREEN, LEDColor::BLUE);
        }
    }
}

// === EXAMPLE SETUP AND LOOP ===

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    
    Serial.println("🎛️ MIDI Looper with ControlPad LED Integration");
    Serial.println("===============================================");
    
    // Initialize your existing systems first
    // usbHost.begin();
    // trackManager.init();
    // clockManager.init();
    // buttonManager.init();
    
    // Then initialize ControlPad LED integration
    setupControlPadIntegration();
    
    Serial.println("🚀 System ready! Press ControlPad buttons to control the looper.");
}

void loop() {
    // Run the integrated loop
    loopWithControlPadLEDs();
    
    // Small delay to prevent overwhelming the system
    delay(1);
}

/* 
 * === INTEGRATION CHECKLIST ===
 * 
 * 1. ✅ Include ControlPadLEDManager.h in your main project
 * 2. ✅ Add setupControlPadIntegration() to your setup()
 * 3. ✅ Call ledManager.update() in your main loop
 * 4. ✅ Call onControlPadButton() from your ButtonManager
 * 5. ✅ Call ledManager.onTrack*() from your TrackManager state changes
 * 6. ✅ Call ledManager.onMeasureBeat() from your ClockManager
 * 7. ✅ Adapt TrackState enum to match your existing states
 * 8. ✅ Customize button layout in ControlPadLayout class
 * 9. ✅ Adjust colors in LEDColor class to match your aesthetic
 * 10. ✅ Test with your existing MIDI looper workflow
 * 
 * === PERFORMANCE NOTES ===
 * 
 * - LED updates run at 20 FPS (50ms intervals) for smooth animations
 * - Button layout optimized for 4-track looper + future clip expansion
 * - Record button (24) pulses in sync with your BPM
 * - Track buttons (1-4) change color/animation based on state
 * - Beautiful baseline pattern when not in active use
 * - Failsafe protection prevents ControlPad static mode lockup
 */ 