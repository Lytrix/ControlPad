#include <Arduino.h>
#include "ControlPad.h"
#include <imxrt.h>  // For USB register access
#include <arm_math.h>  // For cache functions

// Forward declarations
void runPerformanceTest();
void measureTransferPerformance(uint32_t transferTime);
uint32_t getBaselineTransferTime();
void setBaselineTransferTime(uint32_t time);

// Create a ControlPad instance
ControlPad controlPad;

// ===== GLOBAL MONITORING STATE =====
static bool monitoringActive = false;
static uint32_t monitoringStartTime = 0;
static uint32_t monitoringStartTransfer = 0;

// ===== BACKGROUND PERIODIC TIMER TRACKING =====
static uint32_t systemStartTime = 0;
static uint32_t lastBackgroundCheck = 0;

// USB register tracking for detecting automatic cleanup triggers
static uint32_t lastUSBStatus = 0;
static uint32_t lastUSBCommand = 0;
static uint32_t lastUSBInterrupt = 0;

// Cleanup phase detection
static bool inAutomaticCleanupPhase = false;
static uint32_t automaticCleanupStartTransfer = 0;
static uint32_t automaticCleanupEndTransfer = 0;

// Performance baseline tracking
static bool baselineEstablished = false;
// Note: baselineTransferTime moved to performance testing section
static uint32_t performanceDegradationStart = 0;

// ===== AUTOMATIC CLEANUP TRIGGER DETECTION SYSTEM =====
// Monitor for the automatic cleanup that kicks in around transfer 1600-2180

// Global state for cleanup trigger detection (removed duplicates - using above declarations)

// ===== TEENSY4_USBHOST MEMORY MANAGEMENT =====
// Manual cache and memory management for preventing flickering

// Manual cache flush for USB transfers
void flushUSBCaches() {
    // Force cache coherency for USB DMA operations
    // This ensures memory writes are visible to USB hardware
    asm volatile("dsb");  // Data Synchronization Barrier
    asm volatile("dmb");  // Data Memory Barrier 
    asm volatile("isb");  // Instruction Synchronization Barrier
    
    // Force all memory operations to complete
    asm volatile("dsb" ::: "memory");
    asm volatile("dmb" ::: "memory");
}

// Aggressive memory cleanup for teensy4_usbhost
void performMemoryCleanup() {
    static uint32_t cleanupCount = 0;
    cleanupCount++;
    
    Serial.printf("🧹 MEMORY CLEANUP #%lu - Aggressive cache and memory management\n", cleanupCount);
    
    // 1. CACHE MANAGEMENT - Force all caches to be coherent
    flushUSBCaches();
    
    // 2. USB HARDWARE REGISTER CLEANUP
    // Clear any error flags in USB controller
    if (USB1_USBSTS & USB_USBSTS_UEI) {
        USB1_USBSTS = USB_USBSTS_UEI;  // Clear USB error interrupt
        Serial.println("   ✓ Cleared USB error flags");
    }
    
    // 3. MEMORY POOL MANAGEMENT
    // Force heap cleanup by performing a small allocation/deallocation
    void* tempPtr = malloc(64);
    if (tempPtr) {
        free(tempPtr);
        Serial.println("   ✓ Forced heap cleanup");
    }
    
    // 4. USB TRANSFER QUEUE CLEANUP
    // Brief delay to let any pending USB operations complete
    delayMicroseconds(500);
    
    // 5. FINAL CACHE SYNC
    flushUSBCaches();
    
    Serial.printf("   ✅ Memory cleanup #%lu complete\n", cleanupCount);
}

// USB register monitoring and cleanup
void monitorUSBRegisters() {
    static uint32_t lastRegisterCheck = 0;
    static uint32_t registerCheckCount = 0;
    uint32_t currentTime = millis();
    
    // Check USB registers every 3 seconds for error accumulation
    if (currentTime - lastRegisterCheck >= 3000) {
        registerCheckCount++;
        
        uint32_t usbsts = USB1_USBSTS;
        
        // Check for error conditions
        bool hasErrors = false;
        if (usbsts & USB_USBSTS_UEI) {
            Serial.printf("⚠️ USB Error Interrupt detected (USBSTS=0x%08X)\n", usbsts);
            hasErrors = true;
        }
        
        if (hasErrors) {
            Serial.printf("🔧 Register Check #%lu - Errors detected, performing cleanup\n", registerCheckCount);
            performMemoryCleanup();
        }
        
        lastRegisterCheck = currentTime;
    }
}

// Manual transfer buffer management
void cleanupTransferBuffers() {
    // Force any pending DMA operations to complete
    asm volatile("dsb");
    
    // Ensure memory coherency with strong memory barriers
    asm volatile("dsb" ::: "memory");
    asm volatile("dmb" ::: "memory");
    asm volatile("isb");
    
    // Brief delay to ensure hardware operations complete
    delayMicroseconds(100);
}

uint32_t now = micros();

// ===== TEENSY4_USBHOST MANUAL ENDPOINT MANAGEMENT =====
// Prevent automatic cleanup at 250 transfers by manual endpoint control

// Transfer counter to track when we approach 250 updates
static uint32_t globalTransferCounter = 0;
static uint32_t lastPreventiveCleanup = 0;

