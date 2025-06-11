#include "ControlPadHardware.h"
#include "ARMTimer.h"
#include <USBHost_t36.h>  // For testing with standard drivers

// Global hardware instance for access from USB callbacks
ControlPadHardware* globalHardwareInstance = nullptr;

// ===== ASYNC LED UPDATE SYSTEM =====

// ===== GLOBAL USB HOST AND DRIVER (USBHost_t36 standard pattern) =====
// Based on working HIDDeviceInfo example - includes all essential drivers
USBHost globalUSBHost;

// Hub support (required for many devices) - these don't interfere with claiming
USBHub hub1(globalUSBHost);
USBHub hub2(globalUSBHost);

// HID parsers (required for HID device enumeration) - these claim USB interfaces
USBHIDParser hid1(globalUSBHost);
USBHIDParser hid2(globalUSBHost);  
USBHIDParser hid3(globalUSBHost);

// Our ControlPad HID Input driver - works WITH the HID parsers
USBControlPad globalControlPadDriver(globalUSBHost);

// Global driver pointer for easy access
USBControlPad* controlPadDriver = &globalControlPadDriver;

// Driver monitoring arrays (like HIDDeviceInfo example)
USBDriver *usbDrivers[] = {&hub1, &hub2, &hid1, &hid2, &hid3};
#define CNT_DEVICES (sizeof(usbDrivers)/sizeof(usbDrivers[0]))
const char * driver_names[CNT_DEVICES] = {"Hub1", "Hub2", "HID1", "HID2", "HID3"};
bool driver_active[CNT_DEVICES] = {false, false, false, false, false};

// HID Input driver monitoring (our ControlPad driver)
USBHIDInput *hidInputDrivers[] = {&globalControlPadDriver};
#define CNT_HID_DEVICES (sizeof(hidInputDrivers)/sizeof(hidInputDrivers[0]))
const char * hid_driver_names[CNT_HID_DEVICES] = {"ControlPad"};
bool hid_driver_active[CNT_HID_DEVICES] = {false};

// ===== HARDWARE DEBUG PINS FOR TIMING ANALYSIS =====
// Use these pins with oscilloscope/logic analyzer to see timing issues
#define DEBUG_PIN_USB_START     14  // Goes HIGH when USB transfer starts
#define DEBUG_PIN_USB_COMPLETE  15  // Goes HIGH when USB transfer completes  
#define DEBUG_PIN_LED_UPDATE    16  // Goes HIGH during LED updates
#define DEBUG_PIN_QUEUE_FULL    17  // Goes HIGH when LED queue is full

static bool debug_pins_initialized = false;

static void initDebugPins() {
    if (debug_pins_initialized) return;
    
    pinMode(DEBUG_PIN_USB_START, OUTPUT);
    pinMode(DEBUG_PIN_USB_COMPLETE, OUTPUT);
    pinMode(DEBUG_PIN_LED_UPDATE, OUTPUT);
    pinMode(DEBUG_PIN_QUEUE_FULL, OUTPUT);
    
    digitalWrite(DEBUG_PIN_USB_START, LOW);
    digitalWrite(DEBUG_PIN_USB_COMPLETE, LOW);
    digitalWrite(DEBUG_PIN_LED_UPDATE, LOW);
    digitalWrite(DEBUG_PIN_QUEUE_FULL, LOW);
    
    debug_pins_initialized = true;
    Serial.println("🔧 Hardware debug pins initialized:");
    Serial.printf("   Pin %d: USB transfer start\n", DEBUG_PIN_USB_START);
    Serial.printf("   Pin %d: USB transfer complete\n", DEBUG_PIN_USB_COMPLETE);
    Serial.printf("   Pin %d: LED update active\n", DEBUG_PIN_LED_UPDATE);
    Serial.printf("   Pin %d: Queue full condition\n", DEBUG_PIN_QUEUE_FULL);
}

// ===== LED COMMAND QUEUE SYSTEM =====
// Now using EventResponder-based queue from header file

// ===== NEW SIMPLIFIED QUEUE SYSTEM =====
static LEDPacketQueue ledQueue;  // Simple packet queue
static LEDTimingController ledTimingController(ledQueue);  // 1ms timing controller

// *** TEMPORARY: Test with slower timing to debug queue issues ***
class TimingControllerSetup {
public:
    TimingControllerSetup() {
        Serial.println("🚀 TimingControllerSetup constructor starting...");
        
        // Set 1ms interval for optimal performance
        ledTimingController.setIntervalMicros(1000);  // 1ms precise timing
        Serial.println("⚡ TIMING: Set interval to 1ms for optimal LED performance");
        
        // Show initial controller state
        Serial.printf("🔧 Timing controller: enabled=%s, queueSize=%d, interval=%dus\n", 
                     ledTimingController.isEnabled() ? "YES" : "NO",
                     ledTimingController.getQueueSize(),
                     5000);
        
        Serial.println("✅ TimingControllerSetup constructor complete");
    }
};
static TimingControllerSetup setupTiming;  // This runs at startup

// ===== USB BANDWIDTH MONITORING WITH QUEUE TRACKING =====
struct USBBandwidthMonitor {
    uint32_t transfers_per_second = 0;
    uint32_t bytes_per_second = 0;
    uint32_t last_report_time = 0;
    uint32_t transfer_count = 0;
    uint32_t byte_count = 0;
    uint32_t last_queue_position = 999; // Track queue wraparound
    
    void recordTransfer(size_t bytes) {
        transfer_count++;
        byte_count += bytes;
        
        // *** ENHANCED QUEUE MONITORING WITH ANOMALY DETECTION ***
        size_t current_queue_size = ledQueue.size();
        // Queue position tracking simplified for EventResponder
        
        // Simplified queue monitoring for atomic queue
        
        // *** LED DATA PAIR TRACKING ***
        // With new split approach: LED data comes in pairs (2), Apply commands are singles
        // Normal queue progression: varies based on LED pairs + individual apply commands
        static uint32_t last_check_time = 0;
        static size_t last_check_size = 0;
        
        // Only check for stuck queues every few seconds to reduce noise
        static ARMIntervalTimer queueCheckTimer;
        static bool queueTimerInit = false;
        if (!queueTimerInit) {
            queueCheckTimer.setIntervalMillis(3000);
            queueCheckTimer.start();
            queueTimerInit = true;
        }
        
        if (queueCheckTimer.hasElapsed()) {
            if (current_queue_size > 0 && last_check_size == current_queue_size) {
                // Debug removed to prevent timing issues
            }
            last_check_size = current_queue_size;
        }
        
        // Queue position tracking removed for simplified queue
        
        // Report bandwidth every 10 seconds (reduced frequency)
        static ARMIntervalTimer reportTimer;
        static bool reportTimerInit = false;
        if (!reportTimerInit) {
            reportTimer.setIntervalMillis(10000);
            reportTimer.start();
            reportTimerInit = true;
        }
        
        if (reportTimer.hasElapsed()) {
            // Reset counters quietly
            transfer_count = 0;
            byte_count = 0;
        }
    }
};

