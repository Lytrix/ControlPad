#pragma once
#include <Arduino.h>

// ARM Timer Utilities for Teensy 4.x
// Uses ARM DWT (Data Watchpoint and Trace) cycle counter for microsecond precision
// F_CPU_ACTUAL is the actual CPU frequency (typically 600MHz on Teensy 4.1)

class ARMTimer {
public:
    // Initialize ARM DWT cycle counter (call once in setup)
    static void begin() {
        ARM_DEMCR |= ARM_DEMCR_TRCENA;
        ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
        ARM_DWT_CYCCNT = 0; // Reset counter
        initialized = true;
    }
    
    // Get current cycle count (32-bit, wraps every ~7 seconds at 600MHz)
    static inline uint32_t getCycles() {
        return ARM_DWT_CYCCNT;
    }
    
    // Get microseconds since last reset/overflow
    static inline uint32_t getMicros() {
        return ARM_DWT_CYCCNT / (F_CPU_ACTUAL / 1000000);
    }
    
    // High precision delay in microseconds (non-blocking check)
    static inline bool delayMicros(uint32_t startCycles, uint32_t microSeconds) {
        uint32_t targetCycles = microSeconds * (F_CPU_ACTUAL / 1000000);
        return (ARM_DWT_CYCCNT - startCycles) >= targetCycles;
    }
    
    // High precision delay in cycles (non-blocking check)
    static inline bool delayCycles(uint32_t startCycles, uint32_t cycles) {
        return (ARM_DWT_CYCCNT - startCycles) >= cycles;
    }
    
    // Convert microseconds to cycles
    static inline uint32_t microsToClocks(uint32_t micros) {
        return micros * (F_CPU_ACTUAL / 1000000);
    }
    
    // Convert milliseconds to cycles  
    static inline uint32_t millisToClocks(uint32_t millis) {
        return millis * (F_CPU_ACTUAL / 1000);
    }
    
    // Blocking delay using ARM timer (for initialization only)
    static void blockingDelayMicros(uint32_t micros) {
        uint32_t start = ARM_DWT_CYCCNT;
        uint32_t cycles = microsToClocks(micros);
        while ((ARM_DWT_CYCCNT - start) < cycles) {
            // Busy wait
        }
    }
    
private:
    static bool initialized;
};

// High-precision timer class for periodic tasks
class ARMIntervalTimer {
public:
    ARMIntervalTimer() : startCycles(0), intervalCycles(0), enabled(false) {}
    
    // Set interval in microseconds
    void setIntervalMicros(uint32_t micros) {
        intervalCycles = ARMTimer::microsToClocks(micros);
        startCycles = ARMTimer::getCycles();
    }
    
    // Set interval in milliseconds
    void setIntervalMillis(uint32_t millis) {
        intervalCycles = ARMTimer::millisToClocks(millis);
        startCycles = ARMTimer::getCycles();
    }
    
    // Check if interval has elapsed (resets automatically)
    bool hasElapsed() {
        if (!enabled) return false;
        
        uint32_t currentCycles = ARMTimer::getCycles();
        if ((currentCycles - startCycles) >= intervalCycles) {
            startCycles = currentCycles; // Auto-reset for next interval
            return true;
        }
        return false;
    }
    
    // Start the timer
    void start() {
        enabled = true;
        startCycles = ARMTimer::getCycles();
    }
    
    // Stop the timer
    void stop() {
        enabled = false;
    }
    
    // Reset the timer without changing enabled state
    void reset() {
        startCycles = ARMTimer::getCycles();
    }
    
    bool isEnabled() const { return enabled; }
    
private:
    uint32_t startCycles;
    uint32_t intervalCycles;
    bool enabled;
};

// State machine helper for non-blocking delays
class ARMStateMachine {
public:
    enum State {
        IDLE,
        WAITING,
        READY
    };
    
    ARMStateMachine() : state(IDLE), delayCycles(0), startCycles(0) {}
    
    // Start a delay (non-blocking)
    void startDelayMicros(uint32_t micros) {
        delayCycles = ARMTimer::microsToClocks(micros);
        startCycles = ARMTimer::getCycles();
        state = WAITING;
    }
    
    void startDelayMillis(uint32_t millis) {
        delayCycles = ARMTimer::millisToClocks(millis);
        startCycles = ARMTimer::getCycles();
        state = WAITING;
    }
    
    // Update state machine (call in loop)
    void update() {
        if (state == WAITING) {
            if ((ARMTimer::getCycles() - startCycles) >= delayCycles) {
                state = READY;
            }
        }
    }
    
    // Check if delay is complete
    bool isReady() {
        update();
        if (state == READY) {
            state = IDLE;
            return true;
        }
        return false;
    }
    
    State getState() const { return state; }
    
private:
    State state;
    uint32_t delayCycles;
    uint32_t startCycles;
}; 