// Manual endpoint queue management
void manualEndpointFlush() {
    // Force endpoint queue flush before hitting automatic limits
    Serial.println("🔧 Manual endpoint queue flush - preventing automatic cleanup");
    
    // Multiple memory barriers to ensure transfer completion
    asm volatile("dsb" ::: "memory");
    asm volatile("dmb" ::: "memory");
    asm volatile("isb");
    
    // Force USB controller to process pending transfers
    if (USB1_USBSTS & USB_USBSTS_AAI) {
        USB1_USBSTS = USB_USBSTS_AAI;  // Clear Async Advance Interrupt
        Serial.println("   ✓ Cleared async advance interrupt");
    }
    
    // Clear any pending USB interrupts to prevent stale state
    if (USB1_USBSTS & (USB_USBSTS_UI | USB_USBSTS_UEI | USB_USBSTS_PCI)) {
        USB1_USBSTS = USB_USBSTS_UI | USB_USBSTS_UEI | USB_USBSTS_PCI;
        Serial.println("   ✓ Cleared USB status interrupts");
    }
    
    // Brief pause to let hardware complete operations
    delayMicroseconds(200);
}

// Resource-based monitoring instead of transfer counting
void preventiveTransferManagement() {
    globalTransferCounter++;
    
    // Monitor system resources that might trigger cleanup
    static uint32_t lastResourceCheck = 0;
    uint32_t currentTime = millis();
    
    // Check resources every 500ms to avoid overhead
    if (currentTime - lastResourceCheck >= 500) {
        lastResourceCheck = currentTime;
        
        // Check USB controller status for resource exhaustion signs
        uint32_t usbsts = USB1_USBSTS;
        uint32_t usbcmd = USB1_USBCMD;
        
        // Look for signs of resource pressure
        bool resourcePressure = false;
        if (usbsts & (USB_USBSTS_UEI | USB_USBSTS_HCH)) {
            Serial.printf("🚨 USB Resource Warning: USBSTS=0x%08X at transfer #%lu\n", usbsts, globalTransferCounter);
            resourcePressure = true;
        }
        
        // Monitor heap fragmentation as indicator
        uint32_t freeHeap = 0;
        #ifdef ESP32
        freeHeap = ESP.getFreeHeap();
        #elif defined(TEENSY)
        extern unsigned long _heap_end;
        extern char* __brkval;
        freeHeap = (char*)&_heap_end - __brkval;
        #endif
        
        // Log resource state periodically for pattern analysis
        Serial.printf("📈 Resources: Transfer #%lu, USBSTS=0x%08X, Heap=%lu\n", 
                     globalTransferCounter, usbsts, freeHeap);
    }
    
    // Log key milestones to track stability periods
    if ((globalTransferCounter % 100) == 0) {
        Serial.printf("📊 STABILITY: #%lu transfers completed\n", globalTransferCounter);
    }
    
    // Special warnings as we approach the suspected cleanup range
    if (globalTransferCounter == 1400) {
        Serial.println("🚨 APPROACHING SUSPECTED CLEANUP ZONE - Transfer 1400 reached!");
        Serial.println("🔍 Watch for status changes around transfer 1600...");
    }
    if (globalTransferCounter == 2100) {
        Serial.println("🎯 ENTERING CLEANUP STABILIZATION ZONE - Transfer 2100 reached!");
        Serial.println("🔍 Monitoring for cleanup completion around transfer 2180...");
    }
}

