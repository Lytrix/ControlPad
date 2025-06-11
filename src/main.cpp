#include <Arduino.h>
#include <USBHost_t36.h>  // For USBHost Task() processing
#include <vector>

#include "ControlPad.h"
#include "ControlPadHardware.h"  // For USBControlPad class
#include "DeviceBehavior.h"
#include "ClockManager.h"
#include "ARMTimer.h"

// ===== GLOBAL USB HOST SETUP (doctea pattern) =====
extern USBHost globalUSBHost;
extern USBHub hub1, hub2;
extern USBHIDParser hid1, hid2, hid3;
extern USBControlPad globalControlPadDriver;

// ===== GLOBAL MANAGERS (declared in Globals.cpp) =====

// ===== MAIN CONTROLPAD INSTANCE =====
ControlPad controlPad;
BehaviourControlPad* controlPadBehavior = nullptr;

// ===== USB DEVICE DETECTION (doctea pattern) =====
MIDIDevice* midiDevices[] = {nullptr, nullptr, nullptr, nullptr};
bool deviceActive[] = {false, false, false, false};
const char* deviceNames[] = {"MIDI1", "MIDI2", "MIDI3", "MIDI4"};

// *** PRE-BUFFERED ANIMATION FRAMES - Eliminate real-time generation ***
static ControlPadColor precomputedFrames[24][24]; // 24 frames, 24 LEDs each
static bool framesInitialized = false;

