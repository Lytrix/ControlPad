#include "ControlPadHardware.h"
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
// Using USBHost_t36 proper queuing pattern to avoid USB contention
struct QueuedLEDCommand {
    uint8_t data[64];
    size_t length;
    uint8_t commandType; // 1=Package1, 2=Package2, 3=Apply1, 4=Apply2
    bool priority;       // High priority for button events
};

// Global queue size configuration
static const size_t GLOBAL_LED_QUEUE_SIZE = 128
;  // Optimized buffer for MIDI application 

// Forward declaration for static member
class LEDCommandQueue;

class LEDCommandQueue {
private:
    QueuedLEDCommand commands[GLOBAL_LED_QUEUE_SIZE];
    volatile size_t head = 0;
    volatile size_t tail = 0;
    volatile size_t count = 0;
    volatile bool processing = false;
    
    // *** SIMPLIFIED INTERRUPT-SAFE MUTEX PROTECTION ***
    // Simple atomic operations without timeouts or complex logic
    
    inline void enterCritical() const {
        noInterrupts();
    }
    
    inline void exitCritical() const {
        interrupts();
    }
    
    // Simple atomic read operations
    size_t atomicReadCount() const {
        enterCritical();
        size_t result = count;
        exitCritical();
        return result;
    }
    
    size_t atomicReadHead() const {
        enterCritical();
        size_t result = head;
        exitCritical();
        return result;
    }
    
    size_t atomicReadTail() const {
        enterCritical();
        size_t result = tail;
        exitCritical();
        return result;
    }
    
public:
    // *** ATOMIC LED DATA PACKET ENQUEUING for Package1 & Package2 ***
    bool enqueueLEDData(const uint8_t* pkg1, const uint8_t* pkg2, bool priority = false) {
        // Check if we have space for 2 LED data packets atomically
        if (atomicReadCount() > (GLOBAL_LED_QUEUE_SIZE - 2)) {
            Serial.printf("⚠️ LED queue full - need 2 slots, have %zu available\n", GLOBAL_LED_QUEUE_SIZE - atomicReadCount());
            digitalWrite(DEBUG_PIN_QUEUE_FULL, HIGH);
            delayMicroseconds(50);
            digitalWrite(DEBUG_PIN_QUEUE_FULL, LOW);
            return false;
        }
        
        enterCritical();
        
        if (priority) {
            // *** HIGH PRIORITY: Insert LED data packets at front ***
            tail = (tail - 2 + GLOBAL_LED_QUEUE_SIZE) % GLOBAL_LED_QUEUE_SIZE;
            size_t pos = tail;
            
            // Package1
            memcpy(commands[pos].data, pkg1, 64);
            commands[pos].length = 64;
            commands[pos].commandType = 1;
            commands[pos].priority = priority;
            pos = (pos + 1) % GLOBAL_LED_QUEUE_SIZE;
            
            // Package2
            memcpy(commands[pos].data, pkg2, 64);
            commands[pos].length = 64;
            commands[pos].commandType = 2;
            commands[pos].priority = priority;
            
        } else {
            // *** NORMAL PRIORITY: Insert at head (back of queue) ***
            size_t pos = head;
            
            // Package1
            memcpy(commands[pos].data, pkg1, 64);
            commands[pos].length = 64;
            commands[pos].commandType = 1;
            commands[pos].priority = priority;
            pos = (pos + 1) % GLOBAL_LED_QUEUE_SIZE;
            
            // Package2
            memcpy(commands[pos].data, pkg2, 64);
            commands[pos].length = 64;
            commands[pos].commandType = 2;
            commands[pos].priority = priority;
            
            head = (head + 2) % GLOBAL_LED_QUEUE_SIZE;
        }
        
        count += 2;
        
        exitCritical();
        
        if (!processing) {
            processNext();
        }
        
        return true;
    }
    
    // *** SINGLE PACKET ENQUEUING for Apply commands ***
    bool enqueueSingle(const uint8_t* data, uint8_t type, bool priority = false) {
        if (atomicReadCount() >= GLOBAL_LED_QUEUE_SIZE) {
            return false;
        }
        
        enterCritical();
        
        if (priority) {
            tail = (tail - 1 + GLOBAL_LED_QUEUE_SIZE) % GLOBAL_LED_QUEUE_SIZE;
            memcpy(commands[tail].data, data, 64);
            commands[tail].length = 64;
            commands[tail].commandType = type;
            commands[tail].priority = priority;
        } else {
            memcpy(commands[head].data, data, 64);
            commands[head].length = 64;
            commands[head].commandType = type;
            commands[head].priority = priority;
            head = (head + 1) % GLOBAL_LED_QUEUE_SIZE;
        }
        
        count++;
        
        exitCritical();
        
        if (!processing) {
            processNext();
        }
        return true;
    }
    