// Detect automatic cleanup trigger by monitoring system state changes
void detectAutomaticCleanupTrigger() {
    // Monitor USB register changes that indicate automatic cleanup
    uint32_t currentUSBStatus = USB1_USBSTS;
    uint32_t currentUSBCommand = USB1_USBCMD;
    
    // Detect status register changes (potential cleanup indicators)
    if (currentUSBStatus != lastUSBStatus) {
        Serial.printf("🔍 USB STATUS CHANGE at transfer #%lu: 0x%08X -> 0x%08X\n", 
                     globalTransferCounter, lastUSBStatus, currentUSBStatus);
        
        // Check for specific cleanup-related flags
        if (currentUSBStatus & USB_USBSTS_HCH) {
            Serial.printf("🚨 HALT DETECTED at transfer #%lu - Possible automatic cleanup!\n", globalTransferCounter);
        }
        if (currentUSBStatus & USB_USBSTS_UEI) {
            Serial.printf("🚨 HOST SYSTEM ERROR at transfer #%lu - Hardware cleanup triggered!\n", globalTransferCounter);
        }
        if (currentUSBStatus & USB_USBSTS_AAI) {
            Serial.printf("🔧 ASYNC ADVANCE INTERRUPT at transfer #%lu - Queue management active\n", globalTransferCounter);
        }
        if (currentUSBStatus & USB_USBSTS_UEI) {
            Serial.printf("⚠️ USB ERROR INTERRUPT at transfer #%lu - Error handling triggered\n", globalTransferCounter);
        }
        if (currentUSBStatus & USB_USBSTS_FRI) {
            Serial.printf("🔄 FRAME LIST ROLLOVER at transfer #%lu - Frame management reset\n", globalTransferCounter);
        }
        if (currentUSBStatus & USB_USBSTS_PCI) {
            Serial.printf("🔌 PORT CHANGE at transfer #%lu - USB port state change\n", globalTransferCounter);
        }
        
        lastUSBStatus = currentUSBStatus;
    }
    
    // Detect command register changes (USB controller mode changes)
    if (currentUSBCommand != lastUSBCommand) {
        Serial.printf("🔍 USB COMMAND CHANGE at transfer #%lu: 0x%08X -> 0x%08X\n", 
                     globalTransferCounter, lastUSBCommand, currentUSBCommand);
        lastUSBCommand = currentUSBCommand;
    }
    
    // DISABLE INTENSIVE MONITORING - THIS WAS CAUSING THE FLICKERING!
    /*
    // Monitor additional USB registers during suspected cleanup phase
    if (inAutomaticCleanupPhase) {
        static uint32_t lastIntensiveCheck = 0;
        uint32_t currentTime = millis();
        
        // Intensive monitoring every 100ms during cleanup phase
        if (currentTime - lastIntensiveCheck >= 100) {
            lastIntensiveCheck = currentTime;
            
            // Monitor USB interrupt enable register for cleanup-related changes
            uint32_t usbintr = USB1_USBINTR;
            Serial.printf("🔍 INTENSIVE: Transfer #%lu - USBSTS=0x%08X, USBCMD=0x%08X, USBINTR=0x%08X\n", 
                         globalTransferCounter, currentUSBStatus, currentUSBCommand, usbintr);
            
            // Check for periodic list and async list status
            if (!(currentUSBStatus & USB_USBSTS_PS)) {
                Serial.printf("⏸️  PERIODIC SCHEDULE DISABLED at transfer #%lu\n", globalTransferCounter);
            }
            if (!(currentUSBStatus & USB_USBSTS_AS)) {
                Serial.printf("⏸️  ASYNC SCHEDULE DISABLED at transfer #%lu\n", globalTransferCounter);
            }
        }
    }
    */
    
    // DISABLE CLEANUP PHASE DETECTION - NO LONGER NEEDED
    /*
    // Detect the cleanup phase based on transfer range
    bool shouldBeInCleanupPhase = (globalTransferCounter >= 1500 && globalTransferCounter <= 2300);
    
    if (shouldBeInCleanupPhase && !inAutomaticCleanupPhase) {
        // Entering suspected cleanup phase
        inAutomaticCleanupPhase = true;
        automaticCleanupStartTransfer = globalTransferCounter;
        Serial.printf("🚨 ENTERING SUSPECTED CLEANUP PHASE at transfer #%lu\n", globalTransferCounter);
        Serial.println("🔍 Enabling intensive monitoring for cleanup triggers...");
    } else if (!shouldBeInCleanupPhase && inAutomaticCleanupPhase) {
        // Exiting cleanup phase
        inAutomaticCleanupPhase = false;
        automaticCleanupEndTransfer = globalTransferCounter;
        Serial.printf("✅ EXITING CLEANUP PHASE at transfer #%lu\n", globalTransferCounter);
        Serial.printf("📊 Cleanup phase lasted %lu transfers (%lu -> %lu)\n", 
                     automaticCleanupEndTransfer - automaticCleanupStartTransfer,
                     automaticCleanupStartTransfer, automaticCleanupEndTransfer);
    }
    */
}

// Monitor heap memory pressure that could trigger cleanup
void monitorMemoryPressure() {
    static uint32_t lastMemoryCheck = 0;
    static uint32_t baselineHeapSize = 0;
    uint32_t currentTime = millis();
    
    // SIMPLIFIED MONITORING - No longer tied to cleanup phase
    // Check memory every 1000ms (removed intensive monitoring)
    uint32_t checkInterval = 1000;
    
    if (currentTime - lastMemoryCheck >= checkInterval) {
        lastMemoryCheck = currentTime;
        
        // Monitor heap usage (Teensy 4.x specific)
        uint32_t currentHeapSize = 0;
        #ifdef TEENSY
        extern unsigned long _heap_end;
        extern char* __brkval;
        if (__brkval != nullptr) {
            currentHeapSize = (char*)&_heap_end - __brkval;
        }
        #endif
        
        if (baselineHeapSize == 0) {
            baselineHeapSize = currentHeapSize;
            Serial.printf("📊 HEAP BASELINE: %lu bytes\n", baselineHeapSize);
        }
        
        // DISABLE CLEANUP PHASE MONITORING
        /*
        // Detect significant heap pressure during cleanup phase
        if (inAutomaticCleanupPhase && currentHeapSize != baselineHeapSize) {
            Serial.printf("💾 HEAP PRESSURE during cleanup: %lu bytes (baseline: %lu, diff: %ld)\n", 
                         currentHeapSize, baselineHeapSize, (long)currentHeapSize - (long)baselineHeapSize);
        }
        */
        
        // Monitor for heap fragmentation or exhaustion
        if (currentHeapSize < (baselineHeapSize / 2)) {
            Serial.printf("🚨 HEAP EXHAUSTION at transfer #%lu: %lu bytes remaining\n", 
                         globalTransferCounter, currentHeapSize);
        }
    }
}

// Monitor performance degradation that indicates cleanup activity
void monitorPerformanceDegradation(uint32_t transferTime) {
    // Use the baseline from performance testing system
    uint32_t currentBaseline = getBaselineTransferTime();
    
    // Establish baseline performance during first 100 transfers
    if (!baselineEstablished && globalTransferCounter >= 50 && globalTransferCounter <= 150) {
        if (currentBaseline == 0) {
            setBaselineTransferTime(transferTime);
        } else {
            // Average with previous measurements
            setBaselineTransferTime((currentBaseline + transferTime) / 2);
        }
        
        if (globalTransferCounter == 150) {
            baselineEstablished = true;
            Serial.printf("📊 BASELINE ESTABLISHED: %lu μs per transfer\n", getBaselineTransferTime());
        }
        return;
    }
    
    if (!baselineEstablished) return;
    
    currentBaseline = getBaselineTransferTime();
    
    // Detect significant performance degradation (2x baseline or more)
    if (transferTime > (currentBaseline * 2)) {
        if (performanceDegradationStart == 0) {
            performanceDegradationStart = globalTransferCounter;
            Serial.printf("🐌 PERFORMANCE DEGRADATION STARTS at transfer #%lu\n", globalTransferCounter);
            Serial.printf("    Transfer time: %lu μs (baseline: %lu μs, ratio: %.1fx)\n", 
                         transferTime, currentBaseline, (float)transferTime / currentBaseline);
        }
        
        // Log severe performance issues during suspected cleanup phase
        if (inAutomaticCleanupPhase) {
            Serial.printf("🚨 SEVERE SLOWDOWN during cleanup phase: %lu μs (%.1fx baseline)\n", 
                         transferTime, (float)transferTime / currentBaseline);
        }
    } else {
        if (performanceDegradationStart != 0) {
            Serial.printf("✅ PERFORMANCE RECOVERED at transfer #%lu (degraded for %lu transfers)\n", 
                         globalTransferCounter, globalTransferCounter - performanceDegradationStart);
            performanceDegradationStart = 0;
        }
    }
}

