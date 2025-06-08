#include "USBSynchronizedPacketController.h"

// Static instance definition
USBSynchronizedPacketController USBSynchronizedPacketController::staticInstance;

// Global instance definition  
USBSynchronizedPacketController usbSyncController;

void USBSynchronizedPacketController::initialize() {
    // Initialize simple coordination system
    buttonActivityDetected = false;
    lastButtonActivityTime = 0;
    
    // Basic statistics
    totalPacketsSent = 0;
    packetsInCurrentBurst = 0;
    lastStatusLogTime = 0;
    currentPacket = 0;
    
    Serial.println("🔧 Simple Button/LED Coordinator initialized");
    Serial.printf("   Button quiet period: %dms\n", BUTTON_QUIET_PERIOD_MS);
}

void USBSynchronizedPacketController::notifyButtonActivity() {
    // **MUCH LIGHTER COORDINATION** - Only pause briefly if not already paused
    if (!buttonActivityDetected) {
        buttonActivityDetected = true;
        lastButtonActivityTime = millis();
        
        // Much shorter pause and only log the first one in a sequence
        Serial.printf("🎮 Button activity - brief LED pause (%dms)\n", BUTTON_QUIET_PERIOD_MS);
    }
    // Don't reset timer on subsequent button events - let it expire naturally
}

bool USBSynchronizedPacketController::isButtonQuietPeriod() {
    if (!buttonActivityDetected) return true;  // No recent button activity
    
    uint32_t timeSinceActivity = millis() - lastButtonActivityTime;
    if (timeSinceActivity >= BUTTON_QUIET_PERIOD_MS) {
        buttonActivityDetected = false;  // Clear the flag
        Serial.printf("✅ Button quiet period ended (%dms) - LED updates resumed\n", timeSinceActivity);
        return true;  // Quiet period has passed
    }
    
    return false;  // Still in button activity period
}

bool USBSynchronizedPacketController::isSafeToSendPacket() {
    // **SIMPLE COORDINATION** - Avoid LED/button conflicts
    if (!isButtonQuietPeriod()) {
        static uint32_t lastBlockedLog = 0;
        uint32_t now = millis();
        if (now - lastBlockedLog >= 100) {  // Log every 100ms during blocking
            Serial.printf("🚫 LED update blocked - button quiet period active (%dms remaining)\n", 
                         BUTTON_QUIET_PERIOD_MS - (now - lastButtonActivityTime));
            lastBlockedLog = now;
        }
        return false;  // Block LED packets during button activity
    }
    
    // Basic burst limiting
    if (packetsInCurrentBurst >= MAX_PACKETS_PER_BURST) {
        packetsInCurrentBurst = 0;  // Reset counter
    }
    
    return true;  // Safe to send LED packets
}

