#pragma once
#include <Arduino.h>
#include <USBHost_t36.h>
#include "ControlPad.h"

// ===== DEVICE BEHAVIOR BASE CLASSES (doctea pattern) =====

class DeviceBehaviourBase {
public:
    virtual ~DeviceBehaviourBase() {}
    virtual void setup() {}
    virtual void loop() {}
    virtual void onClock() {}
    virtual void onStart() {}
    virtual void onStop() {}
    virtual void onNoteOn(byte note, byte velocity, byte channel) {}
    virtual void onNoteOff(byte note, byte velocity, byte channel) {}
    virtual void onControlChange(byte control, byte value, byte channel) {}
    
protected:
    bool enabled = true;
    unsigned long lastActivity = 0;
};

class DeviceBehaviourUSBBase : public DeviceBehaviourBase {
public:
    DeviceBehaviourUSBBase(MIDIDevice* dev) : device(dev) {}
    
    virtual bool claim(MIDIDevice* dev) {
        if (!dev) return false;
        device = dev;
        return true;
    }
    
    virtual void disconnect() {
        device = nullptr;
        enabled = false;
    }
    
    bool isConnected() const { return device != nullptr && enabled; }
    
protected:
    MIDIDevice* device = nullptr;
};

// ===== CONTROLPAD BEHAVIOR (Your device as a behavior) =====

class BehaviourControlPad : public DeviceBehaviourBase {
public:
    BehaviourControlPad(ControlPad* pad) : controlPad(pad) {}
    
    void setup() override {
        Serial.println("🎮 ControlPad Behavior initialized");
        initializeBeatFrames();
        lastBeatTime = millis();
    }
    
    void loop() override {
        updateBeatLED();
    }
    
    void onClock() override {
        // Advance beat on MIDI clock (24 PPQN)
        clockTicks++;
        if (clockTicks >= 24) {  // One quarter note
            clockTicks = 0;
            onBeat();
        }
    }
    
    void onStart() override {
        clockTicks = 0;
        isPlaying = true;
        Serial.println("🎵 ControlPad: Clock started");
    }
    
    void onStop() override {
        isPlaying = false;
        Serial.println("⏹️ ControlPad: Clock stopped");
    }
    
    void onNoteOn(byte note, byte velocity, byte channel) override {
        // Handle button presses for manual beat triggering
        if (channel == 10 && note == 36) {  // Kick drum triggers beat
            onBeat();
        }
    }
    
    void setBPM(float bpm) {
        beatInterval = (60000.0f / bpm);  // ms per beat
    }
    
private:
    ControlPad* controlPad;
    
    // Beat LED system
    static const int NUM_LEDS = 24;
    ControlPadColor beatFrames[NUM_LEDS][NUM_LEDS];
    bool framesInitialized = false;
    
    // Timing
    unsigned long lastBeatTime = 0;
    unsigned long beatInterval = 500;  // 120 BPM default
    int currentBeat = 0;
    
    // MIDI clock
    int clockTicks = 0;
    bool isPlaying = false;
    
    void initializeBeatFrames() {
        if (framesInitialized) return;
        
        // Base rainbow colors (your existing animation)
        ControlPadColor baseRainbow[NUM_LEDS] = {
            {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
            {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
            {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
            {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
            {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
        };
        
        // Pre-compute all beat frames (your existing pattern)
        for (int beat = 0; beat < NUM_LEDS; beat++) {
            for (int led = 0; led < NUM_LEDS; led++) {
                if (led == beat) {
                    beatFrames[beat][led] = {255, 255, 255}; // White highlight for current beat
                } else {
                    beatFrames[beat][led] = baseRainbow[led]; // Base color
                }
            }
        }
        
        framesInitialized = true;
        Serial.println("🌈 Beat LED frames initialized");
    }
    
    void updateBeatLED() {
        // Internal timing when no external clock
        if (!isPlaying && (millis() - lastBeatTime) >= beatInterval) {
            onBeat();
        }
    }
    
    void onBeat() {
        currentBeat = (currentBeat + 1) % NUM_LEDS;
        lastBeatTime = millis();
        
        // Update LEDs with pre-computed frame
        if (controlPad) {
            controlPad->setAllButtonColors(beatFrames[currentBeat]);
            controlPad->forceUpdate();
        }
    }
};

// ===== BEHAVIOR MANAGER (doctea pattern) =====

class BehaviourManager {
public:
    void addBehavior(DeviceBehaviourBase* behavior) {
        behaviors.push_back(behavior);
        behavior->setup();
    }
    
    void setupAll() {
        for (auto* behavior : behaviors) {
            behavior->setup();
        }
    }
    
    void loopAll() {
        for (auto* behavior : behaviors) {
            behavior->loop();
        }
    }
    
    // Broadcast MIDI events to all behaviors
    void broadcastClock() {
        for (auto* behavior : behaviors) {
            behavior->onClock();
        }
    }
    
    void broadcastStart() {
        for (auto* behavior : behaviors) {
            behavior->onStart();
        }
    }
    
    void broadcastStop() {
        for (auto* behavior : behaviors) {
            behavior->onStop();
        }
    }
    
    void broadcastNoteOn(byte note, byte velocity, byte channel) {
        for (auto* behavior : behaviors) {
            behavior->onNoteOn(note, velocity, channel);
        }
    }
    
    void broadcastNoteOff(byte note, byte velocity, byte channel) {
        for (auto* behavior : behaviors) {
            behavior->onNoteOff(note, velocity, channel);
        }
    }
    
private:
    std::vector<DeviceBehaviourBase*> behaviors;
};

// Global behavior manager
extern BehaviourManager globalBehaviorManager; 