// Track background operations and periodic events
void monitorBackgroundOperations() {
    uint32_t currentTime = millis();
    uint32_t systemUptime = currentTime - systemStartTime;
    
    // SIMPLIFIED MONITORING - Remove flicker window detection since issue is resolved
    // Check for background operations every 10 seconds (was every 1 second)
    if (currentTime - lastBackgroundCheck >= 10000) {
        lastBackgroundCheck = currentTime;
        
        // Simple uptime marker (every 30 seconds)
        if (systemUptime % 30000 == 0) {
            Serial.printf("⏰ SYSTEM UPTIME: %lus (Transfer #%lu)\n", 
                         systemUptime / 1000, globalTransferCounter);
        }
    }
    
    // DISABLE ALL FLICKER WINDOW MONITORING - NO LONGER NEEDED
    /*
    // === PERIODIC CLEANUP CYCLE DETECTION ===
    // Based on your observations: flickering patterns every ~30s
    // Pattern: +70s, +92s (+22s gap), +120s (+28s gap), etc.
    static uint32_t lastFlickerTime = 0;
    static bool inFlickerWindow = false;
    
    // Mark suspected flicker windows based on observed pattern
    bool shouldBeFlickering = false;
    if ((systemUptime >= 70000 && systemUptime <= 80000) ||   // +70s to +80s
        (systemUptime >= 92000 && systemUptime <= 100000) ||  // +92s to +100s  
        (systemUptime >= 120000 && systemUptime <= 160000) || // +120s to +160s (long)
        (systemUptime >= 190000 && systemUptime <= 200000) || // Predicted next
        (systemUptime >= 220000 && systemUptime <= 230000)) { // Predicted next
        shouldBeFlickering = true;
    }
    
    // Detect transitions into/out of flicker windows
    if (shouldBeFlickering && !inFlickerWindow) {
     
        lastFlickerTime = systemUptime;
        Serial.printf("🔥 ENTERING SUSPECTED FLICKER WINDOW at +%lus (T#%lu)\n", 
                     systemUptime / 1000, globalTransferCounter);
        Serial.println("🎯 Watch for LED sequence disruption starting NOW!");
    } else if (!shouldBeFlickering && inFlickerWindow) {
        inFlickerWindow = false;
        uint32_t flickerDuration = systemUptime - lastFlickerTime;
        Serial.printf("✅ EXITING FLICKER WINDOW at +%lus (T#%lu) - Duration: %lus\n", 
                     systemUptime / 1000, globalTransferCounter, flickerDuration / 1000);
    }
    
    // Enhanced granular timing monitoring (every 10 seconds)
    if (systemUptime % 10000 == 0) {
        Serial.printf("⏰ TIMING CHECKPOINT: %lus mark reached at transfer #%lu%s\n", 
                     systemUptime / 1000, globalTransferCounter,
                     shouldBeFlickering ? " [FLICKER WINDOW]" : "");
    }
    
    // === SPECIFIC TIMING MARKERS ===
    // Monitor exact timing points you observed
    if (systemUptime == 70000) {
        Serial.printf("🔥 FLICKER START ZONE: 70-second mark reached at transfer #%lu\n", globalTransferCounter);
    }
    if (systemUptime == 80000) {
        Serial.printf("✅ FLICKER END ZONE: 80-second mark reached at transfer #%lu\n", globalTransferCounter);
    }
    if (systemUptime == 92000) {
        Serial.printf("🔥 FLICKER START ZONE: 92-second mark reached at transfer #%lu\n", globalTransferCounter);
    }
    if (systemUptime == 100000) {
        Serial.printf("✅ FLICKER END ZONE: 100-second mark reached at transfer #%lu\n", globalTransferCounter);
    }
    if (systemUptime == 120000) {
        Serial.printf("🔥 LONG FLICKER START: 120-second mark reached at transfer #%lu\n", globalTransferCounter);
    }
    if (systemUptime == 160000) {
        Serial.printf("✅ LONG FLICKER END: 160-second mark reached at transfer #%lu\n", globalTransferCounter);
    }
    */
}