void USBSynchronizedPacketController::monitorUSBActivity() {
    uint32_t currentTime = millis();
    
    // *** ENHANCED HARDWARE REGISTER MONITORING FOR followup_Error() PREDICTION ***
    static uint32_t lastErrorCheck = 0;
    if (currentTime - lastErrorCheck >= 50) {  // Check every 50ms for faster response
        
        bool errorConditionsDetected = false;
        bool memoryCleanupImminent = false;
        uint32_t usbStatus = 0;
        uint32_t usbCommand = 0;
        uint32_t currentFrame = 0;
        uint32_t periodicListBase = 0;
        uint32_t asyncListAddr = 0;
        
        // Read USB Host registers for comprehensive monitoring
        if (USBHS_USBSTS) {
            usbStatus = USBHS_USBSTS;
        }
        if (USBHS_USBCMD) {
            usbCommand = USBHS_USBCMD;
        }
        if (USBHS_FRINDEX) {
            currentFrame = USBHS_FRINDEX & 0x3FFF;
        }
        if (USBHS_PERIODICLISTBASE) {
            periodicListBase = USBHS_PERIODICLISTBASE;
        }
        if (USBHS_ASYNCLISTADDR) {
            asyncListAddr = USBHS_ASYNCLISTADDR;
        }
        
        // *** SPECIFIC followup_Error() TRIGGER CONDITIONS ***
        
        // 1. Check for actual USB Error Interrupt (UEI) bit - IMMEDIATE followup_Error()
        if (usbStatus & 0x02) {  // USBHS_USBSTS_UEI bit
            Serial.printf("🚨 USB ERROR INTERRUPT ACTIVE! followup_Error() will be called NOW\n");
            Serial.printf("   💾 MEMORY CLEANUP: Transfer descriptors being freed\n");
            errorConditionsDetected = true;
            memoryCleanupImminent = true;
        }
        
        // 2. Check for Port Change Detect - triggers device re-enumeration
        if (usbStatus & 0x04) {  // USBHS_USBSTS_PCI bit
            Serial.printf("🔌 PORT CHANGE DETECTED! Device enumeration/cleanup in progress\n");
            Serial.printf("   💾 MEMORY CLEANUP: Device descriptors being reallocated\n");
            errorConditionsDetected = true;
            memoryCleanupImminent = true;
        }
        
        // 3. Check for System Error (SEI) - serious USB issues requiring cleanup
        if (usbStatus & 0x10) {  // USBHS_USBSTS_SEI bit
            Serial.printf("🚨 USB SYSTEM ERROR! followup_Error() and memory cleanup required\n");
            Serial.printf("   💾 MEMORY CLEANUP: Full USB stack reset imminent\n");
            errorConditionsDetected = true;
            memoryCleanupImminent = true;
        }
        
        // 4. Check for Async Schedule Advance - indicates queue management
        if (usbStatus & 0x20) {  // USBHS_USBSTS_AAI bit
            Serial.printf("🔄 ASYNC SCHEDULE ADVANCE! Queue management in progress\n");
            Serial.printf("   💾 MEMORY CLEANUP: Transfer queues being reorganized\n");
            memoryCleanupImminent = true;
        }
        
        // 5. Monitor for Host Controller Reset (indicates major cleanup)
        static uint32_t lastCommand = 0;
        if ((lastCommand & 0x02) == 0 && (usbCommand & 0x02) != 0) {
            Serial.printf("🔄 HOST CONTROLLER RESET INITIATED! Major memory cleanup starting\n");
            Serial.printf("   💾 MEMORY CLEANUP: All USB structures being cleared\n");
            errorConditionsDetected = true;
            memoryCleanupImminent = true;
        }
        lastCommand = usbCommand;
        
        // 6. Check for USB Host Controller halt (Run/Stop bit = 0)
        if ((usbCommand & 0x01) == 0) {
            Serial.printf("🚨 USB HOST HALTED! Controller in recovery mode\n");
            Serial.printf("   💾 MEMORY CLEANUP: Pending transfers being aborted\n");
            errorConditionsDetected = true;
            memoryCleanupImminent = true;
        }
        
        // 7. Monitor transfer completion patterns for stalls
        static uint32_t consecutiveStalls = 0;
        static uint32_t lastFrameActivity = 0;
        
        if (usbStatus & 0x01) {  // USB Interrupt (UI) bit - transfer completed
            lastFrameActivity = currentFrame;
            consecutiveStalls = 0;
        } else {
            uint32_t framesSinceActivity = currentFrame - lastFrameActivity;
            if (framesSinceActivity > 100) {  // No activity for ~100ms (800 frames at 8KHz)
                consecutiveStalls++;
                if (consecutiveStalls >= 3) {  // Multiple checks confirming stall
                    Serial.printf("⚠️ USB TRANSFER STALL detected - followup_Error() may be needed soon\n");
                    Serial.printf("   📊 Frames since activity: %lu (stall count: %lu)\n", framesSinceActivity, consecutiveStalls);
                    errorConditionsDetected = true;
                    consecutiveStalls = 0;  // Reset to avoid spam
                }
            }
        }
        
        // 8. Monitor Schedule Status for queue issues
        static bool lastPeriodicEnabled = false;
        static bool lastAsyncEnabled = false;
        bool periodicEnabled = (usbStatus & 0x4000) != 0;  // Periodic Schedule Status
        bool asyncEnabled = (usbStatus & 0x8000) != 0;     // Async Schedule Status
        
        if (lastPeriodicEnabled && !periodicEnabled) {
            Serial.printf("📋 PERIODIC SCHEDULE DISABLED! Cleanup of periodic transfers\n");
            Serial.printf("   💾 MEMORY CLEANUP: Periodic transfer descriptors freed\n");
            memoryCleanupImminent = true;
        }
        
        if (lastAsyncEnabled && !asyncEnabled) {
            Serial.printf("📋 ASYNC SCHEDULE DISABLED! Cleanup of async transfers\n");
            Serial.printf("   💾 MEMORY CLEANUP: Async transfer descriptors freed\n");
            memoryCleanupImminent = true;
        }
        
        lastPeriodicEnabled = periodicEnabled;
        lastAsyncEnabled = asyncEnabled;
        
        // 9. Monitor for Doorbell operations (manual queue advance)
        static uint32_t lastAsyncListAddr = 0;
        if (asyncListAddr != lastAsyncListAddr && asyncListAddr != 0) {
            Serial.printf("🚪 ASYNC QUEUE DOORBELL! Manual queue advance detected\n");
            Serial.printf("   💾 MEMORY CLEANUP: Queue pointers being updated (0x%08lX → 0x%08lX)\n", 
                         lastAsyncListAddr, asyncListAddr);
            memoryCleanupImminent = true;
        }
        lastAsyncListAddr = asyncListAddr;
        
        // *** MEMORY CLEANUP PREDICTION AND LOGGING ***
        if (memoryCleanupImminent) {
            Serial.printf("🔥 MEMORY CLEANUP IMMINENT! followup_Error() will free descriptors\n");
            Serial.printf("   📊 USB State: Status=0x%08lX, Command=0x%08lX, Frame=%lu\n", 
                         usbStatus, usbCommand, currentFrame);
            Serial.printf("   🔗 Queues: Periodic=0x%08lX, Async=0x%08lX\n", 
                         periodicListBase, asyncListAddr);
            
            // Predict what memory will be freed
            if (usbStatus & 0x02) Serial.printf("   💾 Will free: Error transfer descriptors\n");
            if (usbStatus & 0x04) Serial.printf("   💾 Will free: Device enumeration structures\n");
            if (usbStatus & 0x10) Serial.printf("   💾 Will free: Complete USB stack memory\n");
            if (usbStatus & 0x20) Serial.printf("   💾 Will free: Async queue elements\n");
        }
        
        // *** SUMMARY LOGGING ***
        if (errorConditionsDetected) {
            Serial.printf("🔥 USB ERROR CONDITIONS ACTIVE - followup_Error() imminent!\n");
            Serial.printf("   Status: 0x%08lX, Command: 0x%08lX, Frame: %lu\n", 
                         usbStatus, usbCommand, currentFrame);
        }
        
        lastErrorCheck = currentTime;
    }
    
    // *** CONTINUOUS MEMORY MONITORING ***
    trackMemoryCleanup();
    
    // **STATUS REPORTING** - Every 30 seconds
    if (currentTime - lastStatusLogTime >= 30000) {
        Serial.printf("📊 Error Monitor: %lu packets sent, USB status checks active\n", totalPacketsSent);
        lastStatusLogTime = currentTime;
    }
}

