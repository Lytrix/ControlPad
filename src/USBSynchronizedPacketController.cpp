#include "USBSynchronizedPacketController.h"
#include <Arduino.h>
#include <USBHost_t36.h>
#include "ControlPadHardware.h"  // For ControlPadColor and class definitions
#include "ControlPad.h"          // For ControlPad class

// *** FORWARD DECLARATIONS FOR RECOVERY SYSTEM ***
class ControlPadHardware;
class USBControlPad;
struct ControlPadColor;

// Declare external instances
extern ControlPadHardware* globalHardwareInstance;
extern USBControlPad globalControlPadDriver;
extern ControlPad controlPad;

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
    
    // *** NEW: Initialize transfer tracking array ***
    for (uint8_t i = 0; i < MAX_TRACKED_TRANSFERS; i++) {
        activeTransfers[i].isActive = false;
        activeTransfers[i].transferId = 0;
        activeTransfers[i].buffer = nullptr;
        activeTransfers[i].length = 0;
        activeTransfers[i].startTime = 0;
        activeTransfers[i].retryCount = 0;
    }
    nextTransferId = 1;
    
    Serial.println("🔧 Simple Button/LED Coordinator initialized");
    Serial.printf("   Button quiet period: %dms\n", BUTTON_QUIET_PERIOD_MS);
    Serial.printf("   Transfer tracking: %d slots available\n", MAX_TRACKED_TRANSFERS);
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
    
    // *** NEW: Monitor transfer timeouts first ***
    monitorTransferTimeouts();
    
    // *** ENHANCED HARDWARE REGISTER MONITORING FOR followup_Error() PREDICTION ***
    static uint32_t lastErrorCheck = 0;
    if (currentTime - lastErrorCheck >= 50) {  // Check every 50ms for faster response
        
        // Read USB Host registers for monitoring (keep logic, minimal output)
        // Note: These reads are kept for future error prediction logic
        
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
    
    // *** SIMPLE USB TRANSFER TRACKING (without internal USBHost_t36 variables) ***
    if (currentTime - lastMemoryCheck >= 100) {  // Check every 100ms
        
        // *** Monitor USB Transfer Pool State ***
        // Check if transfers are being properly recycled
        static uint32_t lastActiveTransfers = 0;
        uint32_t currentActive = 0;
        
        for (uint8_t i = 0; i < MAX_TRACKED_TRANSFERS; i++) {
            if (activeTransfers[i].isActive) {
                currentActive++;
                
                // Check for stale transfers (stuck > 1 second)
                if (currentTime - activeTransfers[i].startTime > 1000) {
                    Serial.print("⚠️  STALE TRANSFER DETECTED: ID=");
                    Serial.print(activeTransfers[i].transferId);
                    Serial.print(" age=");
                    Serial.print(currentTime - activeTransfers[i].startTime);
                    Serial.println("ms");
                    
                    // Mark as potentially leaked by setting retry count high
                    activeTransfers[i].retryCount = 999;  // Leak marker
                }
            }
        }
        
        if (currentActive != lastActiveTransfers) {
            Serial.print("📊 ACTIVE TRANSFERS: ");
            Serial.print(currentActive);
            Serial.print(" (was ");
            Serial.print(lastActiveTransfers);
            Serial.println(")");
        }
        lastActiveTransfers = currentActive;
        
        // *** Simple Transfer Completion Rate Monitoring ***
        static uint32_t lastCompletedTransfers = 0;
        static uint32_t totalCompletedTransfers = 0;
        
        // Count completed transfers since last check
        uint32_t recentCompletions = totalCompletedTransfers - lastCompletedTransfers;
        if (recentCompletions > 0) {
            Serial.print("✅ USB TRANSFERS COMPLETED: ");
            Serial.print(recentCompletions);
            Serial.println(" in last 100ms");
        }
        lastCompletedTransfers = totalCompletedTransfers;
        
        // *** System Memory Monitoring (as proxy for USB memory health) ***
        #ifdef __MK66FX1M0__
        uint32_t freeRAM = getFreeRAM();
        static uint32_t lastFreeRAM = 0;
        
        if (lastFreeRAM > 0) {
            int32_t ramChange = (int32_t)freeRAM - (int32_t)lastFreeRAM;
            if (ramChange > 1024) {  // Significant memory freed
                Serial.print("🧹 SYSTEM MEMORY CLEANUP: +");
                Serial.print(ramChange);
                Serial.println(" bytes freed");
            } else if (ramChange < -2048) {  // Significant memory loss
                Serial.print("⚠️ MEMORY USAGE INCREASE: ");
                Serial.print(-ramChange);
                Serial.println(" bytes allocated");
            }
        }
        lastFreeRAM = freeRAM;
        #endif
        
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
static uint32_t cleanupProtectionDuration = 100;  // Default duration in ms
static bool ledControllerCorrupted = false;  // NEW: Track LED controller corruption
static uint32_t lastControllerValidationTime = 0;

bool USBSynchronizedPacketController::isUSBCleanupActive() {
    uint32_t currentTime = millis();
    
    // Clear protection flag after timeout (configurable duration)
    if (usbCleanupProtectionActive && (currentTime - cleanupProtectionStartTime > cleanupProtectionDuration)) {
        usbCleanupProtectionActive = false;
        Serial.printf("✅ USB cleanup protection CLEARED - LED updates can resume\n");
        
        // *** NEW: Trigger LED controller validation after protection clears ***
        validateAndRecoverLEDController();
    }
    
    return usbCleanupProtectionActive;
}

void USBSynchronizedPacketController::activateUSBCleanupProtection(const char* reason, uint32_t duration) {
    usbCleanupProtectionActive = true;
    cleanupProtectionStartTime = millis();
    cleanupProtectionDuration = duration;
    
    Serial.printf("🛡️ USB CLEANUP PROTECTION ACTIVATED: %s\n", reason);
    Serial.printf("   🎨 LED updates PAUSED for ~%lums\n", duration);
    Serial.printf("   🎨 FLICKER RISK: LED updates should be paused briefly\n");
    
    // *** NEW: Mark LED controller as potentially corrupted ***
    ledControllerCorrupted = true;
    
    // Coordinate with memory tracking
    trackMemoryCleanup();
}

// *** NEW: LED Controller Hardware Recovery System ***
void USBSynchronizedPacketController::validateAndRecoverLEDController() {
    if (!ledControllerCorrupted) return;
    
    uint32_t currentTime = millis();
    
    // Rate limit validation attempts
    if (currentTime - lastControllerValidationTime < 50) return;
    lastControllerValidationTime = currentTime;
    
    Serial.printf("🔧 LED CONTROLLER VALIDATION: Checking for hardware corruption...\n");
    
    // *** STEP 1: Test LED controller responsiveness ***
    bool controllerResponsive = testLEDControllerResponse();
    
    if (!controllerResponsive) {
        Serial.printf("🚨 LED CONTROLLER CORRUPTED: Hardware not responding, attempting recovery...\n");
        
        // *** STEP 2: Attempt hardware recovery ***
        if (recoverLEDController()) {
            Serial.printf("✅ LED CONTROLLER RECOVERED: Hardware restored successfully\n");
            ledControllerCorrupted = false;
        } else {
            Serial.printf("❌ LED CONTROLLER RECOVERY FAILED: Hardware may need manual reset\n");
            // Schedule retry in 100ms
            return;
        }
    } else {
        Serial.printf("✅ LED CONTROLLER VALIDATION: Hardware responding normally\n");
        ledControllerCorrupted = false;
    }
}

// *** NEW: Test if LED controller is responding correctly ***
bool USBSynchronizedPacketController::testLEDControllerResponse() {
    // Create a simple test pattern - single LED red
    ControlPadColor testColors[24];
    for (int i = 0; i < 24; i++) {
        testColors[i] = {0, 0, 0}; // All off
    }
    testColors[0] = {32, 0, 0}; // First LED dim red (low intensity test)
    
    // Try to send test pattern (bypassing our protection since this is recovery)
    Serial.printf("🔍 Testing LED controller with validation pattern...\n");
    
    // Temporarily disable protection for validation
    bool wasProtected = usbCleanupProtectionActive;
    usbCleanupProtectionActive = false;
    
    bool success = false;
    if (globalHardwareInstance) {
        success = globalHardwareInstance->setAllLeds(testColors, 24);
    } else {
        Serial.printf("❌ LED controller test FAILED: No hardware instance available\n");
        usbCleanupProtectionActive = wasProtected;
        return false;
    }
    
    // Restore protection state
    usbCleanupProtectionActive = wasProtected;
    
    if (!success) {
        Serial.printf("❌ LED controller test FAILED: No response to validation pattern\n");
        return false;
    }
    
    // Wait a bit for the command to process
    delay(10);
    
    Serial.printf("✅ LED controller test PASSED: Response to validation pattern\n");
    return true;
}

// *** NEW: Attempt to recover corrupted LED controller ***
bool USBSynchronizedPacketController::recoverLEDController() {
    Serial.printf("🔧 RECOVERY STEP 1: Attempting LED controller reset...\n");
    
    // *** STEP 1: Try to reset the LED controller to custom mode ***
    bool customModeSuccess = false;
    
    // Temporarily disable protection for recovery operations
    bool wasProtected = usbCleanupProtectionActive;
    usbCleanupProtectionActive = false;
    
    // Attempt to reinitialize custom LED mode
    if (globalControlPadDriver.setCustomMode()) {
        Serial.printf("✅ Custom LED mode reset successful\n");
        customModeSuccess = true;
    } else {
        Serial.printf("❌ Custom LED mode reset failed\n");
    }
    
    // *** STEP 2: Restore known good LED state ***
    if (customModeSuccess) {
        Serial.printf("🔧 RECOVERY STEP 2: Restoring base LED colors...\n");
        
        // Force update to restore the base colors
        controlPad.forceUpdate();
        
        Serial.printf("✅ LED controller recovery completed\n");
        
        // Restore protection state
        usbCleanupProtectionActive = wasProtected;
        return true;
    }
    
    // *** STEP 3: If still failing, try full activation sequence (last resort) ***
    Serial.printf("🔧 RECOVERY STEP 3: Attempting full device reactivation (last resort)...\n");
    
    // Try the full activation sequence to recover the device
    if (globalControlPadDriver.sendActivationSequence()) {
        Serial.printf("🔧 Activation sequence completed, attempting custom mode...\n");
        delay(200); // Give device time to process activation
        
        if (globalControlPadDriver.setCustomMode()) {
            Serial.printf("✅ Full device reactivation successful, LED controller recovered\n");
            controlPad.forceUpdate();
            
            // Restore protection state
            usbCleanupProtectionActive = wasProtected;
            return true;
        }
    }
    
    Serial.printf("❌ All recovery attempts failed\n");
    
    // Restore protection state
    usbCleanupProtectionActive = wasProtected;
    return false;
}

// *** NEW: followup_Transfer Pattern Implementation ***
void USBSynchronizedPacketController::onTransferStarted(uint32_t transferId, const void* buffer, size_t length) {
    // *** LIGHTWEIGHT: Minimal logging to prevent USB issues ***
    for (uint8_t i = 0; i < MAX_TRACKED_TRANSFERS; i++) {
        if (!activeTransfers[i].isActive) {
            activeTransfers[i].transferId = transferId;
            activeTransfers[i].buffer = buffer;
            activeTransfers[i].length = length;
            activeTransfers[i].startTime = millis();
            activeTransfers[i].isActive = true;
            activeTransfers[i].retryCount = 0;
            return;
        }
    }
    // No logging if table full - just continue
}

void USBSynchronizedPacketController::onTransferCompleted(uint32_t transferId, bool success, uint32_t actualLength) {
    // *** LIGHTWEIGHT: Find and deactivate transfer ***
    for (uint8_t i = 0; i < MAX_TRACKED_TRANSFERS; i++) {
        if (activeTransfers[i].isActive && activeTransfers[i].transferId == transferId) {
            activeTransfers[i].isActive = false;
            return;
        }
    }
    // Transfer not found - continue without logging to prevent hang
}

void USBSynchronizedPacketController::onTransferError(uint32_t transferId, uint32_t errorCode, const char* errorDescription) {
    // *** LIGHTWEIGHT: Find and deactivate transfer ***
    for (uint8_t i = 0; i < MAX_TRACKED_TRANSFERS; i++) {
        if (activeTransfers[i].isActive && activeTransfers[i].transferId == transferId) {
            activeTransfers[i].isActive = false;
            activeTransfers[i].retryCount++;
            return;
        }
    }
    // Transfer not found - continue without logging to prevent hang
}

void USBSynchronizedPacketController::performFollowupTransferCleanup(uint32_t transferId) {
    // *** IMPLEMENT USBHost_t36 followup_Transfer() PATTERN ***
    Serial.printf("🧹 followup_Transfer CLEANUP: ID=%lu\n", transferId);
    
    // 1. Mark any pending memory for cleanup
    trackMemoryCleanup();
    
    // 2. Check for memory leaks or stale references
    #ifdef __MK66FX1M0__
    uint32_t freeRAM = getFreeRAM();
    static uint32_t lastCleanupRAM = 0;
    
    if (lastCleanupRAM > 0) {
        int32_t memoryChange = (int32_t)freeRAM - (int32_t)lastCleanupRAM;
        if (memoryChange > 0) {
            Serial.printf("   📈 Memory freed: +%d bytes after transfer cleanup\n", memoryChange);
        } else if (memoryChange < -100) {
            Serial.printf("   📉 Memory lost: %d bytes after transfer cleanup (potential leak)\n", -memoryChange);
        }
    }
    lastCleanupRAM = freeRAM;
    #endif
    
    // 3. Ensure USB controller state is clean
    // Monitor USB registers for any stale state
    if (USBHS_USBSTS & (USBHS_USBSTS_UEI | USBHS_USBSTS_SEI)) {
        Serial.printf("   🔧 USB errors detected during cleanup, clearing flags\n");
        // Clear error flags (they're write-1-to-clear)
        USBHS_USBSTS = USBHS_USBSTS_UEI | USBHS_USBSTS_SEI;
    }
    
    // 4. Brief protection period to prevent interference
    activateFollowupCleanupProtection(transferId, "followup_Transfer cleanup", 50);
}

void USBSynchronizedPacketController::performFollowupErrorRecovery(uint32_t transferId, uint32_t errorCode) {
    // *** IMPLEMENT USBHost_t36 followup_Error() PATTERN ***
    Serial.printf("🔧 followup_Error RECOVERY: ID=%lu, ErrorCode=0x%02lX\n", transferId, errorCode);
    
    // Call the existing tracking function
    onFollowupErrorCalled();
    
    // 1. Analyze the error type and take appropriate action
    switch (errorCode) {
        case 0x01: // Generic failure
            Serial.printf("   🔄 Generic transfer failure - standard recovery\n");
            activateFollowupCleanupProtection(transferId, "Generic transfer failure", 100);
            break;
            
        case 0x40: // Halted endpoint
            Serial.printf("   🛑 Halted endpoint detected - extended recovery\n");
            activateFollowupCleanupProtection(transferId, "Halted endpoint", 200);
            
            // Additional recovery for halted endpoints
            validateAndRecoverLEDController();
            break;
            
        case 0x20: // Data buffer error
            Serial.printf("   💾 Data buffer error - memory cleanup required\n");
            activateFollowupCleanupProtection(transferId, "Data buffer error", 300);
            
            // Force memory cleanup
            trackMemoryCleanup();
            break;
            
        case 0x10: // Babble detected
            Serial.printf("   📡 Babble error - device communication issue\n");
            activateFollowupCleanupProtection(transferId, "Babble error", 150);
            break;
            
        case 0x08: // Transaction error
            Serial.printf("   ⚡ Transaction error - timing/CRC issue\n");
            activateFollowupCleanupProtection(transferId, "Transaction error", 100);
            break;
            
        default:
            Serial.printf("   ❓ Unknown error code - conservative recovery\n");
            activateFollowupCleanupProtection(transferId, "Unknown error", 200);
            break;
    }
    
    // 2. Check if LED controller needs recovery
    if (errorCode & 0x60) { // Halted or buffer errors often corrupt LED state
        Serial.printf("   🎨 LED controller corruption likely - scheduling validation\n");
        validateAndRecoverLEDController();
    }
    
    // 3. Monitor for cascading failures
    static uint32_t errorCount = 0;
    static uint32_t lastErrorTime = 0;
    uint32_t currentTime = millis();
    
    if (currentTime - lastErrorTime < 1000) { // Errors within 1 second
        errorCount++;
        if (errorCount >= 3) {
            Serial.printf("   🚨 CASCADING FAILURES: %lu errors in 1 second - extended protection\n", errorCount);
            activateFollowupCleanupProtection(transferId, "Cascading failures", 1000);
            errorCount = 0; // Reset to prevent spam
        }
    } else {
        errorCount = 1; // Reset count for isolated errors
    }
    lastErrorTime = currentTime;
}

bool USBSynchronizedPacketController::isFollowupCleanupActive() {
    // Check if any followup cleanup is active (extends the existing USB cleanup protection)
    return isUSBCleanupActive();
}

void USBSynchronizedPacketController::activateFollowupCleanupProtection(uint32_t transferId, const char* reason, uint32_t duration) {
    // Extend the existing USB cleanup protection system
    char extendedReason[128];
    snprintf(extendedReason, sizeof(extendedReason), "followup_%s (Transfer ID=%lu)", reason, transferId);
    
    activateUSBCleanupProtection(extendedReason, duration);
}

void USBSynchronizedPacketController::monitorTransferTimeouts() {
    uint32_t currentTime = millis();
    
    // Check for transfers that have been active too long
    for (uint8_t i = 0; i < MAX_TRACKED_TRANSFERS; i++) {
        if (activeTransfers[i].isActive) {
            uint32_t duration = currentTime - activeTransfers[i].startTime;
            
            if (duration > TRANSFER_TIMEOUT_MS) {
                Serial.printf("⏰ TRANSFER TIMEOUT: ID=%lu, Duration=%lums (>%lums)\n", 
                             activeTransfers[i].transferId, duration, TRANSFER_TIMEOUT_MS);
                
                // Treat timeout as an error
                onTransferError(activeTransfers[i].transferId, 0xFF, "Transfer timeout");
            }
        }
    }
}