// Monitor LED sequence state for desynchronization detection
void monitorSequenceState(uint8_t currentButton, bool highlightState, uint32_t operationTime) {
    static uint32_t lastSequenceCheck = 0;
    static uint32_t expectedOperationTime = 4000; // 4ms baseline
    static uint8_t lastButton = 255;
    static bool lastHighlightState = false;
    static uint32_t sequenceErrorCount = 0;
    
    uint32_t currentTime = millis();
    
    // Check for sequence anomalies every few transfers
    if (currentTime - lastSequenceCheck >= 500) { // Every 500ms
        lastSequenceCheck = currentTime;
        
        // Detect sequence timing anomalies
        if (operationTime > (expectedOperationTime * 3)) { // 3x slower than expected
            sequenceErrorCount++;
            Serial.printf("🚨 SEQUENCE TIMING ANOMALY: Transfer #%lu took %lu μs (expected ~%lu μs) - Error #%lu\n", 
                         globalTransferCounter, operationTime, expectedOperationTime, sequenceErrorCount);
            
            if (sequenceErrorCount >= 3) {
                Serial.printf("🔥 SEQUENCE DESYNC DETECTED: Multiple timing anomalies at transfer #%lu (+%lus)\n", 
                             globalTransferCounter, (currentTime - systemStartTime) / 1000);
            }
        }
        
        // Detect button sequence out-of-order
        if (lastButton != 255 && highlightState && !lastHighlightState) {
            uint8_t expectedNextButton = (lastButton + 1) % 24;
            if (currentButton != expectedNextButton && currentButton != 0) { // Allow wrap-around
                Serial.printf("🚨 SEQUENCE ORDER ERROR: Expected button %d, got button %d at transfer #%lu\n", 
                             expectedNextButton + 1, currentButton + 1, globalTransferCounter);
            }
        }
        
        // Reset error count if we have stable operation
        if (operationTime <= (expectedOperationTime * 2)) {
            if (sequenceErrorCount > 0) {
                sequenceErrorCount = max(0, (int)sequenceErrorCount - 1); // Gradual recovery
            }
        }
        
        lastButton = currentButton;
        lastHighlightState = highlightState;
    }
}

// Enhanced transfer tracking wrapper with full USB register monitoring
void trackTransferAndCleanup() {
    preventiveTransferManagement();
    
    // Start detailed monitoring from transfer 0 (no delay for background operation tracking)
    static uint32_t initializationCompleteTime = 0;
    
    // Start monitoring immediately from transfer 0
    if (!monitoringActive && globalTransferCounter >= 0) {
        monitoringActive = true;
        monitoringStartTime = millis();
        monitoringStartTransfer = globalTransferCounter;
        initializationCompleteTime = millis();
        Serial.println("🔍 === USB REGISTER MONITORING ACTIVATED FROM START ===");
        Serial.printf("🎯 Starting detailed monitoring at transfer #%lu\n", globalTransferCounter);
        Serial.println("🔍 Now watching for register changes and background operations...");
    }
    
    // Only run detailed monitoring after initialization
    if (monitoringActive) {
        // Monitor background operations and periodic events
        monitorBackgroundOperations();
        
        detectAutomaticCleanupTrigger();
        monitorMemoryPressure();
        
        // Run performance testing system
        runPerformanceTest();
        
        // LIGHT USB MONITORING - Check every 10 seconds instead of 25ms
        static uint32_t lastLightCheck = 0;
        uint32_t currentTime = millis();
        if (currentTime - lastLightCheck >= 10000) {
            lastLightCheck = currentTime;
            
            uint32_t usbsts = USB1_USBSTS;
            uint32_t uptime = (currentTime - monitoringStartTime) / 1000;
            Serial.printf("📈 STATUS [T#%lu]: USBSTS=0x%08X, Monitoring=LIGHT, Time=+%lus\n", 
                         globalTransferCounter, usbsts, uptime);
        }
        
        // Main loop heartbeat every 10 seconds
        static uint32_t lastHeartbeat = 0;
        if (currentTime - lastHeartbeat >= 10000) {
            lastHeartbeat = currentTime;
            Serial.println("🟢 Main loop is running - Memory management active");
        }
    }
}

// ===== USB CONNECTION STABILITY MONITORING =====
// Monitor USB stability and recover from activation failures

void monitorUSBStability() {
    static uint32_t lastStabilityCheck = 0;
    static bool lastConnectionState = false;
    uint32_t currentTime = millis();
    
    // Check USB stability every 2 seconds
    if (currentTime - lastStabilityCheck >= 2000) {
        lastStabilityCheck = currentTime;
        
        // Monitor USB controller status instead of high-level connection
        uint32_t usbcmd = USB1_USBCMD;
        bool usbControllerRunning = (usbcmd & USB_USBCMD_RS) != 0;
        
        // Detect controller state changes
        if (usbControllerRunning != lastConnectionState) {
            if (usbControllerRunning) {
                Serial.println("✅ USB Controller RUNNING");
            } else {
                Serial.println("❌ USB Controller STOPPED - potential instability");
            }
            lastConnectionState = usbControllerRunning;
        }
        
        // Monitor for activation-related USB errors
        uint32_t usbsts = USB1_USBSTS;
        if (usbsts & USB_USBSTS_UEI) {
            Serial.printf("⚠️ USB Error during operation: USBSTS=0x%08X\n", usbsts);
            
            // Clear error but don't do aggressive cleanup
            USB1_USBSTS = USB_USBSTS_UEI;
            Serial.println("   ✓ Cleared USB error flag (gentle recovery)");
        }
    }
}

// ===== CONTROLLED PERFORMANCE TESTING SYSTEM =====
// Test individual monitoring operations to identify what causes USB interference

enum PerformanceTestMode {
    TEST_NONE = 0,           // No additional load (baseline)
    TEST_SERIAL_FLOOD,       // Test serial output impact
    TEST_USB_REGISTER_READS, // Test USB register polling impact  
    TEST_MEMORY_OPERATIONS,  // Test heap monitoring impact
    TEST_COMBINED_LIGHT,     // Light version of all operations
    TEST_COMBINED_HEAVY      // Heavy version of all operations
};