    // *** DEPRECATED - Use enqueueSequence instead ***
    bool enqueue(const uint8_t* data, size_t length, uint8_t type, bool priority = false) {
        Serial.println("⚠️ WARNING: Using deprecated single enqueue() - use enqueueSequence() for atomic operations");
        
        if (atomicReadCount() >= GLOBAL_LED_QUEUE_SIZE) {
            Serial.println("⚠️ LED command queue full!");
            return false;
        }
        
        enterCritical();
        
        if (priority) {
            tail = (tail - 1 + GLOBAL_LED_QUEUE_SIZE) % GLOBAL_LED_QUEUE_SIZE;
            memcpy(commands[tail].data, data, length);
            commands[tail].length = length;
            commands[tail].commandType = type;
            commands[tail].priority = priority;
        } else {
            memcpy(commands[head].data, data, length);
            commands[head].length = length;
            commands[head].commandType = type;
            commands[head].priority = priority;
            head = (head + 1) % GLOBAL_LED_QUEUE_SIZE;
        }
        
        count++;
        
        exitCritical();
        
        if (!processing) {
            processNext();
        }
        return true;
    }
    
    bool dequeue(QueuedLEDCommand& cmd) {
        if (atomicReadCount() == 0) return false;
        
        enterCritical();
        
        cmd = commands[tail];
        tail = (tail + 1) % GLOBAL_LED_QUEUE_SIZE;
        count--;
        
        exitCritical();
        
        return true;
    }
    
    void processNext() {
        // *** CRITICAL: Only process if not already processing and queue has items ***
        if (processing || atomicReadCount() == 0) return;
        
        processing = true;
        QueuedLEDCommand cmd;
        if (dequeue(cmd)) {
            // *** ACTUALLY SEND THE COMMAND via USBHost_t36 ***
            
            bool success = globalControlPadDriver.sendCommand(cmd.data, cmd.length);
            if (!success) {
                // If send failed, allow processing to continue immediately
                processing = false;
                Serial.printf("❌ LED command type %d FAILED - retrying next in queue\n", cmd.commandType);
                processNext(); // Try next command immediately
            }
            // If success, processing flag stays true until USB callback clears it
        } else {
            processing = false;
        }
    }
    
    void onCommandComplete() {
        // *** Called from USB callback when command completes ***
        processing = false;
        processNext(); // Process next command in queue
    }
    
    size_t size() const { return atomicReadCount(); }
    bool empty() const { return atomicReadCount() == 0; }
    bool isProcessing() const { return processing; }
    size_t getHead() const { return atomicReadHead(); }
    size_t getTail() const { 
        return atomicReadTail();
    }

};

// Simplified mutex - no static members needed

static LEDCommandQueue ledQueue;

// ===== ENHANCED USB BANDWIDTH MONITORING WITH ADAPTIVE CONTROL =====
struct USBBandwidthMonitor {
    uint32_t transfers_per_second = 0;
    uint32_t bytes_per_second = 0;
    uint32_t last_report_time = 0;
    uint32_t transfer_count = 0;
    uint32_t byte_count = 0;
    uint32_t last_queue_position = 999; // Track queue wraparound
    
    // *** SATURATION DETECTION AND ADAPTIVE TIMING ***
    uint32_t saturation_events = 0;
    uint32_t consecutive_high_queue = 0;
    bool is_saturated = false;
    uint32_t last_saturation_check = 0;
    uint32_t recommended_interval = 50; // Start with 50ms, adapt based on conditions
    
    // *** NEW: Track high position frequency over time window ***
    static const uint8_t POSITION_HISTORY_SIZE = 20;
    size_t position_history[POSITION_HISTORY_SIZE];
    uint8_t history_index = 0;
    bool history_filled = false;
    
