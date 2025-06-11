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
        
        // DISABLED: All periodic timers disabled to prevent USB frame timing interference  
        /*
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
        */
        
        // Queue position tracking removed for simplified queue
        
        // DISABLED: 10-second timer was causing 1ms timing drops during LED transmission
        // The timer operation interfered with USB frame synchronization causing flickering
        /*
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
        */
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
        uint32_t currentTime = millis();  // Back to millis() for now
        
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
        lastUpdateTime = millis();  // Back to millis() for now
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
        last_failure_time = millis();  // Back to millis() for now
        
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

// *** USB FRAME-BASED TIMING IMPLEMENTATION ***
// All timing in LED control now uses USB frame numbers instead of millis()
// This provides frame-synchronized timing that's coherent with USB bus timing
// 1 USB frame ≈ 1ms, counting in 8ths (microframes), providing more stable timing
// than system millis() which can drift relative to USB bus

// *** USB FRAME-BASED TIMING HELPERS ***
// USB frame-synchronized timing functions (must be declared before use)
uint32_t getUSBFrameNumber() {
    // EHCI FRINDEX register format: [13:3] = frame number, [2:0] = microframe
    // Use bit shifting instead of division to avoid accumulation errors
    uint32_t frindex = USBHS_FRINDEX;
    return (frindex >> 3) & 0x7FF;  // Extract bits [13:3], mask to 11 bits (0-2047)
}

// Get current time in USB frames instead of millis() - provides USB-synchronized timing
uint32_t getUSBFrameTime() {
    return getUSBFrameNumber();
}

// Convert between frame time and milliseconds (1:1 mapping for USB Full Speed)
#define FRAMES_TO_MS(frames) (frames)
#define MS_TO_FRAMES(ms) (ms)

// Simple cleanup detection for logging only
static uint32_t lastCleanupFrame = 0;
static uint32_t cleanupFrameCount = 0;

// Record significant cleanup events for analysis (no prediction)
void recordCleanupFrame(uint32_t frameNumber, uint32_t timingDrop) {
    if (timingDrop > 2350) {  // Only count major timing drops
        cleanupFrameCount++;
        lastCleanupFrame = frameNumber;
        Serial.printf("🔮 Cleanup event #%d at frame 0x%03X: %dμs drop\n", 
                     cleanupFrameCount, frameNumber, timingDrop);
    }
}

// Frame timing correction with cleanup interference compensation
bool sendPacketWithRetry(const uint8_t* packet, size_t length, const char* packetName, uint32_t* transmitTime) {
    const uint32_t TIMING_RETRY_THRESHOLD = 950; // Retry if packet timing > 950μs
    const int MAX_RETRIES = 2;
    
    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        uint32_t frameStart = getUSBFrameNumber();
        uint32_t startTime = micros();
        
        bool success = globalControlPadDriver.sendCommand(packet, length);
        
        if (success) {
            // Force USB transmission with busy-wait
            uint32_t busyWaitStart = micros();
            while ((micros() - busyWaitStart) < 150) {
                globalUSBHost.Task();
            }
            
            uint32_t totalTime = micros() - startTime;
            uint32_t frameEnd = getUSBFrameNumber();
            *transmitTime = totalTime;
            
            // Check if timing is acceptable or if this is the final attempt
            if (totalTime <= TIMING_RETRY_THRESHOLD || attempt == MAX_RETRIES) {
                if (attempt > 0) {
                    Serial.printf("✅ %s retry succeeded on attempt %d (%dμs)\n", packetName, attempt + 1, totalTime);
                }
                return true;
            } else {
                // Timing too high, retry if we have attempts left
                Serial.printf("⚠️ %s timing high (%dμs), retrying... (attempt %d/%d)\n", 
                             packetName, totalTime, attempt + 1, MAX_RETRIES + 1);
                
                // Brief delay before retry
                delayMicroseconds(200);
            }
        } else {
            Serial.printf("❌ %s packet failed on attempt %d\n", packetName, attempt + 1);
            if (attempt < MAX_RETRIES) {
                delayMicroseconds(200); // Brief delay before retry
            }
        }
    }
    
    return false;
}