void USBSynchronizedPacketController::recordPacketSent() {
    // Record that a packet was successfully sent
    totalPacketsSent++;
    packetsInCurrentBurst++;
    
    // **REMOVED OLD LOGGING** - Much simpler tracking
    currentPacket++;
}

// *** NEW: Memory cleanup tracking functions ***
void USBSynchronizedPacketController::trackMemoryCleanup() {
    // Track the state of USB memory pools and descriptors
    uint32_t currentTime = millis();
    static uint32_t lastMemoryCheck = 0;
    
    // *** ENHANCED MEMORY TRACKING WITH ACTUAL CLEANUP DETECTION ***
    if (currentTime - lastMemoryCheck >= 100) {  // Check every 100ms
        
        // Monitor free memory in different pools
        #ifdef __MK66FX1M0__  // Teensy 3.6/4.x specific
        uint32_t freeRAM = getFreeRAM();
        static uint32_t lastFreeRAM = 0;
        static uint32_t memoryCleanupCount = 0;
        static uint32_t lastSignificantChange = 0;
        
        // Detect significant memory changes (actual cleanup events)
        if (lastFreeRAM > 0) {
            int32_t memoryChange = (int32_t)freeRAM - (int32_t)lastFreeRAM;
            
            // Detect major memory cleanup (> 1KB freed)
            if (memoryChange > 1024) {
                memoryCleanupCount++;
                Serial.printf("🧹 MEMORY CLEANUP DETECTED! +%d bytes freed (cleanup #%lu)\n", 
                            memoryChange, memoryCleanupCount);
                Serial.printf("   📊 RAM: %lu → %lu bytes free\n", lastFreeRAM, freeRAM);
                lastSignificantChange = currentTime;
            }
            
            // Detect memory allocation (potential problem)
            else if (memoryChange < -512) {
                Serial.printf("⚠️ LARGE MEMORY ALLOCATION: %d bytes allocated\n", -memoryChange);
                Serial.printf("   📊 RAM: %lu → %lu bytes free\n", lastFreeRAM, freeRAM);
            }
            
            // Detect gradual memory leak
            static uint32_t memoryTrendCount = 0;
            static int32_t memoryTrend = 0;
            memoryTrend += memoryChange;
            memoryTrendCount++;
            
            if (memoryTrendCount >= 10) {  // Every 10 checks (1 second)
                if (memoryTrend < -2048) {  // Losing > 2KB over 1 second
                    Serial.printf("🚨 MEMORY LEAK DETECTED: -%d bytes trend over 1 second\n", -memoryTrend);
                }
                memoryTrend = 0;
                memoryTrendCount = 0;
            }
        }
        
        lastFreeRAM = freeRAM;
        #endif
        
        // Track USB descriptor/transfer pool status
        static uint32_t lastPoolCheck = 0;
        if (currentTime - lastPoolCheck >= 500) {  // Every 500ms
            
            // Monitor USB transfer completion patterns
            static uint32_t lastFrameNumber = 0;
            uint32_t currentFrame = 0;
            if (USBHS_FRINDEX) {
                currentFrame = USBHS_FRINDEX & 0x3FFF;  // 14-bit frame counter
            }
            
            // Detect frame counter jumps (indicates USB reset/cleanup)
            if (lastFrameNumber > 0 && currentFrame > 0) {
                int32_t frameDelta = (int32_t)currentFrame - (int32_t)lastFrameNumber;
                
                // Account for wraparound (frames wrap at 16384)
                if (frameDelta < -8192) frameDelta += 16384;
                if (frameDelta > 8192) frameDelta -= 16384;
                
                // Detect suspicious frame jumps - LOG ONLY FOR ANALYSIS
                if (abs(frameDelta) > 2000 && abs(frameDelta) < 8000) {
                    Serial.printf("🔄 USB FRAME JUMP DETECTED: %ld frames (%lu → %lu)\n", 
                                frameDelta, lastFrameNumber, currentFrame);
                    Serial.printf("   📝 Analysis only - not triggering protection\n");
                }
            }
            
            lastFrameNumber = currentFrame;
            lastPoolCheck = currentTime;
        }
        
        // *** MONITOR USB DEVICE STATE CHANGES ***
        static uint32_t lastDeviceState = 0xFFFFFFFF;  // Initialize to invalid state
        static uint32_t stateCheckCount = 0;
        uint32_t currentDeviceState = 0;
        
        // Check USB device configuration status
        if (USBHS_PORTSC1) {
            currentDeviceState = USBHS_PORTSC1;
            stateCheckCount++;
            
            // Debug removed - was causing timing interference with LED updates
            
            if (lastDeviceState != 0xFFFFFFFF && currentDeviceState != lastDeviceState) {
                // Device state changed - this often triggers cleanup
                Serial.printf("🔌 USB DEVICE STATE CHANGE: 0x%08lX → 0x%08lX\n", 
                            lastDeviceState, currentDeviceState);
                
                // *** ALWAYS ACTIVATE PROTECTION FOR ANY DEVICE STATE CHANGE ***
                activateUSBCleanupProtection("USB device state change detected");
                
                // Check specific state bits
                bool wasConnected = (lastDeviceState & USBHS_PORTSC_CCS) != 0;
                bool isConnected = (currentDeviceState & USBHS_PORTSC_CCS) != 0;
                bool wasEnabled = (lastDeviceState & USBHS_PORTSC_PE) != 0;
                bool isEnabled = (currentDeviceState & USBHS_PORTSC_PE) != 0;
                
                if (wasConnected && !isConnected) {
                    Serial.printf("   📤 DEVICE DISCONNECTED - Major cleanup expected!\n");
                    activateUSBCleanupProtection("Device disconnected");
                }
                if (!wasConnected && isConnected) {
                    Serial.printf("   📥 DEVICE CONNECTED - Enumeration starting\n");
                    activateUSBCleanupProtection("Device connected");
                }
                if (wasEnabled && !isEnabled) {
                    Serial.printf("   ⚠️ DEVICE DISABLED - Transfer cleanup expected!\n");
                    activateUSBCleanupProtection("Device disabled");
                }
                if (!wasEnabled && isEnabled) {
                    Serial.printf("   ✅ DEVICE ENABLED - Transfers can resume\n");
                    activateUSBCleanupProtection("Device enabled");
                }
                
                // This is likely when flickering occurs
                Serial.printf("   🎨 FLICKER RISK: LED updates should be paused briefly\n");
            }
        }
        
        lastDeviceState = currentDeviceState;
        
        lastMemoryCheck = currentTime;
    }
}