    void recordTransfer(size_t bytes) {
        transfer_count++;
        byte_count += bytes;
        
        // *** ENHANCED QUEUE MONITORING WITH SATURATION DETECTION ***
        size_t current_queue_size = ledQueue.size();
        size_t current_queue_pos = (ledQueue.getHead() + current_queue_size - 1) % GLOBAL_LED_QUEUE_SIZE;
        
        // *** SATURATION DETECTION ALGORITHM ***
        detectSaturation(current_queue_size, current_queue_pos);
        
        // Detect significant wraparounds only (reduced logging) 
        if (last_queue_position > current_queue_pos && last_queue_position > (GLOBAL_LED_QUEUE_SIZE - 8)) {
            Serial.printf("🔄 WRAPAROUND: %lu → %lu (size=%zu) %s\n", 
                         last_queue_position, current_queue_pos, current_queue_size,
                         is_saturated ? "[SATURATED]" : "[OK]");
        }
        
        last_queue_position = current_queue_pos;
        
        // Report bandwidth and recommendations every 5 seconds
        if (millis() - last_report_time > 5000) {
            reportStatus();
            transfer_count = 0;
            byte_count = 0;
            last_report_time = millis();
        }
    }
    
    void detectSaturation(size_t queue_size, size_t queue_pos) {
        uint32_t now = millis();
        
        // Check for saturation indicators every 100ms
        if (now - last_saturation_check < 100) return;
        last_saturation_check = now;
        
        // *** NEW: Record position history for frequency analysis ***
        position_history[history_index] = queue_pos;
        history_index = (history_index + 1) % POSITION_HISTORY_SIZE;
        if (history_index == 0) history_filled = true;
        
        // Only analyze if we have enough history
        if (!history_filled && history_index < 10) return;
        
        // *** FREQUENCY-BASED SATURATION DETECTION ***
        // Count how many recent positions were >90% of buffer capacity
        size_t high_threshold = (size_t)(GLOBAL_LED_QUEUE_SIZE * 0.9); // >115 for 128-item buffer
        uint8_t high_position_count = 0;
        uint8_t samples_to_check = history_filled ? POSITION_HISTORY_SIZE : history_index;
        
        for (uint8_t i = 0; i < samples_to_check; i++) {
            if (position_history[i] > high_threshold) {
                high_position_count++;
            }
        }
        
        // Saturation if >60% of recent positions are high (12/20 or 6/10)
        float high_position_ratio = (float)high_position_count / samples_to_check;
        bool was_saturated = is_saturated;
        is_saturated = (high_position_ratio > 0.6);
        
        if (is_saturated && !was_saturated) {
            saturation_events++;
            adaptTiming();
            Serial.printf("🚨 USB SATURATION DETECTED! %u/%u positions >%zu (%.1f%%), Event #%lu - Adapting timing to %lums\n", 
                         high_position_count, samples_to_check, high_threshold, 
                         high_position_ratio * 100.0f, saturation_events, recommended_interval);
        } else if (!is_saturated && was_saturated) {
            Serial.printf("✅ USB saturation cleared - %u/%u positions >%zu (%.1f%%) back to normal\n",
                         high_position_count, samples_to_check, high_threshold, high_position_ratio * 100.0f);
        }
    }
    
    void adaptTiming() {
        // Adaptive timing algorithm based on saturation frequency
        if (saturation_events == 1) {
            recommended_interval = 75;  // First saturation: slow down moderately
        } else if (saturation_events <= 3) {
            recommended_interval = 100; // Multiple saturations: slow down more
        } else if (saturation_events <= 5) {
            recommended_interval = 150; // Persistent issues: much slower
        } else {
            recommended_interval = 200; // Severe issues: fallback to safest timing
        }
    }
    
    void reportStatus() {
        if (saturation_events > 0) {
            Serial.printf("📊 USB Status: %s | Queue avg: %zu | Saturations: %lu | Recommended interval: %lums\n",
                         is_saturated ? "SATURATED" : "OK", 
                         ledQueue.size(), saturation_events, recommended_interval);
        }
    }
    
    uint32_t getRecommendedInterval() const {
        return recommended_interval;
    }
    
    bool isSaturated() const {
        return is_saturated;
    }
};

static USBBandwidthMonitor bandwidthMonitor;

// *** UNIFIED LED STATE MANAGER WITH BUTTON BATCHING ***
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
    
    // *** BUTTON BATCHING SYSTEM ***
    bool buttonStateChanged = false;
    uint32_t firstButtonChangeTime = 0;
    uint32_t lastButtonChangeTime = 0;
    static const uint32_t BUTTON_BATCH_TIMEOUT_MS = 20; // Collect button changes for 20ms
    static const uint32_t MAX_BUTTON_BATCH_DELAY_MS = 50; // Never delay more than 50ms
    uint8_t buttonChangesInBatch = 0;
    