static USBBandwidthMonitor bandwidthMonitor;

// *** UNIFIED LED STATE MANAGER ***
class UnifiedLEDManager {
private:
    static const ControlPadColor BASE_COLORS[24];
    ControlPadColor currentLEDState[24];
    bool buttonStates[24] = {false};
    bool stateChanged = true; // Start with true to initialize LEDs
    uint32_t lastUpdateTime = 0;
    uint32_t animationTime = 0;
    uint8_t animationStep = 0;
    uint8_t lastSentAnimationStep = 255; // Track what was last sent (255 = never sent)
    bool animationEnabled = false;
    
public:
    void setAnimationEnabled(bool enabled) {
        animationEnabled = enabled;
        stateChanged = true; // Force update when animation state changes
    }
    
    bool isAnimationEnabled() const { return animationEnabled; }
    
    void setButtonState(uint8_t buttonIndex, bool pressed) {
        if (buttonIndex < 24 && buttonStates[buttonIndex] != pressed) {
            buttonStates[buttonIndex] = pressed;
            stateChanged = true;
        }
    }
    
    void updateLEDState() {
        // Animation timing is now handled in shouldSendUpdate()
        // Only rebuild LED state if something changed
        if (!stateChanged) return;
        
        // Start with base colors
        for (int i = 0; i < 24; i++) {
            currentLEDState[i] = BASE_COLORS[i];
        }
        
        // TEST: Disable animation highlight to test if static rainbow is stable
        // if (animationEnabled) {
        //     currentLEDState[animationStep] = {255, 255, 255}; // White animation highlight
        // }
        
        // Apply button highlights (override animation on pressed buttons)
        for (int i = 0; i < 24; i++) {
            if (buttonStates[i]) {
                currentLEDState[i] = {255, 255, 255}; // White button highlight
            }
        }
        
        stateChanged = false;
        lastSentAnimationStep = animationStep; // Track what we're about to send
    }
    
    bool shouldSendUpdate() {
        uint32_t currentTime = millis();
        
        // Always send if state changed due to button press/release
        if (stateChanged) return true;
        
        // Send initial update if this is the first time (lastSentAnimationStep == 255)
        if (lastSentAnimationStep == 255) {
            stateChanged = true;
            return true;
        }
        
        // *** ANIMATION TIMING CHECK MOVED HERE FROM updateLEDState() ***
        // Check if animation step should advance (every 100ms - back to moderate speed)
        if (animationEnabled) {
            if (currentTime - animationTime >= 100) {
                uint8_t newAnimationStep = (animationStep + 1) % 24;
                if (newAnimationStep != animationStep) {
                    animationStep = newAnimationStep;
                    animationTime = currentTime;
                    stateChanged = true; // Animation step changed, need update
                    // Animation step logging removed to prevent timing issues
                    return true;
                }
            }
            
            // Also check if we missed sending a previous animation step
            if (animationStep != lastSentAnimationStep) {
                stateChanged = true; // Animation step changed, need to send update
                return true;
            }
        }
        
        // Periodic update even when animation is disabled (every 5 seconds for base colors)
        if (!animationEnabled && (currentTime - lastUpdateTime) > 5000) {
            stateChanged = true;
            return true;
        }
        
        return false;
    }
    
    const ControlPadColor* getLEDState() {
        updateLEDState();
        lastUpdateTime = millis();
        return currentLEDState;
    }
};