// Enhanced USB frame synchronization with cleanup timing correction and short frame detection
bool usbFrameSynchronizedDelayMs(uint32_t milliseconds, bool* hasShortFrames = nullptr, uint32_t shortFrameThreshold = 200) {
    const uint32_t NORMAL_FRAME_TIME = 1000; // 1ms per frame
    const uint32_t FRAME_TOLERANCE = 150;    // Allow ±150μs variance
    
    bool detectedShortFrames = false;
    
    for (uint32_t i = 0; i < milliseconds; i++) {
        uint32_t startFrame = getUSBFrameNumber();
        uint32_t startMicroframe = USB1_FRINDEX & 0x7;
        uint32_t startTime = micros();
        
        // Wait for next USB frame
        uint32_t currentFrame;
        do {
            globalUSBHost.Task();
            currentFrame = getUSBFrameNumber();
        } while (currentFrame == startFrame);
        
        uint32_t actualDelay = micros() - startTime;
        uint32_t endMicroframe = USB1_FRINDEX & 0x7;
        uint32_t frameDiff = (currentFrame >= startFrame) ? 
                            (currentFrame - startFrame) : 
                            (0x800 + currentFrame - startFrame);
        
        // Detect problematic short frames
        if (actualDelay < shortFrameThreshold) {
            detectedShortFrames = true;
        }
        
        // ** SIMPLIFIED LOGGING - NO AGGRESSIVE COMPENSATION **
        // Use dynamic shortFrameThreshold instead of fixed FRAME_TOLERANCE for SHORT detection
        if (actualDelay > (NORMAL_FRAME_TIME + FRAME_TOLERANCE)) {
            // Only log unusual delays, don't try to "fix" them
            Serial.printf("🕐 Frame %d: 0x%03X→0x%03X, %dμs ⚠️LONG (Δ:%d, μf:%d)\\n", 
                         i + 1, startFrame, currentFrame, actualDelay, frameDiff, endMicroframe);
        } else if (actualDelay < shortFrameThreshold) {
            // Log short frames using dynamic threshold (patience mode aware)
            Serial.printf("🕐 Frame %d: 0x%03X→0x%03X, %dμs ⚠️SHORT (Δ:%d, μf:%d)\\n", 
                         i + 1, startFrame, currentFrame, actualDelay, frameDiff, endMicroframe);
        } else {
            // Normal timing (above threshold)
            Serial.printf("🕐 Frame %d: 0x%03X→0x%03X, %dμs (Δ:%d, μf:%d)\\n", 
                         i + 1, startFrame, currentFrame, actualDelay, frameDiff, endMicroframe);
        }
        
        // ** REMOVED ALL COMPENSATION DELAYS **
        // Let the natural USB frame timing work without artificial corrections
    }
    
    // Return short frame detection status
    if (hasShortFrames) {
        *hasShortFrames = detectedShortFrames;
    }
    
    return false; // No cleanup detected (for backward compatibility)
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
    // *** LED UPDATE WITH PER-PACKET RETRY FOR CLEANUP INTERFERENCE ***
    // Retry logic is now handled per-packet inside updateAllLEDs
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

// Debug function to monitor cleanup events
void printCleanupStatus() {
    Serial.printf("🔮 Cleanup Status:\n");
    Serial.printf("   - Last cleanup frame: 0x%03X\n", lastCleanupFrame);
    Serial.printf("   - Total cleanup events: %d\n", cleanupFrameCount);
    Serial.printf("   - Retry mechanism: Active (>950μs triggers retry)\n");
    }

void resetCleanupData() {
    lastCleanupFrame = 0;
    cleanupFrameCount = 0;
    Serial.println("🔮 Cleanup data RESET");
}

// Just send LED data directly with retry mechanism for timing issues
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
    
    // Group 3: LEDs 2,7,12,17 (partial)
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
    
    if (!isDeviceConnected()) {
        Serial.println("❌ Device not connected");
        return false;
    }
    
    // *** ACTIVATION REQUIRED: Device needs activation before LED operations ***
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
    
    // *** RATE LIMITING: Prevent flooding ***
    static uint32_t lastLEDUpdateTime = 0;
    uint32_t currentTime = ARMTimer::getMicros();
    if ((currentTime - lastLEDUpdateTime) < 20000) { // Minimum 20ms between updates
        return false;
    }
    lastLEDUpdateTime = currentTime;
    
    uint32_t updateStart = ARMTimer::getMicros();
    uint32_t startFrame = getUSBFrameNumber();
    
    // *** CLEAN LED UPDATE - NO MONITORING OVERHEAD ***
    static uint32_t lastUpdateDuration = 0;
    
    // *** ENHANCED SEQUENCE TRACKING FOR FRAME TIMING ANALYSIS ***
    Serial.printf("🎬 LED sequence starting at frame 0x%03X\n", startFrame);
    
    // ** FRAME TIMING MONITORING **
    static uint32_t lastSequenceEndFrame = 0;
    if (lastSequenceEndFrame > 0) {
        uint32_t framesSinceLastUpdate = (startFrame >= lastSequenceEndFrame) ? 
                                        (startFrame - lastSequenceEndFrame) : 
                                        (0x1000 + startFrame - lastSequenceEndFrame);
        if (framesSinceLastUpdate > 600 || framesSinceLastUpdate < 400) { // Expected ~500 frames
            Serial.printf("⏱️ Unusual frame gap: %d frames since last LED update\n", framesSinceLastUpdate);
        }
    }
    
    // ** PACKET INTEGRITY TRACKING **
    uint32_t pkg1Checksum = 0, pkg2Checksum = 0;
    for (int i = 0; i < 64; i++) {
        pkg1Checksum += pkg1[i];
        pkg2Checksum += pkg2[i];
    }
    Serial.printf("🧮 Packet checksums - P1: 0x%04X, P2: 0x%04X\n", pkg1Checksum & 0xFFFF, pkg2Checksum & 0xFFFF);
    
    // *** SEND PACKETS WITH RETRY ON FRAME TIMING ISSUES ***
    uint32_t pkt1TransmitTime = 0, pkt2TransmitTime = 0, pkt3TransmitTime = 0, pkt4TransmitTime = 0;
    
    // Variables for tracking frame timing across attempts
    uint32_t p1StartFrame = 0, p1EndFrame = 0, p2StartFrame = 0, p2EndFrame = 0;
    uint32_t p3StartFrame = 0, p3EndFrame = 0, p4StartFrame = 0, p4EndFrame = 0;
    bool cleanupDuringP1 = false, cleanupDuringP2 = false, cleanupDuringP3 = false;
    
    const int MAX_SEQUENCE_RETRIES = 2;
    bool sequenceSuccess = false;
    bool hadRetries = false; // Track if any retries occurred
    
    // Simple fixed threshold - no patience mode complexity
            uint32_t shortFrameThreshold = 25; // Temporarily increased from 18μs to debug flickering
    
    for (int sequenceAttempt = 0; sequenceAttempt <= MAX_SEQUENCE_RETRIES && !sequenceSuccess; sequenceAttempt++) {
        if (sequenceAttempt > 0) {
            hadRetries = true; // Mark that retries occurred
            Serial.printf("🔄 Retrying LED sequence (attempt %d/%d) due to frame timing issues\n", 
                         sequenceAttempt + 1, MAX_SEQUENCE_RETRIES + 1);
        }
        
        // Reset cleanup detection for this attempt
        cleanupDuringP1 = cleanupDuringP2 = cleanupDuringP3 = false;
        bool frameTimingOK = true;
    
        // Packet 1: LED data package 1
        p1StartFrame = getUSBFrameNumber();
        if (!sendPacketWithRetry(pkg1, 64, "P1", &pkt1TransmitTime)) {
            continue; // Retry entire sequence
        }
        p1EndFrame = getUSBFrameNumber();
        
        bool p1HasShortFrames = false;
        cleanupDuringP1 = usbFrameSynchronizedDelayMs(1, &p1HasShortFrames, shortFrameThreshold);
        uint32_t afterP1Frame = getUSBFrameNumber();
        
        // Check for problematic short frames after P1
        if (p1HasShortFrames) {
            Serial.printf("⚠️ P1 short frame detected (<%dμs), retrying sequence\n", shortFrameThreshold);
            frameTimingOK = false;
            continue; // Immediately retry - don't waste time sending remaining packets
        }
        
        if (cleanupDuringP1) {
            Serial.printf("🔄 Cleanup detected after P1 (0x%03X→0x%03X→0x%03X) - continuing to P2...\n", 
                         p1StartFrame, p1EndFrame, afterP1Frame);
        }
        
        // Packet 2: LED data package 2  
        p2StartFrame = getUSBFrameNumber();
        if (!sendPacketWithRetry(pkg2, 64, "P2", &pkt2TransmitTime)) {
            continue; // Retry entire sequence
        }
        p2EndFrame = getUSBFrameNumber();
        
        bool p2HasShortFrames = false;
        cleanupDuringP2 = usbFrameSynchronizedDelayMs(1, &p2HasShortFrames, shortFrameThreshold);
        uint32_t afterP2Frame = getUSBFrameNumber();
        
        // Check for problematic short frames after P2
        if (p2HasShortFrames) {
            Serial.printf("⚠️ P2 short frame detected (<%dμs), retrying sequence\n", shortFrameThreshold);
            frameTimingOK = false;
            continue; // Immediately retry - don't waste time sending remaining packets
        }
        
        if (cleanupDuringP2) {
            Serial.printf("🔄 Cleanup detected after P2 (0x%03X→0x%03X→0x%03X) - continuing to P3...\n", 
                         p2StartFrame, p2EndFrame, afterP2Frame);
        }
        
        // Command 3: Apply command
    uint8_t applyCmd[64] = {0};
    applyCmd[0] = 0x41; applyCmd[1] = 0x80;
    applyCmd[2] = 0x00; applyCmd[3] = 0x00;
    
        p3StartFrame = getUSBFrameNumber();
        if (!sendPacketWithRetry(applyCmd, 64, "P3", &pkt3TransmitTime)) {
            continue; // Retry entire sequence
        }
        p3EndFrame = getUSBFrameNumber();
        
        bool p3HasShortFrames = false;
        cleanupDuringP3 = usbFrameSynchronizedDelayMs(1, &p3HasShortFrames, shortFrameThreshold);
        uint32_t afterP3Frame = getUSBFrameNumber();
        
        // Check for problematic short frames after P3
        if (p3HasShortFrames) {
            Serial.printf("⚠️ P3 short frame detected (<%dμs), retrying sequence\n", shortFrameThreshold);
            frameTimingOK = false;
            continue; // Immediately retry - don't waste time sending remaining packets
        }
        
        if (cleanupDuringP3) {
            Serial.printf("🔄 Cleanup detected after P3 (0x%03X→0x%03X→0x%03X) - continuing to P4...\n", 
                         p3StartFrame, p3EndFrame, afterP3Frame);
        }
        
        // Command 4: Finalize command (no delay after final packet)
    uint8_t finalizeCmd[64] = {0};
    finalizeCmd[0] = 0x51; finalizeCmd[1] = 0x28;
    finalizeCmd[2] = 0x00; finalizeCmd[3] = 0x00;
    finalizeCmd[4] = 0xFF; finalizeCmd[5] = 0x00;
    
        p4StartFrame = getUSBFrameNumber();
        if (!sendPacketWithRetry(finalizeCmd, 64, "P4", &pkt4TransmitTime)) {
            continue; // Retry entire sequence
        }
        p4EndFrame = getUSBFrameNumber();
    
        // If we made it here and frame timing was OK, sequence succeeded
        if (frameTimingOK) {
            sequenceSuccess = true;
            if (sequenceAttempt > 0) {
                Serial.printf("✅ LED sequence retry succeeded on attempt %d\n", sequenceAttempt + 1);
            }
            break; // Exit retry loop
        } else if (sequenceAttempt < MAX_SEQUENCE_RETRIES) {
            Serial.printf("⚠️ Frame timing issues detected, retrying entire sequence...\n");
            delayMicroseconds(500); // Brief delay before retry
        }
    }
    
    if (!sequenceSuccess) {
        Serial.printf("❌ LED sequence failed after %d attempts\n", MAX_SEQUENCE_RETRIES + 1);
        return false;
    }
    
    // Calculate total time and log results
    uint32_t updateEnd = ARMTimer::getMicros();
    uint32_t updateDuration = updateEnd - updateStart;
    uint32_t endFrame = getUSBFrameNumber();
    
    Serial.printf("📦 Packet timings - P1: %dμs, P2: %dμs, P3: %dμs, P4: %dμs, Total: %dμs (0x%03X→0x%03X)\n",
                 pkt1TransmitTime, pkt2TransmitTime, pkt3TransmitTime, pkt4TransmitTime, 
                 updateDuration, startFrame, endFrame);
    
    // ** DETAILED FRAME SEQUENCE FOR FRAME TIMING ANALYSIS **
    Serial.printf("📊 Frame sequence: Start:0x%03X P1:(0x%03X→0x%03X) P2:(0x%03X→0x%03X) P3:(0x%03X→0x%03X) P4:(0x%03X→0x%03X) End:0x%03X\n",
                 startFrame, p1StartFrame, p1EndFrame, p2StartFrame, p2EndFrame, 
                 p3StartFrame, p3EndFrame, p4StartFrame, p4EndFrame, endFrame);
    
    // Report cleanup detection summary
    bool anyCleanupDetected = cleanupDuringP1 || cleanupDuringP2 || cleanupDuringP3;
    if (anyCleanupDetected) {
        Serial.printf("📋 Cleanup summary: P1:%s P2:%s P3:%s (all packets completed)\n",
                     cleanupDuringP1 ? "⚡" : "✓",
                     cleanupDuringP2 ? "⚡" : "✓", 
                     cleanupDuringP3 ? "⚡" : "✓");
    }
    
    // *** SIMPLE CLEANUP REALIGNMENT ***
    // Work WITH the natural USB cleanup cycle instead of against it
    static uint32_t cleanupEventCount = 0;
    
    // Detect cleanup timing and wait 3 frames for natural realignment
    // Only trigger on major cleanup events (>3290μs), avoiding startup variance and post-cleanup elevated timing
    // Skip cleanup detection if retries occurred (extended timing is due to retries, not USB cleanup)
    if (updateDuration > 3291 && !hadRetries) { // High threshold to avoid false positives: startup(3229μs) and post-cleanup(2300-2400μs)
        cleanupEventCount++;
        Serial.printf("🔄 Cleanup event #%d detected: %dμs - waiting 10 frames for USB realignment\n", cleanupEventCount, updateDuration);
        
        // Wait 10 USB frames (10ms) for complete realignment after major cleanup
        for (int i = 0; i < 10; i++) {
            uint32_t currentFrame = getUSBFrameNumber();
            while (getUSBFrameNumber() == currentFrame) {
                // Wait for next frame
            }
        }
        Serial.printf("✅ USB realignment complete (10 frames) - resuming LED updates\n");
    } else if (hadRetries) {
        Serial.printf("🔄 Extended timing (%dμs) due to retries - skipping cleanup detection\n", updateDuration);
    }
    
    // ** UPDATE FRAME TRACKING FOR NEXT SEQUENCE **
    lastSequenceEndFrame = endFrame;
        
    // ** STORE TIMING FOR NEXT CLEANUP DETECTION CYCLE **
    lastUpdateDuration = updateDuration;
        
        return true;
}