public:
    void setAnimationEnabled(bool enabled) {
        animationEnabled = enabled;
        stateChanged = true; // Force update when animation state changes
    }
    
    bool isAnimationEnabled() const { return animationEnabled; }
    
    void setButtonState(uint8_t buttonIndex, bool pressed) {
        if (buttonIndex < 24 && buttonStates[buttonIndex] != pressed) {
            buttonStates[buttonIndex] = pressed;
            
            // *** BUTTON BATCHING LOGIC ***
            uint32_t now = millis();
            
            if (!buttonStateChanged) {
                // First button change in a potential batch
                firstButtonChangeTime = now;
                buttonStateChanged = true;
                buttonChangesInBatch = 1;
                Serial.printf("🎮 Button batch started: Button %u %s\n", 
                             buttonIndex, pressed ? "pressed" : "released");
            } else {
                // Additional button change - extend batch
                buttonChangesInBatch++;
                Serial.printf("🎮 Button batch +%u: Button %u %s (total: %u changes)\n", 
                             buttonChangesInBatch, buttonIndex, pressed ? "pressed" : "released", buttonChangesInBatch);
            }
            
            lastButtonChangeTime = now;
        }
    }
    
    void updateLEDState() {
        if (!stateChanged) return;
        
        // Start with base colors
        for (int i = 0; i < 24; i++) {
            currentLEDState[i] = BASE_COLORS[i];
        }
        
        // Apply animation if enabled
        if (animationEnabled) {
            // Cycle through buttons with adaptive timing based on USB bandwidth
            uint32_t currentTime = millis();
            uint32_t adaptiveInterval = bandwidthMonitor.getRecommendedInterval();
            
            if (currentTime - animationTime >= adaptiveInterval) {
                animationStep = (animationStep + 1) % 24;
                animationTime = currentTime;
            }
            currentLEDState[animationStep] = {255, 255, 255}; // White animation highlight
        }
        
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
        
        // *** BUTTON BATCHING: Check if we should send batched button updates ***
        if (buttonStateChanged) {
            uint32_t timeSinceFirstChange = currentTime - firstButtonChangeTime;
            uint32_t timeSinceLastChange = currentTime - lastButtonChangeTime;
            
            // Send batched update if:
            // 1. No new button changes for BUTTON_BATCH_TIMEOUT_MS (20ms quiet period)
            // 2. OR we've been collecting for MAX_BUTTON_BATCH_DELAY_MS (50ms max delay)
            if (timeSinceLastChange >= BUTTON_BATCH_TIMEOUT_MS || 
                timeSinceFirstChange >= MAX_BUTTON_BATCH_DELAY_MS) {
                
                Serial.printf("🎮 Sending button batch: %u changes over %lums (quiet for %lums)\n",
                             buttonChangesInBatch, timeSinceFirstChange, timeSinceLastChange);
                             
                // Reset batching state and trigger LED update
                buttonStateChanged = false;
                buttonChangesInBatch = 0;
                stateChanged = true;
                return true;
            }
            // Still collecting button changes, don't send yet
            return false;
        }
        
        // Regular animation updates (if no button batching in progress)
        if (stateChanged) return true;
        
        // For animation: use adaptive timing based on USB bandwidth monitoring
        if (animationEnabled) {
            // Get recommended interval from bandwidth monitor
            uint32_t adaptiveInterval = bandwidthMonitor.getRecommendedInterval();
            
            // Update animation step if enough time has passed (adaptive timing)
            if (currentTime - animationTime >= adaptiveInterval) {
                uint8_t nextStep = (animationStep + 1) % 24;
                if (nextStep != lastSentAnimationStep) {
                    stateChanged = true; // Animation step changed, need to send update
                    return true;
                }
            }
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
    
    // Only report failures immediately, skip regular health reports
    if (failed_transfers > 0 && millis() - last_failure_time < 500) {
        // Keep failure reporting for critical issues
    }
    
    // *** HARDWARE DEBUG: USB transfer completed ***
    digitalWrite(DEBUG_PIN_USB_START, LOW);   // Clear transfer start pin
    digitalWrite(DEBUG_PIN_USB_COMPLETE, HIGH);
    delayMicroseconds(10);  // 10us pulse for scope trigger
    digitalWrite(DEBUG_PIN_USB_COMPLETE, LOW);
    
    // *** CRITICAL: Notify queue that command completed and process next ***
    ledQueue.onCommandComplete();
    
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
    
    // *** SEND COMMAND ***
    
    // Record bandwidth usage
    bandwidthMonitor.recordTransfer(length);
    
    bool success = driver_->sendPacket(const_cast<uint8_t*>(data), length);
    
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

bool USBControlPad::begin(DMAQueue<controlpad_event, 16> *q) {
    queue = q;
    if (queue != nullptr) {
        Serial.println("🎯 USB DRIVER BEGIN - Starting with HID Input only...");
        initialized = true;
        Serial.println("✅ USB Driver initialization complete");
        
        // Send activation sequence - device might need this before accepting LED commands
        Serial.println("🚀 Sending device activation sequence...");
        sendActivationSequence();
    }
    return true;
}

bool USBControlPad::updateAllLEDs(const ControlPadColor* colors, size_t count, bool priority) {
    if (!initialized || count > 24) {
        Serial.printf("❌ updateAllLEDs failed: initialized=%d, count=%zu\n", initialized, count);
        return false;
    }
    
    const char* priorityStr = priority ? "HIGH PRIORITY" : "normal";
    
    // *** ALL 4 PACKETS REQUIRED: Both apply commands are essential for LEDs to work ***
    
    // *** CORRECT LED COMMAND SEQUENCE FROM USER DOCUMENTATION ***
    
    // COMMAND 2: Package 1 of 2 (0x56 0x83)
    uint8_t package1[64] = {0};
    package1[0] = 0x56;  // Vendor ID
    package1[1] = 0x83;  // LED package command
    package1[2] = 0x00;  // Package 1 indicator
    package1[3] = 0x00;
    package1[4] = 0x01;  // Unknown field
    package1[5] = 0x00;
    package1[6] = 0x00;
    package1[7] = 0x00;
    package1[8] = 0x80;  // Unknown field
    package1[9] = 0x01;
    package1[10] = 0x00;
    package1[11] = 0x00;
    package1[12] = 0xFF; // Brightness
    package1[13] = 0x00;
    package1[14] = 0x00;
    package1[15] = 0x00;
    package1[16] = 0x00;
    package1[17] = 0x00;
    package1[18] = 0xFF; // Global brightness
    package1[19] = 0xFF; // Global brightness
    package1[20] = 0x00;
    package1[21] = 0x00;
    package1[22] = 0x00;
    package1[23] = 0x00;
    
    // LED data starts at byte 24 - Direct assignments for performance (no loops)
    // Column-major layout: Column 1, Column 2, Column 3 (partial), etc.
    
    // *** PACKAGE 1: DIRECT ASSIGNMENTS FOR MAXIMUM PERFORMANCE ***
    // Column 1: LED indices 0, 5, 10, 15, 20
    package1[24] = colors[0].r;  package1[25] = colors[0].g;  package1[26] = colors[0].b;   // LED 0
    package1[27] = colors[5].r;  package1[28] = colors[5].g;  package1[29] = colors[5].b;   // LED 5
    package1[30] = colors[10].r; package1[31] = colors[10].g; package1[32] = colors[10].b;  // LED 10
    package1[33] = colors[15].r; package1[34] = colors[15].g; package1[35] = colors[15].b;  // LED 15
    package1[36] = colors[20].r; package1[37] = colors[20].g; package1[38] = colors[20].b;  // LED 20
    
    // Column 2: LED indices 1, 6, 11, 16, 21
    package1[39] = colors[1].r;  package1[40] = colors[1].g;  package1[41] = colors[1].b;   // LED 1
    package1[42] = colors[6].r;  package1[43] = colors[6].g;  package1[44] = colors[6].b;   // LED 6
    package1[45] = colors[11].r; package1[46] = colors[11].g; package1[47] = colors[11].b;  // LED 11
    package1[48] = colors[16].r; package1[49] = colors[16].g; package1[50] = colors[16].b;  // LED 16
    package1[51] = colors[21].r; package1[52] = colors[21].g; package1[53] = colors[21].b;  // LED 21
    
    // Column 3: LED indices 2, 7, 12, 17 (partial - only 4 complete LEDs fit)
    package1[54] = colors[2].r;  package1[55] = colors[2].g;  package1[56] = colors[2].b;   // LED 2
    package1[57] = colors[7].r;  package1[58] = colors[7].g;  package1[59] = colors[7].b;   // LED 7
    package1[60] = colors[12].r; package1[61] = colors[12].g; package1[62] = colors[12].b;  // LED 12
    package1[63] = colors[17].r; // LED 17 (partial - only red channel fits)
    
    // COMMAND 3: Package 2 of 2 (0x56 0x83 0x01)
    uint8_t package2[64] = {0};
    package2[0] = 0x56;  // Vendor ID
    package2[1] = 0x83;  // LED package command
    package2[2] = 0x01;  // Package 2 indicator
    package2[3] = 0x00;
    
    // *** PACKAGE 2: DIRECT ASSIGNMENTS FOR MAXIMUM PERFORMANCE ***
    // Complete LED 17 (green, blue channels from Package 1)
    package2[4] = colors[17].g;  // LED 17 green
    package2[5] = colors[17].b;  // LED 17 blue
    
    // LED 22 
    package2[6] = colors[22].r;  package2[7] = colors[22].g;  package2[8] = colors[22].b;   // LED 22
    
    // Column 4: LED indices 3, 8, 13, 18, 23
    package2[9] = colors[3].r;   package2[10] = colors[3].g;  package2[11] = colors[3].b;   // LED 3
    package2[12] = colors[8].r;  package2[13] = colors[8].g;  package2[14] = colors[8].b;   // LED 8
    package2[15] = colors[13].r; package2[16] = colors[13].g; package2[17] = colors[13].b;  // LED 13
    package2[18] = colors[18].r; package2[19] = colors[18].g; package2[20] = colors[18].b;  // LED 18
    package2[21] = colors[23].r; package2[22] = colors[23].g; package2[23] = colors[23].b;  // LED 23
    
    // Column 5: LED indices 4, 9, 14, 19 (only 4 LEDs - there is no LED 24)
    package2[24] = colors[4].r;  package2[25] = colors[4].g;  package2[26] = colors[4].b;   // LED 4
    package2[27] = colors[9].r;  package2[28] = colors[9].g;  package2[29] = colors[9].b;   // LED 9
    package2[30] = colors[14].r; package2[31] = colors[14].g; package2[32] = colors[14].b;  // LED 14
    package2[33] = colors[19].r; package2[34] = colors[19].g; package2[35] = colors[19].b;  // LED 19
    
    // *** PACKAGE2 BYTES 36-63 AUTO-FILLED WITH 0x00 BY INITIALIZATION ***
    // uint8_t package2[64] = {0} ensures all 64 bytes are zero-filled
    
    // COMMAND 4: Apply command (0x41 0x80)
    uint8_t apply1[64] = {0};
    apply1[0] = 0x41;
    apply1[1] = 0x80;
    
    // COMMAND 5: Final apply with brightness (0x51 0x28)  
    uint8_t apply2[64] = {0};
    apply2[0] = 0x51;
    apply2[1] = 0x28;
    apply2[4] = 0xFF; // Brightness
    
    // *** SPLIT ATOMIC ENQUEUING: LED data (2 packets) + Both apply commands (2 singles) ***
    bool ledSuccess = ledQueue.enqueueLEDData(package1, package2, priority);
    bool applySuccess1 = false, applySuccess2 = false;
    
    if (ledSuccess) {
        // Send both apply commands - both are essential for LEDs to work
        applySuccess1 = ledQueue.enqueueSingle(apply1, 3, priority);
        applySuccess2 = ledQueue.enqueueSingle(apply2, 4, priority);
    }
    
    bool success = ledSuccess && applySuccess1 && applySuccess2;
    
    // Only log failures and high-priority events to reduce serial overhead
    if (!success) {
        if (!ledSuccess) {
            Serial.printf("❌ LED data FAILED - queue full (%s)\n", priorityStr);
        } else {
            Serial.printf("❌ Apply commands FAILED (%s)\n", priorityStr);
        }
    } else if (priority) {
        Serial.printf("🚀 HIGH PRIORITY sequence (4 packets) enqueued (%s)\n", priorityStr);
    }
    
    bool overall = success;
    return overall;
}

// *** REMOVED ALL COMPLEX LED METHODS ***
// No more async processing, retries, or blocking delays
// USBHost_t36 handles everything for us

// Essential LED command functions using HID output reports
bool USBControlPad::setCustomMode() {
    Serial.printf("🎨 setCustomMode: Setting device to custom LED mode (CORRECT FORMAT)...\n");
    
    // COMMAND 1: Set effect mode (from user documentation)
    // 56 81 0000 01000000 02000000 bbbbbbbb (custom mode) + trailing zeros
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
    // Rest remains zeros
    
    Serial.printf("🎨 Custom mode command: 0x%02X 0x%02X (CORRECT custom mode setup)\n", 
                 cmd[0], cmd[1]);
    
    bool result = sendCommand(cmd, 64);
    Serial.printf("🎨 setCustomMode result: %s\n", result ? "SUCCESS" : "FAILED");
    return result;
}

// *** REMOVED ALL OTHER LED METHODS ***
// Keeping only the essential activation and the simplified updateAllLEDs

bool USBControlPad::sendActivationSequence() {
    // Based on the original working activation sequence
    bool success = true;
    
    // Step 1: 0x42 00 activation
    uint8_t cmd1[64] = {0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    bool step1 = sendCommand(cmd1, 64);
    success &= step1;
    if (step1) delay(100); // Allow device to process
    
    // Step 2: 0x42 10 variant  
    uint8_t cmd2[64] = {0x42, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    bool step2 = sendCommand(cmd2, 64);
    success &= step2;
    if (step2) delay(100); // Allow device to process
    
    // Step 3: 0x43 00 button activation
    uint8_t cmd3[64] = {0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    bool step3 = sendCommand(cmd3, 64);
    success &= step3;
    if (step3) delay(100); // Allow device to process
    
    // Step 4: 0x41 80 status
    uint8_t cmd4[64] = {0x41, 0x80, 0x00, 0x00};
    bool step4 = sendCommand(cmd4, 64);
    success &= step4;
    if (step4) delay(100); // Allow device to process
    
    // Step 5: 0x52 00 activate effect modes
    uint8_t cmd5[64] = {0x52, 0x00, 0x00, 0x00};
    bool step5 = sendCommand(cmd5, 64);
    success &= step5;
    if (step5) delay(100); // Allow device to process
    
    if (!success) {
        Serial.println("⚠️ Activation sequence failed");
    }
    return success;
}

// ===== PUBLIC LED QUEUE ACCESS FOR MONITORING =====
void getLEDQueueStatus(size_t* queueSize, bool* isProcessing) {
    if (queueSize) *queueSize = ledQueue.size();
    if (isProcessing) *isProcessing = ledQueue.isProcessing();
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
    
    // 1. Initialize DMA queues
    if (!controlpad_queue.begin()) {
        return false;
    }
    
    if (!led_command_queue.begin()) {
        return false;
    }
    
    // 2. Use global USB Host and Driver (USBHost_t36 standard pattern)
    // USB Host is already started in main.cpp
    
    // 3. Allow USB host to discover and enumerate devices
    for (int i = 0; i < 100; i++) {  // Allow up to 10 seconds for enumeration
        globalUSBHost.Task();  // This processes USB enumeration and calls claim()
        
        delay(100);
        if (globalControlPadDriver.device_) {
            break;
        }
    }
    
    // 4. Wait for device enumeration
    unsigned long startTime = millis();
    while (!globalControlPadDriver.device_ && (millis() - startTime) < 10000) {
        delay(100);
    }
    
    if (!globalControlPadDriver.device_) {
        return false;
    }
    
    // 5. Wait for USB configuration to stabilize
    delay(2000);
    
    // 6. Initialize the driver
    bool initResult = globalControlPadDriver.begin(&controlpad_queue);
    if (!initResult) {
        return false;
    }
    
    // 7. Set device to custom LED mode once during initialization
    globalControlPadDriver.setCustomMode();
        delay(50); // Allow mode change to complete
    
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
    return globalControlPadDriver.updateAllLEDs(colors, count);
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
    // Check if we need to send an LED update
    if (ledManager.shouldSendUpdate()) {
        const ControlPadColor* ledState = ledManager.getLEDState();
        bool success = globalControlPadDriver.updateAllLEDs(ledState, 24, false); // NORMAL PRIORITY
        if (!success) {
            Serial.println("⚠️ Unified LED update failed to queue");
        }
    }
}

bool ControlPadHardware::isAnimationEnabled() const {
    return ledManager.isAnimationEnabled();
}