// Define the base rainbow colors
const ControlPadColor UnifiedLEDManager::BASE_COLORS[24] = {
    {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
    {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
    {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
    {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
    {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
};

// Global instance
UnifiedLEDManager ledManager;

// *** BUTTON DEBOUNCING: Track last button states and timing ***
// static uint32_t lastButtonStates[24] = {0}; // Timestamp of last state change for each button - unused for now
// static bool currentButtonStates[24] = {false}; // Current pressed state for each button - unused for now
// static const uint32_t DEBOUNCE_DELAY_MS = 50; // 50ms debouncing - unused for now

// ===== USB CALLBACK COMPLETION HANDLING WITH DETAILED DEBUGGING =====
bool USBControlPad::hid_process_out_data(const Transfer_t *transfer) {
    // Called by USBHost_t36 when any output transfer completes
    
    // *** DETAILED USB TRANSFER DEBUGGING ***
    uint32_t token = transfer->qtd.token;
    uint32_t status = token & 0xFF;
    uint32_t pid = (token >> 8) & 3;
    uint32_t length = (token >> 16) & 0x7FFF;
    bool halted = (token & 0x40) != 0;
    bool data_buffer_error = (token & 0x20) != 0;
    bool babble = (token & 0x10) != 0;
    bool transaction_error = (token & 0x08) != 0;
    bool missed_microframe = (token & 0x04) != 0;
    // bool split_transaction_state = (token & 0x02) != 0;  // Unused for now
    // bool ping_state = (token & 0x01) != 0;              // Unused for now
    
    static uint32_t total_transfers = 0;
    static uint32_t failed_transfers = 0;
    static uint32_t last_failure_time = 0;
    
    total_transfers++;
    
    // Detailed failure analysis
    if (status != 0 || halted || data_buffer_error || babble || transaction_error) {
        failed_transfers++;
        last_failure_time = millis();
        
        Serial.printf("❌ USB TRANSFER FAILED #%lu - Status: 0x%02X, Token: 0x%08X\n", 
                     failed_transfers, status, token);
        Serial.printf("   ⚠️ Errors: Halted:%d DBE:%d Babble:%d XactErr:%d MMF:%d\n", 
                     halted, data_buffer_error, babble, transaction_error, missed_microframe);
        Serial.printf("   📊 PID:%d, Length:%d, Time:%lu ms\n", pid, length, millis());
        
        // Decode specific error conditions
        if (halted) Serial.println("   💥 HALTED: Endpoint is stalled");
        if (data_buffer_error) Serial.println("   💥 DATA BUFFER ERROR: Data under/overrun");
        if (babble) Serial.println("   💥 BABBLE: Device sent more data than expected");
        if (transaction_error) Serial.println("   💥 TRANSACTION ERROR: CRC, timeout, etc.");
        if (missed_microframe) Serial.println("   💥 MISSED MICROFRAME: High-speed timing issue");
    }
    
    // All health reporting disabled for clean testing
    
    // *** HARDWARE DEBUG: USB transfer completed ***
    digitalWrite(DEBUG_PIN_USB_START, LOW);   // Clear transfer start pin
    digitalWrite(DEBUG_PIN_USB_COMPLETE, HIGH);
            ARMTimer::blockingDelayMicros(10);  // 10us pulse for scope trigger
    digitalWrite(DEBUG_PIN_USB_COMPLETE, LOW);
    
    // *** CRITICAL: Notify queue that command completed and process next ***
    // EventResponder handles completion automatically
    
    return true;
}

// ===== MODIFIED SEND COMMAND TO USE PROPER USBHost_t36 PATTERN =====
bool USBControlPad::sendCommand(const uint8_t* data, size_t length) {
    if (!initialized || length > 64) {
        Serial.printf("❌ sendCommand failed: initialized=%d, length=%zu\n", initialized, length);
        return false;
    }
    
    if (!mydevice || !driver_) {
        Serial.printf("❌ sendCommand failed: device=%p, driver=%p\n", mydevice, driver_);
        return false;
    }
    
    // *** HARDWARE DEBUG: USB transfer starting ***
    initDebugPins();
    digitalWrite(DEBUG_PIN_USB_START, HIGH);
    
    // *** SEND COMMAND WITH TIMEOUT MONITORING ***
    uint32_t commandStart = ARMTimer::getMicros();
    
    // Record bandwidth usage
    bandwidthMonitor.recordTransfer(length);
    
    bool success = driver_->sendPacket(const_cast<uint8_t*>(data), length);
    
    uint32_t commandEnd = ARMTimer::getMicros();
    uint32_t commandDuration = commandEnd - commandStart;
    
    // Log USB commands that take too long (over 10ms indicates timeout issues)
    if (commandDuration > 10000) {
        Serial.printf("🐌 USB sendPacket took %dms\n", commandDuration / 1000);
    }
    
    // Critical timeout detection (over 1 second = major problem) 
    if (commandDuration > 1000000) {
        Serial.printf("🚨 CRITICAL USB TIMEOUT: %dms! Device may be unresponsive.\n", commandDuration / 1000);
    }
    
    if (!success) {
        digitalWrite(DEBUG_PIN_USB_START, LOW);  // Clear if failed immediately
    }
    
    // USBHost_t36 handles all timing and will call hid_process_out_data() when complete
    return success;
}

// ===== USB DRIVER IMPLEMENTATION =====

USBControlPad::USBControlPad(USBHost &host) : myusb(&host) {
    Serial.println("🔧 USBControlPad HID Input driver instance created");
    init();
}

void USBControlPad::init() {
    // Register this HID input driver with the HID parsers
    USBHIDParser::driver_ready_for_hid_collection(this);
}

hidclaim_t USBControlPad::claim_collection(USBHIDParser *driver, Device_t *dev, uint32_t topusage) {
    Serial.println("🔍 *** USBControlPad::claim_collection called ***");
    Serial.printf("   Device: VID:0x%04X PID:0x%04X, TopUsage:0x%X\n", 
                 dev->idVendor, dev->idProduct, topusage);
    
    // Check if this is our ControlPad device (VID:0x2516 PID:0x012D)
    if (dev->idVendor != CONTROLPAD_VID || dev->idProduct != CONTROLPAD_PID) {
        Serial.printf("❌ Not ControlPad device: VID:0x%04X PID:0x%04X (looking for VID:0x%04X PID:0x%04X)\n", 
                     dev->idVendor, dev->idProduct, CONTROLPAD_VID, CONTROLPAD_PID);
        return CLAIM_NO;
    }
    
    // Identify and name the different HID collections based on TopUsage
    const char* collectionName = "Unknown";
    bool shouldClaim = false;
    
    switch (topusage) {
        case 0x10006:  // Generic Desktop Keyboard
            collectionName = "Keyboard";
            shouldClaim = true;
            break;
        case 0xFF000001:  // Vendor-specific (Control/LED interface)
            collectionName = "Control/LED";
            shouldClaim = true;
            break;
        case 0x10080:  // Generic Desktop System Control
            collectionName = "Dual Action";
            shouldClaim = false;  // Skip for now
            break;
        case 0xC0001:  // Consumer Control
            collectionName = "Consumer Control";
            shouldClaim = false;  // Skip for now
            break;
        case 0x10002:  // Generic Desktop Mouse
            collectionName = "Mouse/Pointer";
            shouldClaim = false;  // Skip for now
            break;
        default:
            Serial.printf("⚠️ Unknown TopUsage: 0x%X\n", topusage);
            shouldClaim = false;
            break;
    }
    
    Serial.printf("🏷️ Collection identified: %s (TopUsage:0x%X)\n", collectionName, topusage);
    
    if (!shouldClaim) {
        Serial.printf("⏭️ Skipping %s collection (not needed for basic functionality)\n", collectionName);
        return CLAIM_NO;
    }
    
    // Only claim the Control/LED interface for now (the most important one)
    if (topusage != 0xFF000001) {
        Serial.printf("⏭️ Skipping %s collection (focusing on Control/LED only)\n", collectionName);
        return CLAIM_NO;
    }
    
    // Only claim one device at a time
    if (mydevice != NULL && dev != mydevice) {
        Serial.println("❌ Already claimed another device");
        return CLAIM_NO;
    }
    
    Serial.printf("🎯 *** CLAIMING %s COLLECTION! *** TopUsage:0x%X\n", collectionName, topusage);
    
    // Store device and driver references
    mydevice = dev;
    driver_ = driver;
    usage_ = topusage;
    device_ = dev;  // Store for legacy compatibility
    
    // Store the instance globally so our main loop can access it
    controlPadDriver = this;
    
    Serial.printf("✅ %s HID collection connected: VID:0x%04X PID:0x%04X\n", 
                 collectionName, dev->idVendor, dev->idProduct);
    return CLAIM_INTERFACE;
}

void USBControlPad::disconnect_collection(Device_t *dev) {
    if (mydevice == dev) {
        Serial.println("❌ USBControlPad HID Input disconnected");
        initialized = false;
        kbd_polling = false;
        ctrl_polling = false;
        dual_polling = false;
        hall_sensor_polling = false;
        sensor_out_polling = false;
        
        // Clear device references
        mydevice = NULL;
        driver_ = NULL;
        usage_ = 0;
        device_ = NULL;
        
        // Clear global pointer
        if (controlPadDriver == this) {
            controlPadDriver = nullptr;
        }
    }
}

void USBControlPad::hid_input_begin(uint32_t topusage, uint32_t type, int lgmin, int lgmax) {
    // Begin HID input parsing
    Serial.printf("🔍 HID Input Begin: TopUsage=0x%X, Type=%d, Min=%d, Max=%d\n", 
                 topusage, type, lgmin, lgmax);
}

void USBControlPad::hid_input_data(uint32_t usage, int32_t value) {
    // Handle individual HID input field data
    Serial.printf("🔍 HID Data: Usage=0x%X, Value=%d\n", usage, value);
    
    // For ControlPad, we might process individual button states here
    // This gets called for each HID usage item in the report
}

void USBControlPad::hid_input_end() {
    // End HID input parsing
    Serial.println("🔍 HID Input End");
}

bool USBControlPad::hid_process_in_data(const Transfer_t *transfer) {
    // Handle raw HID input data directly - NO QUEUING
    static uint32_t inputCount = 0;
    inputCount++;
    
    // *** FILTER: Only process and log actual button events ***
    // Check if this is a button event before logging or processing
    if (transfer->length >= 7) {
        const uint8_t* buffer = (const uint8_t*)transfer->buffer;
        
        // Only log and process if it's a button event (0x43 0x01)
        if (buffer[0] == 0x43 && buffer[1] == 0x01) {
            Serial.printf("🔍 HID Process In Data #%lu: Length=%d (BUTTON EVENT)\n", inputCount, transfer->length);
            uint8_t buttonId = buffer[4];  // USB button ID (0-23)
            uint8_t state = buffer[5];     // Button state
            
            // Apply BUTTON MAPPING - Testing different mappings to find correct one
            if (buttonId < 24) {
                            // Column-major conversion (vertical to horizontal)
            uint8_t col = buttonId / 5;  // Which column (0-4)
            uint8_t row = buttonId % 5;  // Which row in that column (0-4) 
            uint8_t ledIndex = row * 5 + col;  // Convert to visual button position
                
                bool validEvent = false;
                bool pressed = false;
                
                if (state == 0xC0) {
                    pressed = true;
                    validEvent = true;
                } else if (state == 0x40) {
                    pressed = false;
                    validEvent = true;
                }
                
                if (validEvent) {
                    // *** SINGLE EVENT SYSTEM: Only update Unified LED Manager ***
                    // This eliminates double event processing and duplicate LED updates
                    ledManager.setButtonState(ledIndex, pressed);
                    
                    Serial.printf("🎮 Button %d %s\n", ledIndex + 1, pressed ? "PRESSED" : "RELEASED");
                }
            }
        } else {
            // Not a button event - ignore silently (no logging to reduce spam)
            return true;
        }
    }
    
    return true; // Return true if we processed the data
}

bool USBControlPad::begin() {
    // Simplified initialization - no queue parameter needed
    if (true) {
        Serial.println("🎯 USB DRIVER BEGIN - Starting with HID Input only...");
        initialized = true;
        Serial.println("✅ USB Driver initialization complete");
        
        // Send activation sequence - device might need this before accepting LED commands
        Serial.println("🚀 Sending device activation sequence...");
        sendActivationSequence();
    }
    return true;
}

// ===== SIMPLE WORKING LED QUEUE - FIXES STALL ISSUE =====
// Just send LED data directly with minimal queueing
bool USBControlPad::updateAllLEDs(const ControlPadColor* colors, size_t count, bool priority, uint32_t retryStartTime = 0) {
    if (!colors || count == 0) {
        Serial.println("❌ updateAllLEDs: Invalid parameters");
        return false;
    }

    // Count constraints enforcement
    if (count > CONTROLPAD_NUM_BUTTONS) {
        Serial.printf("⚠️ updateAllLEDs: count %zu exceeds max %d, limiting\n", count, CONTROLPAD_NUM_BUTTONS);
        count = CONTROLPAD_NUM_BUTTONS;
    }

    // *** DISABLED: SET CUSTOM MODE APPROACH ***
    // Temporarily disabling setCustomMode() to test if constant mode switching causes flickering
    // Device should stay in custom mode once initially set during activation
    /*
    uint32_t customModeStart = ARMTimer::getMicros();
    if (!setCustomMode()) {
        Serial.println("❌ Failed to set custom mode before LED update");
        return false;
    }
    uint32_t customModeEnd = ARMTimer::getMicros();
    
    // Give device time to process custom mode command using ARM timer
    uint32_t customModeTime = ARMTimer::getMicros();
    while ((ARMTimer::getMicros() - customModeTime) < 999) {
        // 999μs precise ARM timer delay
    }
    uint32_t delayEnd = ARMTimer::getMicros();
    
    Serial.printf("🎨 CustomMode: %dμs, Delay: %dμs, Total: %dμs\n", 
                 customModeEnd - customModeStart, 
                 delayEnd - customModeTime, 
                 delayEnd - customModeStart);
    */

    // Prepare packages with CORRECT format from working implementation
    uint8_t pkg1[64] = {0};
    uint8_t pkg2[64] = {0};
    
    // *** CORRECT PACKAGE 1 HEADER ***
    pkg1[0] = 0x56; pkg1[1] = 0x83; 
    pkg1[2] = 0x00; pkg1[3] = 0x00;  // 0000
    pkg1[4] = 0x01; pkg1[5] = 0x00; pkg1[6] = 0x00; pkg1[7] = 0x00;  // 01000000
    pkg1[8] = 0x80; pkg1[9] = 0x01; pkg1[10] = 0x00; pkg1[11] = 0x00; // 80010000
    pkg1[12] = 0xFF; pkg1[13] = 0x00; pkg1[14] = 0x00; pkg1[15] = 0x00; // ff000000
    pkg1[16] = 0x00; pkg1[17] = 0x00; // 0000
    pkg1[18] = 0xFF; // brightness
    pkg1[19] = 0xFF; // brightness for all leds
    pkg1[20] = 0x00; pkg1[21] = 0x00; pkg1[22] = 0x00; pkg1[23] = 0x00; // 00000000
    
    // *** CORRECT PACKAGE 2 HEADER ***
    pkg2[0] = 0x56; pkg2[1] = 0x83; pkg2[2] = 0x01; // 568301
    
    // *** CORRECT LED ADDRESSING PATTERN ***
    // Package 1: LEDs in groups of 5 by columns
    // Group 1: LEDs 1,6,11,16,21 (using 0-based indexing: 0,5,10,15,20)
    // Group 2: LEDs 2,7,12,17,22 (using 0-based indexing: 1,6,11,16,21)  
    // Group 3: LEDs 3,8,13,18 (using 0-based indexing: 2,7,12,17) - partial
    
    int pkg1_pos = 24;  // Start after header
    
    // Group 1: LEDs 0,5,10,15,20
    for (int i = 0; i < 5; i++) {
        int led_idx = i * 5;  // 0, 5, 10, 15, 20
        if (led_idx < count) {
            pkg1[pkg1_pos++] = colors[led_idx].r;
            pkg1[pkg1_pos++] = colors[led_idx].g; 
            pkg1[pkg1_pos++] = colors[led_idx].b;
        } else {
            pkg1[pkg1_pos++] = 0; pkg1[pkg1_pos++] = 0; pkg1[pkg1_pos++] = 0;
        }
    }
    
    // Group 2: LEDs 1,6,11,16,21
    for (int i = 0; i < 5; i++) {
        int led_idx = 1 + i * 5;  // 1, 6, 11, 16, 21
        if (led_idx < count) {
            pkg1[pkg1_pos++] = colors[led_idx].r;
            pkg1[pkg1_pos++] = colors[led_idx].g;
            pkg1[pkg1_pos++] = colors[led_idx].b;
        } else {
            pkg1[pkg1_pos++] = 0; pkg1[pkg1_pos++] = 0; pkg1[pkg1_pos++] = 0;
        }
    }
    
    // Group 3: LEDs 2,7,12,17 (partial, LED 17 continues in pkg2)
    for (int i = 0; i < 3; i++) {
        int led_idx = 2 + i * 5;  // 2, 7, 12
        if (led_idx < count) {
            pkg1[pkg1_pos++] = colors[led_idx].r;
            pkg1[pkg1_pos++] = colors[led_idx].g;
            pkg1[pkg1_pos++] = colors[led_idx].b;
        } else {
            pkg1[pkg1_pos++] = 0; pkg1[pkg1_pos++] = 0; pkg1[pkg1_pos++] = 0;
        }
    }
    
    // LED 17 - only red byte in pkg1
    if (17 < count) {
        pkg1[pkg1_pos++] = colors[17].r;
    } else {
        pkg1[pkg1_pos++] = 0;
    }
    
    // *** PACKAGE 2 CONTINUES LED 17 ***
    int pkg2_pos = 3;  // Start after short header
    
    // Masked byte before LED 17's green/blue
    pkg2[pkg2_pos++] = 0x00;
    
    // LED 17 - green and blue bytes
    if (17 < count) {
        pkg2[pkg2_pos++] = colors[17].g;
        pkg2[pkg2_pos++] = colors[17].b;
    } else {
        pkg2[pkg2_pos++] = 0; pkg2[pkg2_pos++] = 0;
    }
    
    // LED 22
    if (22 < count) {
        pkg2[pkg2_pos++] = colors[22].r;
        pkg2[pkg2_pos++] = colors[22].g;
        pkg2[pkg2_pos++] = colors[22].b;
    } else {
        pkg2[pkg2_pos++] = 0; pkg2[pkg2_pos++] = 0; pkg2[pkg2_pos++] = 0;
    }
    
    // Group 4: LEDs 3,8,13,18,23
    for (int i = 0; i < 5; i++) {
        int led_idx = 3 + i * 5;  // 3, 8, 13, 18, 23
        if (led_idx < count) {
            pkg2[pkg2_pos++] = colors[led_idx].r;
            pkg2[pkg2_pos++] = colors[led_idx].g;
            pkg2[pkg2_pos++] = colors[led_idx].b;
        } else {
            pkg2[pkg2_pos++] = 0; pkg2[pkg2_pos++] = 0; pkg2[pkg2_pos++] = 0;
        }
    }
    
    // Group 5: LEDs 4,9,14,19
    for (int i = 0; i < 4; i++) {
        int led_idx = 4 + i * 5;  // 4, 9, 14, 19  
        if (led_idx < count) {
            pkg2[pkg2_pos++] = colors[led_idx].r;
            pkg2[pkg2_pos++] = colors[led_idx].g;
            pkg2[pkg2_pos++] = colors[led_idx].b;
        } else {
            pkg2[pkg2_pos++] = 0; pkg2[pkg2_pos++] = 0; pkg2[pkg2_pos++] = 0;
        }
    }
    
    bool hasColors = true;  // Always assume we have colors with correct format
    
    // Debug: Check if we have any actual color data
    if (!hasColors) {
        // Debug removed to prevent timing issues
        // Add some test colors to verify the system works
        if (count > 0) {
            pkg1[3] = 255; pkg1[4] = 0; pkg1[5] = 0;    // Button 0: Red
            pkg1[6] = 0; pkg1[7] = 255; pkg1[8] = 0;    // Button 1: Green  
            pkg1[9] = 0; pkg1[10] = 0; pkg1[11] = 255;  // Button 2: Blue
            Serial.println("🎨 Added test colors: Red, Green, Blue for buttons 0,1,2");
        }
    } else {
        // Debug removed to prevent timing issues
    }

    // *** HARDWARE-TIMED QUEUE: No blocking delays! ***
    
    if (!isDeviceConnected()) {
        Serial.println("❌ Device not connected");
        return false;
    }
    
    // *** TEST: Try sending activation + custom mode before LED commands ***
    static bool firstTime = true;
    if (firstTime) {
        Serial.println("🔄 First LED update - sending activation + custom mode...");
        sendActivationSequence();
        ARMTimer::blockingDelayMicros(100000); // 100ms - only for activation
        setCustomMode();
        ARMTimer::blockingDelayMicros(50000); // 50ms - only for activation
        firstTime = false;
        Serial.println("✅ Activation completed for LED updates");
    }
    
    // *** RATE LIMITING: Prevent flooding the queue ***
    static uint32_t lastLEDUpdateTime = 0;
    uint32_t currentTime = ARMTimer::getMicros();
    if ((currentTime - lastLEDUpdateTime) < 20000) { // Minimum 20ms between updates
        return false;
    }
    lastLEDUpdateTime = currentTime;
    
    // *** DIAGNOSTIC: Check queue status before enqueuing ***
    uint8_t queueSizeBefore = ledQueue.size();
    bool queueFullBefore = ledQueue.isFull();
    
    if (queueFullBefore) {
        // Debug removed to prevent timing issues
        return false; // Don't enqueue anything if queue is full
    }
    
    // *** BYPASS QUEUE - SEND DIRECTLY TO ELIMINATE STARVATION ***
    uint32_t updateStart = ARMTimer::getMicros();
    
    // *** SIMPLIFIED TIMING - NO COMPENSATION ***
    
retry_packets:  // Retry label for timeout loop
    // Packet 1: LED data package 1 - INDIVIDUAL PACKET RETRY LOGIC
    int pkt1Retries = 0;
    uint32_t pkt1TransmitTime = 0;
    bool success1 = false;
    
retry_p1:
    uint32_t pkt1Start = ARMTimer::getMicros();
    success1 = sendCommand(pkg1, 64);
    uint32_t pkt1End = ARMTimer::getMicros();
    pkt1TransmitTime = pkt1End - pkt1Start;
    
    // CRITICAL: Ensure P1 is actually transmitted - retry if needed
    int retryCount = 0;
    while (pkt1TransmitTime == 0 && success1 && retryCount < 3) {
        // USB controller queued it - wait and retry to ensure transmission
        ARMTimer::blockingDelayMicros(40);
        pkt1End = ARMTimer::getMicros();
        pkt1TransmitTime = pkt1End - pkt1Start;
        retryCount++;
    }
    
    if (pkt1TransmitTime == 0 || !success1) {
        Serial.printf("❌ P1 FAILED after %d retries - aborting LED update\n", retryCount);
        return false; // Abort entire update if P1 fails
    }
    
    // Check P1 timing - retry up to 4 times if slow
    if (pkt1TransmitTime > 60) {
        pkt1Retries++;
        if (pkt1Retries <= 4) {
            Serial.printf("⏰ P1 timeout (attempt %d/4) - %dμs, retrying in 1ms\n", pkt1Retries, pkt1TransmitTime);
            ARMTimer::blockingDelayMicros(990);
            goto retry_p1;
        } else {
            Serial.printf("❌ P1 failed after 4 retries - all attempts >60μs\n");
            return false;
        }
    }
    
    // Fine-tuned timing compensation (985μs to target ~3000μs total)
    ARMTimer::blockingDelayMicros(985);
    
    // Packet 2: LED data package 2 - INDIVIDUAL PACKET RETRY LOGIC
    int pkt2Retries = 0;
    uint32_t pkt2TransmitTime = 0;
    bool success2 = false;
    
retry_p2:
    uint32_t pkt2Start = ARMTimer::getMicros();
    success2 = sendCommand(pkg2, 64);
    uint32_t pkt2End = ARMTimer::getMicros();
    pkt2TransmitTime = pkt2End - pkt2Start;
    
    // CRITICAL: Ensure P2 is actually transmitted - retry if needed
    retryCount = 0;
    while (pkt2TransmitTime == 0 && success2 && retryCount < 3) {
        // USB controller queued it - wait and retry to ensure transmission  
        ARMTimer::blockingDelayMicros(40);
        pkt2End = ARMTimer::getMicros();
        pkt2TransmitTime = pkt2End - pkt2Start;
        retryCount++;
    }
    
    if (pkt2TransmitTime == 0 || !success2) {
        Serial.printf("❌ P2 FAILED after %d retries - aborting LED update\n", retryCount);
        return false; // Abort entire update if P2 fails
    }
    
    // Check P2 timing - retry up to 4 times if slow
    if (pkt2TransmitTime > 60) {
        pkt2Retries++;
        if (pkt2Retries <= 4) {
            Serial.printf("⏰ P2 timeout (attempt %d/4) - %dμs, retrying in 1ms\n", pkt2Retries, pkt2TransmitTime);
            ARMTimer::blockingDelayMicros(998);
            goto retry_p2;
        } else {
            Serial.printf("❌ P2 failed after 4 retries - all attempts >60μs\n");
            return false;
        }
    }
    
    // Fine-tuned timing compensation (985μs to target ~3000μs total)
    ARMTimer::blockingDelayMicros(985);
    
    // *** CORRECT APPLY COMMANDS FROM WORKING IMPLEMENTATION ***
    
    // Command 4: Apply command (4180 0000) - INDIVIDUAL PACKET RETRY LOGIC
    uint8_t applyCmd[64] = {0};
    applyCmd[0] = 0x41; applyCmd[1] = 0x80;
    applyCmd[2] = 0x00; applyCmd[3] = 0x00;
    
    int pkt3Retries = 0;
    uint32_t pkt3TransmitTime = 0;
    bool success3 = false;
    
retry_p3:
    uint32_t pkt3Start = ARMTimer::getMicros();
    success3 = sendCommand(applyCmd, 64);
    uint32_t pkt3End = ARMTimer::getMicros();
    pkt3TransmitTime = pkt3End - pkt3Start;
    
    // CRITICAL: Ensure P3 is actually transmitted - retry if needed
    retryCount = 0;
    while (pkt3TransmitTime == 0 && success3 && retryCount < 3) {
        // USB controller queued it - wait and retry to ensure transmission
        ARMTimer::blockingDelayMicros(40);
        pkt3End = ARMTimer::getMicros();
        pkt3TransmitTime = pkt3End - pkt3Start;
        retryCount++;
    }
    
    if (pkt3TransmitTime == 0 || !success3) {
        Serial.printf("❌ P3 FAILED after %d retries - aborting LED update\n", retryCount);
        return false; // Abort entire update if P3 fails
    }
    
    // Check P3 timing - retry up to 4 times if slow
    if (pkt3TransmitTime > 60) {
        pkt3Retries++;
        if (pkt3Retries <= 4) {
            Serial.printf("⏰ P3 timeout (attempt %d/4) - %dμs, retrying in 1ms\n", pkt3Retries, pkt3TransmitTime);
            ARMTimer::blockingDelayMicros(998);
            goto retry_p3;
        } else {
            Serial.printf("❌ P3 failed after 4 retries - all attempts >60μs\n");
            return false;
        }
    }
    
    // Fine-tuned timing compensation (985μs to target ~3000μs total)
    ARMTimer::blockingDelayMicros(985);
    
    // Command 5: Finalize command (5128 0000 ff00) - INDIVIDUAL PACKET RETRY LOGIC
    uint8_t finalizeCmd[64] = {0};
    finalizeCmd[0] = 0x51; finalizeCmd[1] = 0x28;
    finalizeCmd[2] = 0x00; finalizeCmd[3] = 0x00;
    finalizeCmd[4] = 0xFF; finalizeCmd[5] = 0x00;
    
    int pkt4Retries = 0;
    uint32_t pkt4TransmitTime = 0;
    bool success4 = false;
    
retry_p4:
    uint32_t pkt4Start = ARMTimer::getMicros();
    success4 = sendCommand(finalizeCmd, 64);
    uint32_t pkt4End = ARMTimer::getMicros();
    pkt4TransmitTime = pkt4End - pkt4Start;
    
    // CRITICAL: Ensure P4 is actually transmitted - retry if needed
    retryCount = 0;
    while (pkt4TransmitTime == 0 && success4 && retryCount < 3) {
        // USB controller queued it - wait and retry to ensure transmission
        ARMTimer::blockingDelayMicros(40);
        pkt4End = ARMTimer::getMicros();
        pkt4TransmitTime = pkt4End - pkt4Start;
        retryCount++;
    }
    
    if (pkt4TransmitTime == 0 || !success4) {
        Serial.printf("❌ P4 FAILED after %d retries - aborting LED update\n", retryCount);
        return false; // Abort entire update if P4 fails
    }
    
    // Check P4 timing - retry up to 4 times if slow
    if (pkt4TransmitTime > 60) {
        pkt4Retries++;
        if (pkt4Retries <= 4) {
            Serial.printf("⏰ P4 timeout (attempt %d/4) - %dμs, retrying in 1ms\n", pkt4Retries, pkt4TransmitTime);
            ARMTimer::blockingDelayMicros(998);
            goto retry_p4;
        } else {
            Serial.printf("❌ P4 failed after 4 retries - all attempts >60μs\n");
            return false;
        }
    }
    
    // All packets succeeded with acceptable timing!
    
    uint32_t updateEnd = ARMTimer::getMicros();
    uint32_t updateDuration = updateEnd - updateStart;
    
    // Simple logging without timing analysis
    Serial.printf("📦 Packet timings - P1: %dμs, P2: %dμs, P3: %dμs, P4: %dμs, Total: %dμs\n",
                 pkt1TransmitTime, pkt2TransmitTime, pkt3TransmitTime, pkt4TransmitTime, updateDuration);
    
    // Only log slow LED updates (over 10ms)
    if (updateDuration > 10000) {
        Serial.printf("⏱️ LED update took: %dms\n", updateDuration / 1000);
    }
    
    // *** DIAGNOSTIC: Show queue status after enqueuing ***
    uint8_t queueSizeAfter = ledQueue.size();
    // Debug removed to prevent timing issues
    

    
    if (success1 && success2 && success3 && success4) {
        // Debug removed to prevent timing issues
        
        // Debug removed to prevent timing issues
        
        return true;
    } else {
        // Debug removed to prevent timing issues
        return false;
    }
}

// *** REMOVED ALL COMPLEX LED METHODS ***
// No more async processing, retries, or blocking delays
// USBHost_t36 handles everything for us

// Essential LED command functions using HID output reports
bool USBControlPad::setCustomMode() {
    // Serial.printf("🎨 setCustomMode: Setting device to custom LED mode (CORRECT FORMAT)...\n");
    
    // COMMAND 1: Set effect mode (from user documentation)
    // 56 81 0000 01000000 02000000 bbbbbbbb (custom mode) + trailing zeros
    uint8_t cmd[64] = {0};
    cmd[0] = 0x56;  // Vendor ID
    cmd[1] = 0x81;  // Set effect command
    cmd[2] = 0x00;  // 0000
    cmd[3] = 0x00;
    cmd[4] = 0x01;  // 01000000 = activate
    cmd[5] = 0x00;
    cmd[6] = 0x00;
    cmd[7] = 0x00;
    cmd[8] = 0x02;  // 02000000
    cmd[9] = 0x00;
    cmd[10] = 0x00;
    cmd[11] = 0x00;
    cmd[12] = 0xBB; // bbbbbbbb = custom mode
    cmd[13] = 0xBB;
    cmd[14] = 0xBB;
    cmd[15] = 0xBB;
    // Rest remains zeros
    
    // Serial.printf("🎨 Custom mode command: 0x%02X 0x%02X (CORRECT custom mode setup)\n", 
    //              cmd[0], cmd[1]);
    
    bool result = sendCommand(cmd, 64);
    // Serial.printf("🎨 setCustomMode result: %s\n", result ? "SUCCESS" : "FAILED");
    return result;
}

// *** REMOVED ALL OTHER LED METHODS ***
// Keeping only the essential activation and the simplified updateAllLEDs

bool USBControlPad::sendActivationSequence() {
    Serial.println("🔧 Starting activation sequence...");
    // Based on the original working activation sequence
    bool success = true;
    
    // Step 1: 0x42 00 activation
    uint8_t cmd1[64] = {0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    bool step1 = sendCommand(cmd1, 64);
    success &= step1;
    if (step1) ARMTimer::blockingDelayMicros(999); // Allow device to process (999μs)
    
    // Step 2: 0x42 10 variant  
    uint8_t cmd2[64] = {0x42, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    bool step2 = sendCommand(cmd2, 64);
    success &= step2;
    if (step2) ARMTimer::blockingDelayMicros(999); // Allow device to process (999μs)
    
    // Step 3: 0x43 00 button activation
    uint8_t cmd3[64] = {0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    bool step3 = sendCommand(cmd3, 64);
    success &= step3;
    if (step3) ARMTimer::blockingDelayMicros(999); // Allow device to process (999μs)
    
    // Step 4: 0x41 80 status
    uint8_t cmd4[64] = {0x41, 0x80, 0x00, 0x00};
    bool step4 = sendCommand(cmd4, 64);
    success &= step4;
    if (step4) ARMTimer::blockingDelayMicros(999); // Allow device to process (999μs)
    
    // Step 5: 0x52 00 activate effect modes
    uint8_t cmd5[64] = {0x52, 0x00, 0x00, 0x00};
    bool step5 = sendCommand(cmd5, 64);
    success &= step5;
    if (step5) ARMTimer::blockingDelayMicros(999); // Allow device to process (999μs)

        uint8_t cmd6[64] = {0x41, 0x80, 0x00, 0x00};
    bool step6 = sendCommand(cmd6, 64);
    success &= step6;
    if (step6) ARMTimer::blockingDelayMicros(999); // Allow device to process (999μs)
    
    if (!success) {
        Serial.println("⚠️ Activation sequence failed");
    }
    return success;
}

// ===== PUBLIC LED QUEUE ACCESS FOR MONITORING =====
void getLEDQueueStatus(size_t* queueSize, bool* isProcessing) {
    if (queueSize) *queueSize = ledQueue.size();
    if (isProcessing) *isProcessing = false; // Simplified queue doesn't track processing state
}

// ===== HARDWARE MANAGER IMPLEMENTATION =====

ControlPadHardware::ControlPadHardware() {
    // Constructor - initialize any hardware-specific settings
    globalHardwareInstance = this;  // Set global instance for USB callbacks
}

ControlPadHardware::~ControlPadHardware() {
    // DMA queues clean up automatically
    // usbDriver is now a member object, not a pointer - no need to delete
}

bool ControlPadHardware::begin(ControlPad& pad) {
    // Store reference to the ControlPad instance
    currentPad = &pad;
    
    // 1. Initialize simplified queues
    Serial.println("✅ LED queue initialized");
    
    // 2. Use global USB Host and Driver (USBHost_t36 standard pattern)
    // USB Host is already started in main.cpp
    
    // 3. Allow USB host to discover and enumerate devices
    for (int i = 0; i < 100; i++) {  // Allow up to 10 seconds for enumeration
        globalUSBHost.Task();  // This processes USB enumeration and calls claim()
        
        ARMTimer::blockingDelayMicros(100000); // 100ms
        if (globalControlPadDriver.device_) {
            break;
        }
    }
    
    // 4. Wait for device enumeration using ARM timer
    uint32_t startTime = ARMTimer::getMicros();
    while (!globalControlPadDriver.device_ && (ARMTimer::getMicros() - startTime) < 10000000) {
        ARMTimer::blockingDelayMicros(100000); // 100ms
    }
    
    if (!globalControlPadDriver.device_) {
        return false;
    }
    
    // 5. Wait for USB configuration to stabilize
    ARMTimer::blockingDelayMicros(2000000); // 2 seconds
    
    // 6. Initialize the driver
    bool initResult = globalControlPadDriver.begin();
    if (!initResult) {
        return false;
    }
    
    // 7. Send activation sequence and set custom mode
    Serial.println("🚀 Sending device activation sequence...");
    bool activationResult = globalControlPadDriver.sendActivationSequence();
    if (activationResult) {
        Serial.println("✅ Activation sequence completed successfully");
        ARMTimer::blockingDelayMicros(100000); // 100ms for activation to settle
        
        Serial.println("🎨 Setting device to custom LED mode...");
        bool customModeResult = globalControlPadDriver.setCustomMode();
        if (customModeResult) {
            Serial.println("✅ Custom mode activated successfully");
        } else {
            Serial.println("⚠️ Custom mode activation failed");
        }
        ARMTimer::blockingDelayMicros(50000); // Allow mode change to complete (50ms)
    } else {
        Serial.println("❌ Activation sequence failed");
    }
    
    return true;
}

void ControlPadHardware::poll() {
    // *** POLLING DISABLED - Now using direct USB callbacks ***
    // All button processing happens in USBControlPad::hid_process_in_data()
    // This provides optimal timing for MIDI applications
    
    // This method is kept for compatibility but does nothing
        return;
}

bool ControlPadHardware::setAllLeds(const ControlPadColor* colors, size_t count) {
    // *** NON-BLOCKING LED UPDATE FOR MIDI TIMING ***
    // Simple, fast LED update using USBHost_t36's built-in capabilities
    return globalControlPadDriver.updateAllLEDs(colors, count, false);
}

// *** DUPLICATE CLASS DEFINITION REMOVED - NOW DEFINED EARLIER IN FILE ***

// *** PUBLIC ANIMATION CONTROL ***
void ControlPadHardware::enableAnimation() {
    ledManager.setAnimationEnabled(true);
}

void ControlPadHardware::disableAnimation() {
    ledManager.setAnimationEnabled(false);
}

void ControlPadHardware::updateAnimation() {
    // This method is now empty as the animation logic is handled by ledManager
}

void ControlPadHardware::updateButtonHighlights() {
    // This method is now empty as the animation logic is handled by ledManager
}

void ControlPadHardware::updateUnifiedLEDs() {
    // *** NEW 1MS-TIMED QUEUE PROCESSING ***
    // Debug removed to prevent timing issues
    
    ledTimingController.processTimedSending(); // Process queued LED commands with 1ms timing
    
    // *** CHECK BOTH LED SYSTEMS ***
    
    // 1. Check the UnifiedLEDManager (if animation enabled)
    if (ledManager.isAnimationEnabled() && ledManager.shouldSendUpdate()) {
        const ControlPadColor* ledState = ledManager.getLEDState();
        bool success = globalControlPadDriver.updateAllLEDs(ledState, 24, false);
        if (!success) {
            // LED update failure logging disabled to prevent timing issues
        }
    }
    
    // 2. Check the old ControlPad system (if there are changes from setAllButtonColors)
    if (currentPad && currentPad->hasLedChanges()) {
        Serial.println("🎨 Detected LED changes from setAllButtonColors - sending to queue...");
        // Use the old system but it will go through the queue now
        currentPad->updateSmartLeds(); // This should use the queue system
    }
}

bool ControlPadHardware::isAnimationEnabled() const {
    return ledManager.isAnimationEnabled();
}

// ===== LED TIMING CONTROLLER IMPLEMENTATION =====
void LEDTimingController::processTimedSending() {
    // Minimal debug: Track queue empty events
    static uint32_t lastEmptyWarning = 0;
    static bool wasEmpty = true; // Track when queue transitions from full to empty
    
    if (!enabled) {
        // Show once that timing controller is disabled
        static bool disabledWarningShown = false;
        if (!disabledWarningShown) {
            Serial.println("⚠️ LEDTimingController is DISABLED");
            disabledWarningShown = true;
        }
        return;
    }
    
    uint32_t currentTime = ARMTimer::getMicros();
    
    // Only send if enough time has passed AND we have packets
    if ((currentTime - lastSendTime >= sendIntervalMicros)) {
        uint8_t packet[64];
        if (queue.dequeue(packet)) {
            // Detect queue starvation recovery
            if (wasEmpty) {
                Serial.printf("✅ Queue recovered (size: %d)\n", queue.size());
                wasEmpty = false;
            }
            
            // Send the packet
            if (globalControlPadDriver.isDeviceConnected()) {
                                    bool success = globalControlPadDriver.sendCommand(packet, 64);
                    if (success) {
                        lastSendTime = currentTime;
                        // Debug removed to prevent timing issues
                    }
            } else {
                Serial.println("❌ Timing controller: Device not connected");
            }
        } else {
            // Queue is empty - detect starvation
            if (!wasEmpty) {
                Serial.printf("🚨 QUEUE STARVED! Animation timing issue?\n");
                wasEmpty = true;
            }
            
            // Show long-term empty status occasionally
                          static uint32_t lastEmptyLocalWarning = 0;
              if ((currentTime - lastEmptyLocalWarning) > 5000000) { // Every 5 seconds
                  Serial.printf("⚠️ Queue empty for >5sec\n");
                  lastEmptyLocalWarning = currentTime;
              }
        }
    }
}