void initializeAnimationFrames() {
    if (framesInitialized) return;
    
    // Base rainbow colors
    ControlPadColor baseRainbow[24] = {
        {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
        {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
        {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
        {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
        {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
    };
    
    // Pre-compute all 24 animation frames
    for (int step = 0; step < 24; step++) {
        for (int led = 0; led < 24; led++) {
            if (led == step) {
                precomputedFrames[step][led] = {255, 255, 255}; // White highlight
            } else {
                precomputedFrames[step][led] = baseRainbow[led]; // Base color
            }
        }
    }
    
    framesInitialized = true;
}

// ===== DEVICE DETECTION & BEHAVIOR ASSIGNMENT =====
void detectAndSetupDevices() {
    static ARMIntervalTimer detectionTimer;
    static bool timerInitialized = false;
    
    if (!timerInitialized) {
        detectionTimer.setIntervalMillis(5000);  // Check every 5 seconds
        detectionTimer.start();
        timerInitialized = true;
    }
    
    if (!detectionTimer.hasElapsed()) return;
    
    // Check for device changes (doctea pattern)
    for (int i = 0; i < 4; i++) {
        bool currentlyActive = (midiDevices[i] != nullptr) && (*midiDevices[i]);
        
        if (currentlyActive != deviceActive[i]) {
            if (currentlyActive && !deviceActive[i]) {
                // Device connected
                deviceActive[i] = true;
                Serial.printf("🎹 MIDI Device %d connected\n", i);
                
            } else if (!currentlyActive && deviceActive[i]) {
                // Device disconnected
                deviceActive[i] = false;
                Serial.printf("📤 MIDI Device %d disconnected\n", i);
            }
        }
    }
}

// ===== MIDI EVENT HANDLERS (doctea pattern) =====
void onMIDIControlChange(byte channel, byte control, byte value) {
    // Broadcast to all behaviors
    globalBehaviorManager.broadcastClock();
    
    // Handle tempo control from external device
    if (control == 1) {  // Mod wheel controls tempo
        float bpm = 60.0f + (value * 180.0f / 127.0f);  // 60-240 BPM
        // TODO: Add setBPM to ClockManager if needed
        if (controlPadBehavior) {
            controlPadBehavior->setBPM(bpm);
        }
    }
}

void onMIDINoteOn(byte channel, byte note, byte velocity) {
    globalBehaviorManager.broadcastNoteOn(note, velocity, channel);
}

void onMIDINoteOff(byte channel, byte note, byte velocity) {
    globalBehaviorManager.broadcastNoteOff(note, velocity, channel);
}

void onMIDIClock() {
    // TODO: Handle external MIDI clock if needed
}

void onMIDIStart() {
    // TODO: Handle external MIDI start if needed
}

void onMIDIStop() {
    // TODO: Handle external MIDI stop if needed
}

void setup() {
    Serial.begin(115200);
    
    // Initialize ARM Timer system (replaces manual ARM_DWT setup)
    ARMTimer::begin();
    
    // Non-blocking startup delay using ARM timer
    ARMTimer::blockingDelayMicros(1000000); // 1 second startup delay
    
    Serial.println("🚀 ControlPad starting...");
    
    // ===== USB HOST INITIALIZATION (doctea pattern) =====
    globalUSBHost.begin();
    
    // Give USB Host extended time to initialize properly using ARM timer
    for (int i = 0; i < 50; i++) {
        globalUSBHost.Task();
        ARMTimer::blockingDelayMicros(100000); // 100ms delay
    }
    
    ARMTimer::blockingDelayMicros(1000000); // 1 second delay
    
    if (!controlPad.begin()) {
        Serial.println("❌ Failed to initialize ControlPad");
        return;
    }
    
    Serial.println("✅ ControlPad initialized");
    
    ARMTimer::blockingDelayMicros(2000000);  // Give device time to complete activation sequence (2 seconds)
      
    // ===== BEHAVIOR SETUP (doctea pattern) =====
    controlPadBehavior = new BehaviourControlPad(&controlPad);
    globalBehaviorManager.addBehavior(controlPadBehavior);
    
    // ===== CLOCK MANAGER SETUP =====
    ClockManager::getInstance().init();  // Initialize uClock-based timing
    
    // Initialize pre-computed animation frames
    initializeAnimationFrames();
    
    Serial.println("🎮 Ready - 50ms LED animation active");
    
    // *** DISABLE THE UNIFIED LED ANIMATION SYSTEM ***
    // Let the main loop handle animation instead
    controlPad.disableAnimation();
    Serial.println("🌈 Main loop animation system enabled (Unified disabled)");
}

// *** MAIN LOOP STATE VARIABLES ***
static uint8_t animationStep = 0;

void loop() {
    // *** UCLOCK INTERRUPT-BASED ANIMATION SYSTEM (doctea approach) ***
    static bool uclockAnimationInitialized = false;
    
    // Process USB at much lower frequency to reduce timing interference
    static uint32_t lastUSBTime = 0;
    uint32_t currentTime = millis();
    if ((currentTime - lastUSBTime) >= 10) {  // Only every 10ms instead of every loop
        globalUSBHost.Task();
        lastUSBTime = currentTime;
    }
    
    // Initialize uClock-based animation timing on first run
    if (!uclockAnimationInitialized) {
        #ifdef USE_UCLOCK
        // Set up uClock step-based musical timing
        // Each step = 16th note, animation updates every 4 steps (quarter note)
        // At 120 BPM: quarter note = 500ms - back to stable timing to reduce flickering
        Serial.println("🎵 uClock musical beat animation initialized (doctea approach)");
        #endif
        uclockAnimationInitialized = true;
    }
    
    // *** MUSICAL BEAT-BASED LED ANIMATION (doctea approach) ***
    // uClock step-based timing - each step is a 16th note (125ms at 120 BPM)
    #ifdef USE_UCLOCK
    static uint32_t lastStep = 0;
    uint32_t currentStep = ClockManager::getInstance().getTicks();
    
    if (currentStep != lastStep) {
        lastStep = currentStep;
        
        // Update animation on every 4th step (quarter note = ~500ms at 120 BPM)
        // Back to lower frequency to reduce flickering
        if ((currentStep % 4) == 0) {
            // Timing diagnostics to detect drift
            static uint32_t lastBeatTime = 0;
            static uint32_t lastStep4 = 0;
            uint32_t beatTime = millis();
            uint32_t stepDelta = currentStep - lastStep4;
            uint32_t timeDelta = beatTime - lastBeatTime;
            
            // Debug: Track quarter note timing with drift detection
            if (lastBeatTime > 0) {
                Serial.printf("🎵 Beat frame: %d (step: %d, Δstep: %d, Δtime: %dms)\n", 
                    animationStep, currentStep, stepDelta, timeDelta);
                
                // Flag significant drift (should be ~500ms at 120 BPM for quarter notes)
                if (timeDelta > 600 || timeDelta < 400) {
                    Serial.printf("⚠️ Timing drift detected: expected ~500ms, got %dms\n", timeDelta);
                }
            }
            
            lastBeatTime = beatTime;
            lastStep4 = currentStep;
            
            // Advance animation step
            animationStep = (animationStep + 1) % 24;
            
            // *** ROBUST LED UPDATE WITH PROACTIVE MAINTENANCE ***
            extern USBControlPad globalControlPadDriver;
            static uint8_t consecutiveFailures = 0;
            static uint32_t lastSuccessStep = 0;
            static uint32_t totalUpdateAttempts = 0;
            static uint32_t totalUpdateFailures = 0;
            totalUpdateAttempts++;
            bool ledSuccess = globalControlPadDriver.updateAllLEDs(precomputedFrames[animationStep], 24);
            
            if (ledSuccess) {
                consecutiveFailures = 0;
                lastSuccessStep = currentStep;
                
                // Log success ratio periodically (every 48 steps = 24 seconds)
                if ((currentStep % 48) == 0 && totalUpdateAttempts > 0) {
                    float successRate = ((float)(totalUpdateAttempts - totalUpdateFailures) / totalUpdateAttempts) * 100.0f;
                    Serial.printf("📊 LED Stats: %d/%d successful (%.1f%%) \n", 
                        totalUpdateAttempts - totalUpdateFailures, totalUpdateAttempts, successRate);
                }
            } else {
                consecutiveFailures++;
                totalUpdateFailures++;
                
                // Log EVERY failure to understand the pattern
                Serial.printf("❌ LED update FAILED - frame %d, step %d (failure #%d)\n", 
                    animationStep, currentStep, consecutiveFailures);
                
                // More conservative recovery - avoid interfering with cleanup retries
                // Only attempt recovery after many failures to avoid interfering with cleanup retry system
                if (consecutiveFailures >= 10) {  // Increased from 2 to 10
                    Serial.printf("🔧 %d consecutive LED failures - attempting device recovery\n", consecutiveFailures);
                    
                    // Try device reactivation sequence
                    globalControlPadDriver.sendActivationSequence();
                    delay(50);  // Brief delay for reactivation
                    globalControlPadDriver.setCustomMode();
                    delay(50);  // Brief delay for custom mode
                    
                    // Reset failure counter to avoid spam
                    consecutiveFailures = 0;
                }
                
                // Log extended failures for monitoring
                if ((currentStep - lastSuccessStep) >= 8) { // Every 8 steps = 2 seconds at quarter note timing
                    Serial.printf("🚨 LED BLACKOUT: %d steps (%.1fs) - investigating USB\n", 
                        currentStep - lastSuccessStep, 
                        (currentStep - lastSuccessStep) * 0.125f * 4.0f);
                }
            }
        }
    }
    #else
    // Fallback to simple millis() timing if uClock not available
    static uint32_t lastAnimationTime = 0;
    uint32_t currentTime = millis();
    
    if ((currentTime - lastAnimationTime) >= 50) {
        Serial.printf("🎬 Fallback Animation frame: %d\n", animationStep);
        animationStep = (animationStep + 1) % 24;
        
        extern USBControlPad globalControlPadDriver;
        globalControlPadDriver.updateAllLEDs(precomputedFrames[animationStep], 24);
        
        lastAnimationTime = currentTime;
    }
    #endif
    
    // *** REMOVED ALL BACKGROUND TASKS TO ELIMINATE INTERFERENCE ***
    // Commented out: globalBehaviorManager.loopAll();  // Can cause LED conflicts
    // Commented out: globalClockManager.loop();        // Can cause LED conflicts
    // Commented out: serialEvent();                    // Can cause timing issues
    
    // *** CRITICAL: NO EXTRA DELAYS - Pure LED animation only ***
}

// ===== SERIAL COMMAND INTERFACE =====
void serialEvent() {
    static String command = "";
    
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            command.trim();
            
            if (command.startsWith("bpm ")) {
                float bpm = command.substring(4).toFloat();
                if (bpm >= 60.0f && bpm <= 240.0f) {
                    // TODO: Add setBPM to ClockManager if needed
                    if (controlPadBehavior) {
                        controlPadBehavior->setBPM(bpm);
                    }
                    Serial.printf("🎵 BPM set to %.1f\n", bpm);
                }
            } else if (command == "start") {
                ClockManager::getInstance().restart();
                Serial.println("▶️ Clock started");
            } else if (command == "stop") {
                ClockManager::getInstance().stop();
                Serial.println("⏹️ Clock stopped");
            } else if (command == "beat") {
                // Manual beat trigger via MIDI note
                globalBehaviorManager.broadcastNoteOn(36, 127, 10);
                Serial.println("🥁 Manual beat triggered");
            } else if (command == "status") {
                Serial.printf("🎵 Running: %s, Ticks: %d\n", 
                    ClockManager::getInstance().running() ? "Yes" : "No",
                    ClockManager::getInstance().getTicks());
            }
            
            command = "";
        } else {
            command += c;
        }
    }
}