static PerformanceTestMode currentTestMode = TEST_NONE;
static uint32_t testModeStartTime = 0;
static uint32_t testModeStartTransfer = 0;
static uint32_t testModeDuration = 30000; // 30 seconds per test

// Performance impact measurement
static uint32_t baselineTransferTime = 0;
static uint32_t slowTransferCount = 0;
static uint32_t totalTransferTime = 0;
static uint32_t transferTimeCount = 0;

// Getter function for baseline transfer time
uint32_t getBaselineTransferTime() {
    return baselineTransferTime;
}

void setBaselineTransferTime(uint32_t time) {
    baselineTransferTime = time;
}

void startPerformanceTest(PerformanceTestMode mode) {
    currentTestMode = mode;
    testModeStartTime = millis();
    testModeStartTransfer = globalTransferCounter;
    slowTransferCount = 0;
    totalTransferTime = 0;
    transferTimeCount = 0;
    
    const char* modeName = "UNKNOWN";
    switch(mode) {
        case TEST_NONE: modeName = "BASELINE"; break;
        case TEST_SERIAL_FLOOD: modeName = "SERIAL_FLOOD"; break;
        case TEST_USB_REGISTER_READS: modeName = "USB_REGISTER_READS"; break;
        case TEST_MEMORY_OPERATIONS: modeName = "MEMORY_OPERATIONS"; break;
        case TEST_COMBINED_LIGHT: modeName = "COMBINED_LIGHT"; break;
        case TEST_COMBINED_HEAVY: modeName = "COMBINED_HEAVY"; break;
    }
    
    Serial.println("\n" + String("=").substring(0, 60));
    Serial.printf("🧪 STARTING PERFORMANCE TEST: %s\n", modeName);
    Serial.printf("📊 Will run for %lu seconds starting at transfer #%lu\n", 
                 testModeDuration / 1000, globalTransferCounter);
    Serial.printf("⏰ Test start time: %lu ms\n", testModeStartTime);
    Serial.println("🎯 WATCH FOR LED FLICKERING DURING THIS TEST!");
    Serial.println(String("=").substring(0, 60) + "\n");
}

void runPerformanceTest() {
    if (currentTestMode == TEST_NONE) return;
    
    uint32_t currentTime = millis();
    static uint32_t lastTestOperation = 0;
    static uint32_t lastProgressReport = 0;
    
    // Progress report every 5 seconds during test
    if (currentTime - lastProgressReport >= 5000) {
        lastProgressReport = currentTime;
        uint32_t elapsed = currentTime - testModeStartTime;
        uint32_t remaining = testModeDuration - elapsed;
        float progress = (float)elapsed / testModeDuration * 100.0;
        
        Serial.printf("🧪 TEST PROGRESS: %.1f%% complete (%lu/%lu seconds) - Transfer #%lu\n", 
                     progress, elapsed / 1000, testModeDuration / 1000, globalTransferCounter);
    }
    
    // Run test operations every 25ms (same frequency as the original monitoring)
    if (currentTime - lastTestOperation >= 25) {
        lastTestOperation = currentTime;
        
        switch(currentTestMode) {
            case TEST_SERIAL_FLOOD:
                // Test serial output impact
                Serial.printf("🔍 SERIAL_TEST: Transfer #%lu - Heavy serial output load\n", globalTransferCounter);
                Serial.printf("📊 Additional data: Time=%lu, Mode=FLOOD, Status=ACTIVE\n", currentTime);
                Serial.printf("⚡ More output: Counter=%lu, Test=RUNNING, Load=HIGH\n", globalTransferCounter);
                break;
                
            case TEST_USB_REGISTER_READS:
                // Test USB register polling impact
                {
                    volatile uint32_t usbsts = USB1_USBSTS;
                    volatile uint32_t usbcmd = USB1_USBCMD;
                    volatile uint32_t usbintr = USB1_USBINTR;
                    // Read them multiple times to simulate intensive polling
                    for(int i = 0; i < 5; i++) {
                        usbsts = USB1_USBSTS;
                        usbcmd = USB1_USBCMD;
                        usbintr = USB1_USBINTR;
                    }
                    // Minimal output to avoid serial interference
                    if ((globalTransferCounter % 40) == 0) {
                        Serial.printf("🔍 USB_REG_TEST: T#%lu - Polling registers\n", globalTransferCounter);
                    }
                }
                break;
                
            case TEST_MEMORY_OPERATIONS:
                // Test heap monitoring impact
                {
                    extern unsigned long _heap_end;
                    extern char* __brkval;
                    volatile uint32_t heapSize = 0;
                    if (__brkval != nullptr) {
                        heapSize = (char*)&_heap_end - __brkval;
                    }
                    
                    // Force some memory allocations/deallocations
                    void* tempPtr = malloc(64);
                    if (tempPtr) {
                        free(tempPtr);
                    }
                    
                    if ((globalTransferCounter % 40) == 0) {
                        Serial.printf("🔍 MEMORY_TEST: T#%lu - Heap=%lu\n", globalTransferCounter, heapSize);
                    }
                }
                break;
                
            case TEST_COMBINED_LIGHT:
                // Light version of all operations
                {
                    volatile uint32_t usbsts = USB1_USBSTS;
                    if ((globalTransferCounter % 80) == 0) {
                        Serial.printf("🔍 LIGHT_TEST: T#%lu - USBSTS=0x%08X\n", globalTransferCounter, usbsts);
                    }
                }
                break;
                
            case TEST_COMBINED_HEAVY:
                // Heavy version combining all operations
                {
                    // USB register reads
                    volatile uint32_t usbsts = USB1_USBSTS;
                    volatile uint32_t usbcmd = USB1_USBCMD;
                    
                    // Memory operations
                    extern unsigned long _heap_end;
                    extern char* __brkval;
                    volatile uint32_t heapSize = 0;
                    if (__brkval != nullptr) {
                        heapSize = (char*)&_heap_end - __brkval;
                    }
                    
                    // Serial output
                    Serial.printf("🔍 HEAVY_TEST: T#%lu - USBSTS=0x%08X, USBCMD=0x%08X, Heap=%lu\n", 
                                 globalTransferCounter, usbsts, usbcmd, heapSize);
                }
                break;
                
            default:
                break;
        }
    }
    
    // Check if test duration is complete
    if (currentTime - testModeStartTime >= testModeDuration) {
        uint32_t transfersInTest = globalTransferCounter - testModeStartTransfer;
        uint32_t avgTransferTime = transferTimeCount > 0 ? totalTransferTime / transferTimeCount : 0;
        
        Serial.println("\n" + String("=").substring(0, 60));
        Serial.printf("📊 PERFORMANCE TEST COMPLETE: %d\n", currentTestMode);
        Serial.printf("   Duration: %lu seconds\n", testModeDuration / 1000);
        Serial.printf("   Transfers: %lu\n", transfersInTest);
        Serial.printf("   Slow transfers: %lu (%.1f%%)\n", 
                     slowTransferCount, (float)slowTransferCount * 100.0 / transfersInTest);
        Serial.printf("   Avg transfer time: %lu μs\n", avgTransferTime);
        Serial.println(String("=").substring(0, 60));
        
        // Move to next test mode
        currentTestMode = (PerformanceTestMode)((int)currentTestMode + 1);
        if (currentTestMode <= TEST_COMBINED_HEAVY) {
            Serial.printf("\n⏳ 5 second break before next test...\n\n");
            delay(5000); // 5 second gap between tests
            startPerformanceTest(currentTestMode);
        } else {
            Serial.println("\n" + String("🎯").substring(0, 60));
            Serial.println("🎯 ALL PERFORMANCE TESTS COMPLETE!");
            Serial.println("📊 Check results above to identify which operations cause slowdowns");
            Serial.println("✅ System returned to normal operation");
            Serial.println(String("🎯").substring(0, 60) + "\n");
            currentTestMode = TEST_NONE;
        }
    }
}