void USBSynchronizedPacketController::onFollowupErrorCalled() {
    // Called when followup_Error() is actually invoked
    static uint32_t followupErrorCount = 0;
    followupErrorCount++;
    
    Serial.printf("🚨 followup_Error() CALLED! Event #%lu\n", followupErrorCount);
    Serial.printf("   💾 USB memory cleanup in progress...\n");
    Serial.printf("   📊 Timing: %lu ms since startup\n", millis());
    
    // Track what happens after followup_Error()
    trackMemoryCleanup();
}

#ifdef __MK66FX1M0__
// Helper function to get free RAM on Teensy
uint32_t USBSynchronizedPacketController::getFreeRAM() {
    extern char _heap_start;
    extern char *__brkval;
    char *heap_end = __brkval;
    if (heap_end == nullptr) heap_end = &_heap_start;
    return ((char*)__get_MSP()) - heap_end;
}
#endif

// *** ENHANCED: USB Cleanup Protection Flag ***
static bool usbCleanupProtectionActive = false;
static uint32_t cleanupProtectionStartTime = 0;

bool USBSynchronizedPacketController::isUSBCleanupActive() {
    uint32_t currentTime = millis();
    
    // Clear protection flag after timeout (100ms should be enough for device state cleanup)
    if (usbCleanupProtectionActive && (currentTime - cleanupProtectionStartTime > 100)) {
        usbCleanupProtectionActive = false;
        Serial.printf("✅ USB cleanup protection CLEARED - LED updates can resume\n");
    }
    
    return usbCleanupProtectionActive;
}

void USBSynchronizedPacketController::activateUSBCleanupProtection(const char* reason) {
    uint32_t currentTime = millis();
    
    if (!usbCleanupProtectionActive) {
        usbCleanupProtectionActive = true;
        cleanupProtectionStartTime = currentTime;
        Serial.printf("🛡️ USB CLEANUP PROTECTION ACTIVATED: %s\n", reason);
        Serial.printf("   🎨 LED updates PAUSED for ~100ms\n");
    } else {
        // Extend protection period if already active
        cleanupProtectionStartTime = currentTime;
        Serial.printf("🛡️ Protection EXTENDED: %s\n", reason);
    }
}





 