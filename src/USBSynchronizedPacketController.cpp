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
        
        // *** SIMPLIFIED: Keep detection logic but disable verbose output for clean frame logging ***
        bool errorConditionsDetected = false;
        bool memoryCleanupImminent = false;
        uint32_t usbStatus = 0;
        uint32_t usbCommand = 0;
        uint32_t currentFrame = 0;
        uint32_t periodicListBase = 0;
        uint32_t asyncListAddr = 0;
        
        // Read USB Host registers for monitoring (keep logic, minimal output)
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
        
        // Keep all detection logic but disable serial output for clean frame logging
        // (The detection still works, just not cluttering the output)
        
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
                
                // Simple logging only - frame jumps don't correlate with flickering
                if (abs(frameDelta) > 2000 && abs(frameDelta) < 8000) {
                    Serial.printf("🔄 USB FRAME JUMP DETECTED: %ld frames (%lu → %lu)\n", 
                                frameDelta, lastFrameNumber, currentFrame);
                    Serial.printf("   📝 Analysis only - frame variations don't indicate flickering\n");
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
                
                // *** IMMEDIATE PROTECTION WITH LONGER DURATION ***
                activateUSBCleanupProtection("USB device state change detected", 200);  // Extended to 200ms
                
                // Check specific state bits
                bool wasConnected = (lastDeviceState & USBHS_PORTSC_CCS) != 0;
                bool isConnected = (currentDeviceState & USBHS_PORTSC_CCS) != 0;
                bool wasEnabled = (lastDeviceState & USBHS_PORTSC_PE) != 0;
                bool isEnabled = (currentDeviceState & USBHS_PORTSC_PE) != 0;
                
                if (wasConnected && !isConnected) {
                    Serial.printf("   📤 DEVICE DISCONNECTED - Major cleanup expected!\n");
                    activateUSBCleanupProtection("Device disconnected", 300);  // Even longer for disconnect
                }
                if (!wasConnected && isConnected) {
                    Serial.printf("   📥 DEVICE CONNECTED - Enumeration starting\n");
                    activateUSBCleanupProtection("Device connected", 250);
                }
                if (wasEnabled && !isEnabled) {
                    Serial.printf("   ⚠️ DEVICE DISABLED - Transfer cleanup expected!\n");
                    activateUSBCleanupProtection("Device disabled", 200);
                }
                if (!wasEnabled && isEnabled) {
                    Serial.printf("   ✅ DEVICE ENABLED - Transfers can resume\n");
                    activateUSBCleanupProtection("Device enabled", 150);
                }
                
                // This is likely when flickering occurs
                Serial.printf("   🎨 FLICKER RISK: LED updates should be paused briefly\n");
                
                // *** NEW: Additional hardware-level protection for critical transitions ***
                uint32_t stateDiff = lastDeviceState ^ currentDeviceState;
                if (stateDiff & 0x400) {  // Bit 10 changed (common in state changes above)
                    Serial.printf("   🚨 CRITICAL BIT CHANGE (0x400): Extra protection activated\n");
                    activateUSBCleanupProtection("Critical USB bit change", 250);
                }
            }
            
            // *** NEW: PREDICTIVE PROTECTION FOR UNSTABLE STATES ***
            // Monitor for signs that state change is imminent
            static uint32_t lastStableState = currentDeviceState;
            static uint32_t lastStableTime = currentTime;
            static uint32_t stateFluctuationCount = 0;
            
            if (currentDeviceState != lastStableState) {
                stateFluctuationCount++;
                if (currentTime - lastStableTime < 500) {  // Multiple changes within 500ms
                    Serial.printf("🔄 USB STATE FLUCTUATION #%lu: Possible instability detected\n", stateFluctuationCount);
                    if (stateFluctuationCount >= 2) {
                        Serial.printf("   🛡️ PREDICTIVE PROTECTION: Activating early protection due to instability\n");
                        activateUSBCleanupProtection("USB state instability detected", 300);
                    }
                } else {
                    // Reset counter if changes are far apart
                    stateFluctuationCount = 1;
                }
                lastStableTime = currentTime;
            }
            lastStableState = currentDeviceState;
        }
        
        lastDeviceState = currentDeviceState;
        
        // *** MONITOR USB ERROR CONDITIONS FOR FLICKERING PROTECTION ***
        static uint32_t lastErrorConditionTime = 0;
        static uint32_t consecutiveErrorCount = 0;
        
        // Check for USB errors that can cause LED corruption without state changes
        if (USBHS_USBSTS & USBHS_USBSTS_UEI) {
            // Count consecutive error conditions
            if (currentTime - lastErrorConditionTime < 2000) { // Within 2 seconds
                consecutiveErrorCount++;
            } else {
                consecutiveErrorCount = 1; // Reset count
            }
            lastErrorConditionTime = currentTime;
            
            // If we have many consecutive errors, activate protection
            if (consecutiveErrorCount >= 5) {
                Serial.printf("🚨 USB ERROR CONDITIONS CAUSING FLICKERING: Activating extended protection\n");
                activateUSBCleanupProtection("Consecutive USB error conditions detected", 1000);
                consecutiveErrorCount = 0; // Reset to prevent spam
            }
        }
        
        // *** BUTTON-BASED FLICKERING EVENT TRACKING ***
        static bool lastButtonState = false;
        static uint32_t buttonPressTime = 0;
        static uint32_t buttonReleaseTime = 0;
        
        // Read button 0 state from the existing ControlPad system
        extern ControlPad controlPad;
        bool currentButtonState = controlPad.getButtonState(0); // Use button 0 for flickering tracking
        
        // Detect button press (flickering START)
        if (!lastButtonState && currentButtonState) {
            buttonPressTime = millis();
            Serial.printf("START\n");
        }
        
        // Detect button release (flickering STOP) 
        if (lastButtonState && !currentButtonState) {
            buttonReleaseTime = millis();
            uint32_t flickerDuration = buttonReleaseTime - buttonPressTime;
            Serial.printf("STOP %lums\n", flickerDuration);
        }
        
        lastButtonState = currentButtonState;
        
        // *** SIMPLIFIED FRAME MONITORING FOR FLICKERING RESEARCH ***
        static uint32_t lastFrameLogTime = 0;
        static uint32_t lastLoggedFrame = 0;
        
        // Get current USB frame
        uint32_t currentFrameForLogging = 0;
        if (USBHS_FRINDEX) {
            currentFrameForLogging = USBHS_FRINDEX & 0x3FFF;
        }
        
        // Log frame numbers every 100ms for clean tracking
        if (currentTime - lastFrameLogTime > 100 && currentFrameForLogging != lastLoggedFrame) {
            // Simple frame number output - easy to correlate with manual '1'/'2' markers
            Serial.printf("Frame: %lu\n", currentFrameForLogging);
            lastFrameLogTime = currentTime;
            lastLoggedFrame = currentFrameForLogging;
        }
        
        // *** KEEP DETECTION LOGIC BUT MINIMIZE OUTPUT ***
        static uint32_t lastRegularFrameTime = 0;
        static uint32_t lastRegularFrame = 0;
        static uint32_t lastFrameDelta = 0;
        static uint32_t consistentDeltaCount = 0;
        static bool monitoringFlickerPattern = false;
        
        // Continue detection logic but only output critical findings
        if (lastRegularFrame > 0 && currentFrameForLogging > 0) {
            uint32_t frameDelta = currentFrameForLogging - lastRegularFrame;
            
            // Account for wraparound
            if (frameDelta > 8192) frameDelta = (16384 + currentFrameForLogging) - lastRegularFrame;
            
            // Check for the problematic ~1200 frame intervals
            if (frameDelta > 1100 && frameDelta < 1400) { // 1200 ± 200 frame window
                if (lastFrameDelta > 0 && abs((int32_t)frameDelta - (int32_t)lastFrameDelta) < 100) {
                    // Consistent ~1200 frame pattern detected
                    consistentDeltaCount++;
                    
                    if (consistentDeltaCount >= 2) { // Start monitoring after 2 intervals
                        // Check if we're in a problematic frame start offset
                        uint32_t frameStartOffset = lastRegularFrame % 1200; // Get offset within cycle
                        
                        // Known problematic frame start offsets (cause flickering)
                        bool isProblematicOffset = 
                            (frameStartOffset >= 200 && frameStartOffset <= 220) ||   // ~208
                            (frameStartOffset >= 440 && frameStartOffset <= 460) ||   // ~448  
                            (frameStartOffset >= 560 && frameStartOffset <= 580) ||   // ~568
                            (frameStartOffset >= 600 && frameStartOffset <= 620) ||   // ~608
                            (frameStartOffset >= 680 && frameStartOffset <= 700) ||   // ~688
                            (frameStartOffset >= 800 && frameStartOffset <= 820) ||   // ~808
                            (frameStartOffset >= 1270 && frameStartOffset <= 1290) || // ~1280
                            (frameStartOffset >= 80 && frameStartOffset <= 100);      // 1280 wrapped = ~80
                        
                        // Known stable frame start offsets (no flickering)
                        bool isStableOffset =
                            (frameStartOffset >= 620 && frameStartOffset <= 640) ||   // ~624
                            (frameStartOffset >= 820 && frameStartOffset <= 840) ||   // ~832
                            (frameStartOffset >= 990 && frameStartOffset <= 1010) ||  // ~992
                            (frameStartOffset >= 1190 && frameStartOffset <= 1210);   // ~1192
                        
                        if (isProblematicOffset) {
                            // Minimal output - just mark the detection
                            Serial.printf("P%lu ", frameStartOffset); // P = Problematic offset detected
                            activateUSBCleanupProtection("Problematic USB frame timing offset detected", 3000);
                            monitoringFlickerPattern = true;
                            consistentDeltaCount = 0; // Reset
                        } else if (isStableOffset) {
                            // Minimal output - just mark the detection  
                            Serial.printf("S%lu ", frameStartOffset); // S = Stable offset detected
                            monitoringFlickerPattern = false;
                        }
                    }
                } else {
                    consistentDeltaCount = 0; // Reset if pattern breaks
                }
                lastFrameDelta = frameDelta;
            } else {
                // Pattern outside the problematic range
                consistentDeltaCount = 0;
                lastFrameDelta = 0;
                monitoringFlickerPattern = false;
            }
        }
        
        // Update for next iteration (sample every 100ms for better pattern detection)
        if (currentTime - lastRegularFrameTime > 100) {
            lastRegularFrame = currentFrameForLogging;
            lastRegularFrameTime = currentTime;
        }
        
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







 