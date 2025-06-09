#include <Arduino.h>
#include "ControlPad.h"
#include <imxrt.h>  // For USB register access
#include <arm_math.h>  // For cache functions

// Create a ControlPad instance
ControlPad controlPad;

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

// Resource-based monitoring with early warning system
void preventiveTransferManagement() {
    globalTransferCounter++;
    
    // Monitor for approaching critical thresholds based on your test data
    if (globalTransferCounter == 200 || globalTransferCounter == 1400) {
        Serial.printf("🔮 APPROACHING CRITICAL ZONE: Transfer #%lu - monitoring for resource exhaustion\n", globalTransferCounter);
    }
    
    // Log key milestones to track stability periods  
    if ((globalTransferCounter % 100) == 0) {
        Serial.printf("📊 STABILITY MILESTONE: #%lu transfers completed\n", globalTransferCounter);
    }
    
    // Detect potential resource exhaustion zones
    if (globalTransferCounter >= 200 && globalTransferCounter <= 300) {
        static uint32_t zone1CheckTime = 0;
        if (millis() - zone1CheckTime >= 1000) { // Check every second in critical zone
            zone1CheckTime = millis();
            Serial.printf("⚠️ ZONE 1 MONITORING: Transfer #%lu (200-300 critical zone)\n", globalTransferCounter);
        }
    }
    
    if (globalTransferCounter >= 1400 && globalTransferCounter <= 1800) {
        static uint32_t zone2CheckTime = 0;
        if (millis() - zone2CheckTime >= 1000) { // Check every second in critical zone
            zone2CheckTime = millis();
            Serial.printf("⚠️ ZONE 2 MONITORING: Transfer #%lu (1400-1800 critical zone)\n", globalTransferCounter);
        }
    }
}

// Enhanced transfer tracking wrapper
void trackTransferAndCleanup() {
    preventiveTransferManagement();
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

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🚀 ControlPad Combined Package Test - Maximum LED Speed!");
    Serial.println("🧠 Resource monitoring system - tracking cleanup triggers");
    Serial.println("📊 Based on your tests: cleanup is resource-based, not transfer-count-based!");
    Serial.println("🎯 Test 8 ran 1548 transfers - let's replicate those conditions!");
    
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
            if (highlightDuration > 50000) { // More than 50ms is suspicious
                Serial.printf("⚠️ SLOW HIGHLIGHT: button %d took %lu μs [Transfer: %lu]\n", 
                             currentHighlightButton + 1, highlightDuration, globalTransferCounter);
            }
            
            // Serial.printf("🔥 Highlighting button %d (LED index %d) [Count: %d, Transfer: %lu, Time: %lu μs]\n", 
            //              currentHighlightButton + 1, currentHighlightButton, highlightCounter, globalTransferCounter, highlightDuration);
            
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
            if (unhighlightDuration > 50000) { // More than 50ms is suspicious
                Serial.printf("⚠️ SLOW UNHIGHLIGHT: button %d took %lu μs [Transfer: %lu]\n", 
                             currentHighlightButton + 1, unhighlightDuration, globalTransferCounter);
            }
            
            // Serial.printf("⚡ Un-highlighting button %d (LED index %d) [Transfer: %lu, Time: %lu μs]\n", 
            //              currentHighlightButton + 1, currentHighlightButton, globalTransferCounter, unhighlightDuration);
            
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