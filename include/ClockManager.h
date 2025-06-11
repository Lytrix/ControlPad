#ifndef CLOCK_MANAGER_H
#define CLOCK_MANAGER_H

#include <Arduino.h>

#ifdef USE_UCLOCK
#include <uClock.h>

// Forward declaration
class ClockManager;

// Static callback functions for uClock
void clockStepCallback(uint32_t step);
void clockStartCallback();
void clockStopCallback();

class ClockManager {
private:
    volatile uint32_t tickCount = 0;
    volatile bool isRunning = false;
    
public:
    void init() {
        // Initialize uClock library
        uClock.init();
        
        // Set up step-based callback for musical beat timing
        // setOnStep() gives us 16th note timing - perfect for step sequencer style animation
        uClock.setOnStep(clockStepCallback);
        
        // Set up start/stop callbacks
        uClock.setOnClockStart(clockStartCallback);
        
        uClock.setOnClockStop(clockStopCallback);
        
        // Set a reasonable musical tempo - 120 BPM
        // At 120 BPM, each 16th note (step) = 125ms which is perfect for LED animation
        uClock.setTempo(120);
        
        // Start the clock - THIS IS THE KEY PART!
        uClock.start();
        
        isRunning = true;
        
        Serial.printf("🎵 uClock initialized and started: BPM=%.1f\n", uClock.getTempo());
    }
    
    void onClockStep() {
        tickCount++;
    }
    
    void onClockStart() {
        Serial.println("🎵 uClock started");
        isRunning = true;
    }
    
    void onClockStop() {
        Serial.println("🎵 uClock stopped");
        isRunning = false;
    }
    
    uint32_t getTicks() const {
        return tickCount;
    }
    
    bool running() const {
        return isRunning;
    }
    
    void stop() {
        if (isRunning) {
            uClock.stop();
        }
    }
    
    void restart() {
        if (!isRunning) {
            uClock.start();
        }
    }
    
    // Singleton pattern
    static ClockManager& getInstance() {
        static ClockManager instance;
        return instance;
    }
};

// Implementation of callback functions
inline void clockStepCallback(uint32_t step) {
    ClockManager::getInstance().onClockStep();
}

inline void clockStartCallback() {
    ClockManager::getInstance().onClockStart();
}

inline void clockStopCallback() {
    ClockManager::getInstance().onClockStop();
}

#else
// Fallback ARM timer implementation
#include "ARMTimer.h"

class ClockManager {
private:
    ARMTimer timer;
    uint32_t startTicks = 0;
    bool initialized = false;
    
public:
    void init() {
        startTicks = timer.getMicros();
        initialized = true;
        Serial.println("⏰ ARM Timer ClockManager initialized");
    }
    
    uint32_t getTicks() const {
        if (!initialized) return 0;
        
        uint32_t currentMicros = timer.getMicros();
        
        // Handle overflow
        if (currentMicros < startTicks) {
            uint32_t overflowTicks = (UINT32_MAX - startTicks) / 50000;
            uint32_t currentTicks = currentMicros / 50000;
            return overflowTicks + currentTicks;
        }
        
        return (currentMicros - startTicks) / 50000; // 50ms intervals
    }
    
    bool running() const {
        return initialized;
    }
    
    void stop() {
        initialized = false;
    }
    
    void restart() {
        if (!initialized) {
            init();
        }
    }
    
    // Singleton pattern
    static ClockManager& getInstance() {
        static ClockManager instance;
        return instance;
    }
};

#endif // USE_UCLOCK

#endif // CLOCK_MANAGER_H