// Essential LED command functions
bool USBControlPad::setCustomMode() {
    uint8_t cmd[64] = {0};
    cmd[0] = 0x56;  // Vendor ID
    cmd[1] = 0x81;  // Set effect command
    cmd[2] = 0x00;  // 0000
    cmd[3] = 0x00;
    cmd[4] = 0x01;  // 01000000
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
    
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendActivationSequence() {
    Serial.println("🔧 Starting activation sequence...");
    bool success = true;
    
    // Step 1: 0x42 00 activation
    uint8_t cmd1[64] = {0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    bool step1 = sendCommand(cmd1, 64);
    success &= step1;
    if (step1) ARMTimer::blockingDelayMicros(999);
    
    // Step 2: 0x42 10 variant  
    uint8_t cmd2[64] = {0x42, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    bool step2 = sendCommand(cmd2, 64);
    success &= step2;
    if (step2) ARMTimer::blockingDelayMicros(999);
    
    // Step 3: 0x43 00 button activation
    uint8_t cmd3[64] = {0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    bool step3 = sendCommand(cmd3, 64);
    success &= step3;
    if (step3) ARMTimer::blockingDelayMicros(999);
    
    // Step 4: 0x41 80 status
    uint8_t cmd4[64] = {0x41, 0x80, 0x00, 0x00};
    bool step4 = sendCommand(cmd4, 64);
    success &= step4;
    if (step4) ARMTimer::blockingDelayMicros(999);
    
    // Step 5: 0x52 00 activate effect modes
    uint8_t cmd5[64] = {0x52, 0x00, 0x00, 0x00};
    bool step5 = sendCommand(cmd5, 64);
    success &= step5;
    if (step5) ARMTimer::blockingDelayMicros(999);

        uint8_t cmd6[64] = {0x41, 0x80, 0x00, 0x00};
    bool step6 = sendCommand(cmd6, 64);
    success &= step6;
    if (step6) ARMTimer::blockingDelayMicros(999);
    
    if (!success) {
        Serial.println("⚠️ Activation sequence failed");
    }
    return success;
}