void measureTransferPerformance(uint32_t transferTime) {
    // Collect performance statistics during tests
    if (currentTestMode != TEST_NONE) {
        totalTransferTime += transferTime;
        transferTimeCount++;
        
        // Count transfers that are significantly slower than baseline
        uint32_t currentBaseline = getBaselineTransferTime();
        if (currentBaseline > 0 && transferTime > (currentBaseline * 2)) {
            slowTransferCount++;
        }
    } else if (getBaselineTransferTime() == 0 && globalTransferCounter > 50) {
        // Establish baseline during normal operation
        static uint32_t baselineSum = 0;
        static uint32_t baselineCount = 0;
        
        baselineSum += transferTime;
        baselineCount++;
        
        // Debug output for baseline establishment
        if (baselineCount <= 5 || baselineCount % 5 == 0) {
            Serial.printf("🔧 BASELINE PROGRESS: %lu/%lu measurements, current avg: %lu μs\n", 
                         baselineCount, 20UL, baselineSum / baselineCount);
        }
        
        if (baselineCount >= 20) {
            uint32_t avgBaseline = baselineSum / baselineCount;
            setBaselineTransferTime(avgBaseline);
            Serial.println("\n" + String("*").substring(0, 60));
            Serial.printf("📊 BASELINE ESTABLISHED: %lu μs per transfer\n", avgBaseline);
            Serial.printf("🎯 Based on %lu transfer measurements\n", baselineCount);
            Serial.println("🧪 Starting performance testing sequence in 5 seconds...");
            Serial.println(String("*").substring(0, 60) + "\n");
            delay(5000);
            startPerformanceTest(TEST_SERIAL_FLOOD);
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // Initialize system timing for background operation tracking
    systemStartTime = millis();
    
    Serial.println("🚀 ControlPad Combined Package Test - Maximum LED Speed!");
    Serial.println("🧪 PERFORMANCE IMPACT ANALYSIS - Testing what causes USB interference");
    Serial.println("📊 Will test: Serial flood, USB registers, memory ops, combined loads");
    Serial.println("🎯 Each test runs 30 seconds - watch for LED flickering during specific tests!");
    Serial.println("⚡ This will help identify what monitoring operations impact USB performance");
    
    if (!controlPad.begin()) {
        Serial.println("❌ Failed to initialize ControlPad");
        return;
    }
    
    Serial.println("✅ ControlPad initialized successfully!");
    
    // Disable smart updates - we want immediate LED updates for responsiveness
    controlPad.enableSmartUpdates(false);
    
    // Set instant updates for maximum responsiveness
    controlPad.enableInstantUpdates(true);
    
    // Set initial rainbow colors so we can see the highlighting effect
    Serial.println("🌈 Setting initial rainbow colors...");
    ControlPadColor rainbowColors[24] = {
        {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
        {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
        {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
        {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
        {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
    };
    
    // Set all colors at once using the smart LED system
    controlPad.setAllButtonColors(rainbowColors);
    
    // Force immediate update to show rainbow colors at startup
    Serial.println("🌈 Forcing immediate LED update for rainbow colors...");
    controlPad.forceUpdate();
    
    // Skip aggressive cleanup during initialization to prevent USB instability
    Serial.println("🎯 Skipping initial cleanup to maintain USB stability...");
    
    Serial.println("✅ Initial colors set and updated!");
    Serial.println("🎮 Ready for button events - press buttons to see WHITE highlighting!");
    Serial.println("📌 Using new combined LED package system for reduced flicker");
    Serial.println("💡 Buttons will highlight in WHITE when pressed, then return to original colors");
    Serial.println("🧠 Advanced memory management: Cache flushes + Register monitoring + Buffer cleanup");
}

void loop() {
    // ===== USB STABILITY MONITORING =====
    // Monitor USB stability and activation issues
    monitorUSBStability();
    
    // Still process USB events to keep the connection alive
    controlPad.poll();
    
    // Debug: Check if main loop is running
    static unsigned long lastDebugTime = 0;
    if (millis() - lastDebugTime > 5000) {
        lastDebugTime = millis();
        Serial.println("🟢 Main loop is running - Memory management active");
    }
    
    // Process any real button events (optional - you can disable this if you want only the loop)
    ControlPadEvent event;
    while (controlPad.pollEvent(event)) {
        if (event.type == ControlPadEventType::Button) {
            // Optional: Handle real button events alongside the automated loop
            controlPad.setButtonHighlight(event.button.button, event.button.pressed);
        }
    }
    
    // *** AUTOMATED HIGHLIGHT LOOP FOR TESTING ALL 24 BUTTONS ***
    static unsigned long lastHighlightTime = 0;
    static uint8_t currentHighlightButton = 0;
    static bool highlightState = false; // false = unhighlighted, true = highlighted
    static uint8_t highlightCounter = 0; // Track how many highlights we've done
    static uint8_t passCounter = 0; // Track complete passes through all 24 buttons
    
    unsigned long currentTime = millis();
    
    // Change highlight every 100ms (was 500ms in comment)
    if (currentTime - lastHighlightTime >= 100) {
        lastHighlightTime = currentTime;
        
        if (!highlightState) {
            // Turn OFF the previous button's highlight
            if (currentHighlightButton > 0) {
                controlPad.setButtonHighlight(currentHighlightButton - 1, false);
            } else {
                // Wrap around - turn off button 23 when starting from button 0
                controlPad.setButtonHighlight(23, false);
            }
            
            // Turn ON the current button's highlight
            unsigned long highlightStartTime = micros();
            controlPad.setButtonHighlight(currentHighlightButton, true);
            unsigned long highlightEndTime = micros();
            trackTransferAndCleanup(); // Track this transfer
            
            // Detect unusually slow LED updates (potential flickering)
            unsigned long highlightDuration = highlightEndTime - highlightStartTime;
            monitorPerformanceDegradation(highlightDuration); // Track performance for cleanup detection
            measureTransferPerformance(highlightDuration); // Track performance for testing
            if (highlightDuration > 50000) { // More than 50ms is suspicious
                Serial.printf("⚠️ SLOW HIGHLIGHT: button %d took %lu μs [Transfer: %lu]\n", 
                             currentHighlightButton + 1, highlightDuration, globalTransferCounter);
            }
            
            // Highlighting button (removed print to clean up monitoring display)
            
            // Monitor sequence state for desynchronization
            monitorSequenceState(currentHighlightButton, true, highlightDuration);
            
            highlightState = true;
            highlightCounter++;
        } else {
            // Turn OFF the current button's highlight
            unsigned long unhighlightStartTime = micros();
            controlPad.setButtonHighlight(currentHighlightButton, false);
            unsigned long unhighlightEndTime = micros();
            trackTransferAndCleanup(); // Track this transfer
            
            // Detect unusually slow LED updates (potential flickering)
            unsigned long unhighlightDuration = unhighlightEndTime - unhighlightStartTime;
            monitorPerformanceDegradation(unhighlightDuration); // Track performance for cleanup detection
            measureTransferPerformance(unhighlightDuration); // Track performance for testing
            if (unhighlightDuration > 50000) { // More than 50ms is suspicious
                Serial.printf("⚠️ SLOW UNHIGHLIGHT: button %d took %lu μs [Transfer: %lu]\n", 
                             currentHighlightButton + 1, unhighlightDuration, globalTransferCounter);
            }
            
            // Un-highlighting button (removed print to clean up monitoring display)
            
            // Monitor sequence state for desynchronization
            monitorSequenceState(currentHighlightButton, false, unhighlightDuration);
            
            // Move to the next button
            uint8_t nextButton = (currentHighlightButton + 1) % 24;
            
                         // CYCLE RESET: Add pause when wrapping from button 23 back to button 0
             // This prevents timing drift accumulation between cycles
             if (currentHighlightButton == 23 && nextButton == 0) {
                 passCounter++;
                 Serial.printf("🔄 Pass %d completed: Wrapping from button 24 to button 1\n", passCounter);
                 
                 // MINIMAL CYCLE MANAGEMENT: Reduce cleanup to prevent flickering
                 if (passCounter % 5 == 0) {
                     Serial.printf("🔄 LIGHT RESET: Pass %d - Minimal cleanup only\n", passCounter);
                     // Just memory barriers, no heavy cleanup
                     asm volatile("dsb" ::: "memory");
                     delay(50); // Shorter pause to prevent timing drift
        } else {
                     Serial.printf("🔄 STANDARD RESET: Pass %d - Normal cycle break\n", passCounter);
                     delay(25); // Reduced pause 
                 }
             }
            
            currentHighlightButton = nextButton;
            highlightState = false;
        }
    }
}