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

// ===== LED COMMAND QUEUE SYSTEM =====
// Using USBHost_t36 proper queuing pattern to avoid USB contention
struct QueuedLEDCommand {
    uint8_t data[64];
    size_t length;
    uint8_t commandType; // 1=Package1, 2=Package2, 3=Apply1, 4=Apply2
    bool priority;       // High priority for button events
};

class LEDCommandQueue {
private:
    static const size_t MAX_QUEUED = 32;
    QueuedLEDCommand commands[MAX_QUEUED];
    volatile size_t head = 0;
    volatile size_t tail = 0;
    volatile size_t count = 0;
    volatile bool processing = false;
    volatile bool batchInsertion = false;  // Flag to prevent processing during batch insertion
    
public:
    bool enqueue(const uint8_t* data, size_t length, uint8_t type, bool priority = false) {
        if (count >= MAX_QUEUED) {
            Serial.println("⚠️  LED command queue full!");
            return false;
        }
        
        __disable_irq();
        
        if (priority) {
            // *** PRIORITY COMMANDS ALWAYS GO TO FRONT (regardless of queue state) ***
            tail = (tail - 1 + MAX_QUEUED) % MAX_QUEUED;
            memcpy(commands[tail].data, data, length);
            commands[tail].length = length;
            commands[tail].commandType = type;
            commands[tail].priority = priority;
            Serial.printf("🚀 HIGH PRIORITY command type %d inserted at FRONT (pos %zu)\n", type, tail);
        } else {
            // *** NORMAL INSERTION: Insert at head position (back of queue) ***
            memcpy(commands[head].data, data, length);
            commands[head].length = length;
            commands[head].commandType = type;
            commands[head].priority = priority;
            head = (head + 1) % MAX_QUEUED;
            Serial.printf("📤 Normal command type %d inserted at BACK (pos %zu)\n", type, (head - 1 + MAX_QUEUED) % MAX_QUEUED);
        }
        
        count++;
        __enable_irq();
        
        // *** PROPER USBHost_t36 PATTERN: Only start processing if not already active ***
        // Don't call processNext() if processing or batch insertion is active
        if (!processing && !batchInsertion) {
            processNext();
        }
        return true;
    }
    
    bool dequeue(QueuedLEDCommand& cmd) {
        if (count == 0) return false;
        
        __disable_irq();
        cmd = commands[tail];
        tail = (tail + 1) % MAX_QUEUED;
        count--;
        __enable_irq();
        
        return true;
    }
    
    void processNext() {
        // *** CRITICAL: Only process if not already processing and queue has items ***
        if (processing || count == 0) return;
        
        processing = true;
        QueuedLEDCommand cmd;
        if (dequeue(cmd)) {
            // *** ACTUALLY SEND THE COMMAND via USBHost_t36 ***
            Serial.printf("📤 Processing queued LED command type %d (queue size: %zu)\n", 
                         cmd.commandType, count + 1);
            
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
        Serial.printf("✅ LED command completed, processing next (queue size: %zu)\n", count);
        processing = false;
        
        // *** Check if batch insertion is active - don't start new processing ***
        if (batchInsertion) {
            Serial.println("⏸️ Batch insertion active - skipping processNext()");
            return;
        }
        
        // *** CRITICAL: 10ms delay between packets for ControlPad device (reduce flicker) ***
        delayMicroseconds(750);
        processNext(); // Process next command in queue
    }
    
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    bool isProcessing() const { return processing; }
    
    // *** BATCH INSERTION CONTROL (prevent processing during multi-command insertion) ***
    void forceStopProcessing() { batchInsertion = true; }
    void resumeProcessing() { 
        batchInsertion = false; 
        processNext(); // Start processing queued commands
    }
};

static LEDCommandQueue ledQueue;

// *** BUTTON DEBOUNCING: Track last button states and timing ***
static uint32_t lastButtonStates[24] = {0}; // Timestamp of last state change for each button
static bool currentButtonStates[24] = {false}; // Current pressed state for each button
static const uint32_t DEBOUNCE_DELAY_MS = 50; // 50ms debouncing

// ===== USB CALLBACK COMPLETION HANDLING =====
bool USBControlPad::hid_process_out_data(const Transfer_t *transfer) {
    // Called by USBHost_t36 when any output transfer completes
    Serial.printf("📨 USB OUT transfer complete - status: 0x%08X\n", transfer->qtd.token);
    
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
    
    // *** SEND COMMAND ***
    Serial.printf("📤 sendCommand: Sending %zu bytes via USBHost_t36...\n", length);
    
    bool success = driver_->sendPacket(const_cast<uint8_t*>(data), length);
    
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
    
    Serial.printf("🔍 HID Process In Data #%lu: Length=%d\n", inputCount, transfer->length);
    
    // *** DIRECT BUTTON PROCESSING - NO QUEUE ***
    // Process button data immediately in the USB callback
    if (transfer->length >= 7) {
        const uint8_t* buffer = (const uint8_t*)transfer->buffer;
        
        if (buffer[0] == 0x43 && buffer[1] == 0x01) {
            uint8_t buttonId = buffer[4];  // USB button ID (0-23)
            uint8_t state = buffer[5];     // Button state
            
            // Apply BUTTON MAPPING - Testing different mappings to find correct one
            if (buttonId < 24) {
                // *** TEST ALL POSSIBLE MAPPINGS ***
                uint8_t directMapping = buttonId;  // Direct mapping: USB Button ID → LED index
                
                // Column-major conversion (vertical to horizontal)
                uint8_t col = buttonId / 5;  // Which column (0-4)
                uint8_t row = buttonId % 5;  // Which row in that column (0-4) 
                uint8_t columnMajorMapping = row * 5 + col;  // Convert to visual button position
                
                // Use the column-major mapping for now (based on user's description)
                uint8_t ledIndex = columnMajorMapping;
                
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
                    // *** ALWAYS UPDATE BUTTON STATE FIRST (regardless of animation) ***
                    extern ControlPadHardware* globalHardwareInstance;
                    if (globalHardwareInstance && globalHardwareInstance->currentPad) {
                        ControlPadEvent event;
                        event.type = ControlPadEventType::Button;
                        event.button.button = ledIndex;
                        event.button.pressed = pressed;
                        
                        // This calls pushEvent which updates buttonState[ledIndex] = pressed
                        globalHardwareInstance->currentPad->pushEvent(event);
                        Serial.printf("📨 Button state updated: Button %d = %s\n", 
                                     ledIndex + 1, pressed ? "PRESSED" : "RELEASED");
                    }
                    
                    // *** DIRECT LED HIGHLIGHTING - IMMEDIATE VISUAL FEEDBACK ***
                    Serial.printf("🎮 Button %d %s (Direct USB callback)\n", 
                                 ledIndex + 1, 
                                 pressed ? "PRESSED" : "RELEASED");
                    
                    // *** MAPPING DEBUG: Show all possible mappings ***
                    Serial.printf("🔍 USB_ID=%d → Direct=%d, ColumnMajor=%d → LED_index=%d (Button %d)\n", 
                                 buttonId, directMapping, columnMajorMapping, ledIndex, ledIndex + 1);
                    
                    // *** CHECK IF ANIMATION IS ACTIVE - IF SO, SKIP DIRECT LED UPDATES ***
                    bool animationActive = (globalHardwareInstance && globalHardwareInstance->isAnimationEnabled());
                    
                    if (animationActive) {
                        // Animation is active - it will handle both animation AND button states
                        // Button state already updated above, LED updates handled by animation
                        Serial.printf("🎭 Animation active - button state updated, LED update handled by animation\n");
                    } else {
                        // Animation not active - use direct LED updates as before
                        Serial.printf("🎯 Sending direct LED update with %s\n", pressed ? "WHITE highlight" : "original colors");
                        
                        // *** USE CORRECT RAINBOW COLORS FROM MAIN.CPP ***
                        ControlPadColor correctRainbowColors[24] = {
                            {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
                            {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
                            {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
                            {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
                            {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
                        };
                        
                        // Create LED color array for the update
                        ControlPadColor ledColors[24];
                        
                        // Copy the correct rainbow base colors
                        for (int i = 0; i < 24; i++) {
                            ledColors[i] = correctRainbowColors[i];
                        }
                        
                        // Apply highlight: White when pressed, original color when released
                        if (pressed) {
                            ledColors[ledIndex] = {255, 255, 255}; // Bright white highlight
                            Serial.printf("💡 Highlighting button %d in WHITE (LED index %d)\n", 
                                         ledIndex + 1, ledIndex);
                        } else {
                            // Return to original rainbow color (already set above)
                            Serial.printf("🎨 Returning button %d to original rainbow color\n", ledIndex + 1);
                        }
                        
                        bool success = updateAllLEDs(ledColors, 24, true); // true = HIGH PRIORITY
                        if (success) {
                            Serial.printf("✅ LED commands queued with HIGH PRIORITY (queue size: %zu)\n", ledQueue.size());
                        } else {
                            Serial.printf("⚠️ HIGH PRIORITY LED command queuing failed\n");
                        }
                    }
                }
            }
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
    Serial.printf("🔧 updateAllLEDs: Starting QUEUED LED update for %zu colors (%s)\n", count, priorityStr);
    
    // *** PROPER PRIORITY HANDLING: For button events, insert entire sequence at front ***
    if (priority) {
        Serial.println("🚀 HIGH PRIORITY: Inserting complete LED sequence at front of queue");
    }
    
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
    package2[4] = 0x00;          // padding
    package2[5] = colors[17].g;  // LED 17 green
    package2[6] = colors[17].b;  // LED 17 blue
    
    // LED 22 
    package2[7] = colors[22].r;  package2[8] = colors[22].g;  package2[9] = colors[22].b;   // LED 22
    
    // Column 4: LED indices 3, 8, 13, 18, 23
    package2[10] = colors[3].r;  package2[11] = colors[3].g;  package2[12] = colors[3].b;   // LED 3
    package2[13] = colors[8].r;  package2[14] = colors[8].g;  package2[15] = colors[8].b;   // LED 8
    package2[16] = colors[13].r; package2[17] = colors[13].g; package2[18] = colors[13].b;  // LED 13
    package2[19] = colors[18].r; package2[20] = colors[18].g; package2[21] = colors[18].b;  // LED 18
    package2[22] = colors[23].r; package2[23] = colors[23].g; package2[24] = colors[23].b;  // LED 23
    
    // Column 5: LED indices 4, 9, 14, 19 (only 4 LEDs - there is no LED 24)
    package2[25] = colors[4].r;  package2[26] = colors[4].g;  package2[27] = colors[4].b;   // LED 4
    package2[28] = colors[9].r;  package2[29] = colors[9].g;  package2[30] = colors[9].b;   // LED 9
    package2[31] = colors[14].r; package2[32] = colors[14].g; package2[33] = colors[14].b;  // LED 14
    package2[34] = colors[19].r; package2[35] = colors[19].g; package2[36] = colors[19].b;  // LED 19
    
    // *** PACKAGE2 BYTES 37-63 AUTO-FILLED WITH 0x00 BY INITIALIZATION ***
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
    
    // *** CRITICAL FIX: For priority commands, insert in REVERSE order so they execute correctly ***
    bool success1, success2, success3, success4;
    
    // *** DEBUG: Show package data and verify 64-byte size ***
    Serial.printf("🔍 Package1 LED data (bytes 24-35): ");
    for (int i = 24; i < 36 && i < 64; i++) {
        Serial.printf("%02X ", package1[i]);
    }
    Serial.println();
    
    Serial.printf("🔍 Package2 LED data (bytes 4-15): ");
    for (int i = 4; i < 16 && i < 64; i++) {
        Serial.printf("%02X ", package2[i]);
    }
    Serial.println();
    
    // *** VALIDATE 64-BYTE PACKET SIZES ***
    Serial.printf("📏 Packet sizes: Package1=%d, Package2=%d, Apply1=%d, Apply2=%d bytes\n", 
                 (int)sizeof(package1), (int)sizeof(package2), (int)sizeof(apply1), (int)sizeof(apply2));
    
    if (priority) {
        // *** BATCH INSERTION: Stop processing during insertion to prevent immediate execution ***
        bool wasProcessing = ledQueue.isProcessing();
        ledQueue.forceStopProcessing();
        
        Serial.println("🚀 Inserting HIGH PRIORITY commands in REVERSE order for correct execution:");
        // Insert in reverse order: Apply2 → Apply1 → Package2 → Package1
        // They will execute as: Package1 → Package2 → Apply1 → Apply2 (correct!)
        
        Serial.printf("📦 Apply2: CMD=0x%02X 0x%02X (Final apply) - %s\n", apply2[0], apply2[1], priorityStr);
        success4 = ledQueue.enqueue(apply2, 64, 4, priority);
        
        Serial.printf("📦 Apply1: CMD=0x%02X 0x%02X (Apply command) - %s\n", apply1[0], apply1[1], priorityStr);
        success3 = ledQueue.enqueue(apply1, 64, 3, priority);
        
        Serial.printf("📦 Package2: CMD=0x%02X 0x%02X 0x%02X (Package 2 of 2) - %s\n", 
                     package2[0], package2[1], package2[2], priorityStr);
        success2 = ledQueue.enqueue(package2, 64, 2, priority);
        
        Serial.printf("📦 Package1: CMD=0x%02X 0x%02X (Package 1 of 2) - %s\n", 
                     package1[0], package1[1], priorityStr);
        success1 = ledQueue.enqueue(package1, 64, 1, priority);
        
        Serial.println("🎯 HIGH PRIORITY sequence inserted - will execute: Package1→Package2→Apply1→Apply2");
        
        // *** RESUME PROCESSING: All 4 commands are now queued, start processing ***
        if (!wasProcessing) {
            ledQueue.resumeProcessing();
        }
    } else {
        // Normal order for regular updates
        Serial.printf("📦 Package1: CMD=0x%02X 0x%02X (Package 1 of 2) - %s\n", 
                     package1[0], package1[1], priorityStr);
        success1 = ledQueue.enqueue(package1, 64, 1, priority);
        
        Serial.printf("📦 Package2: CMD=0x%02X 0x%02X 0x%02X (Package 2 of 2) - %s\n", 
                     package2[0], package2[1], package2[2], priorityStr);
        success2 = ledQueue.enqueue(package2, 64, 2, priority);
        
        Serial.printf("📦 Apply1: CMD=0x%02X 0x%02X (Apply command) - %s\n", 
                     apply1[0], apply1[1], priorityStr);
        success3 = ledQueue.enqueue(apply1, 64, 3, priority);
        
        Serial.printf("📦 Apply2: CMD=0x%02X 0x%02X (Final apply) - %s\n", 
                     apply2[0], apply2[1], priorityStr);
        success4 = ledQueue.enqueue(apply2, 64, 4, priority);
    }
    
    Serial.printf("📤 Package1 queued: %s\n", success1 ? "SUCCESS" : "FAILED");
    Serial.printf("📤 Package2 queued: %s\n", success2 ? "SUCCESS" : "FAILED");
    Serial.printf("📤 Apply1 queued: %s\n", success3 ? "SUCCESS" : "FAILED");
    Serial.printf("📤 Apply2 queued: %s\n", success4 ? "SUCCESS" : "FAILED");
    
    bool overall = success1 && success2 && success3 && success4;
    Serial.printf("🎯 updateAllLEDs QUEUED sequence result: %s (queue size: %zu, %s)\n", 
                  overall ? "SUCCESS" : "FAILED", ledQueue.size(), priorityStr);
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
    Serial.printf("🚀 sendActivationSequence: Starting device activation...\n");
    bool success = true;
    
    // Step 1: 0x42 00 activation
    uint8_t cmd1[64] = {0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    Serial.printf("🚀 Step 1: 0x%02X 0x%02X activation\n", cmd1[0], cmd1[1]);
    bool step1 = sendCommand(cmd1, 64);
    success &= step1;
    if (step1) delay(100); // Allow device to process
    
    // Step 2: 0x42 10 variant  
    uint8_t cmd2[64] = {0x42, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
    Serial.printf("🚀 Step 2: 0x%02X 0x%02X variant\n", cmd2[0], cmd2[1]);
    bool step2 = sendCommand(cmd2, 64);
    success &= step2;
    if (step2) delay(100); // Allow device to process
    
    // Step 3: 0x43 00 button activation
    uint8_t cmd3[64] = {0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    Serial.printf("🚀 Step 3: 0x%02X 0x%02X button activation\n", cmd3[0], cmd3[1]);
    bool step3 = sendCommand(cmd3, 64);
    success &= step3;
    if (step3) delay(100); // Allow device to process
    
    // Step 4: 0x41 80 status
    uint8_t cmd4[64] = {0x41, 0x80, 0x00, 0x00};
    Serial.printf("🚀 Step 4: 0x%02X 0x%02X status\n", cmd4[0], cmd4[1]);
    bool step4 = sendCommand(cmd4, 64);
    success &= step4;
    if (step4) delay(100); // Allow device to process
    
    // Step 5: 0x52 00 activate effect modes
    uint8_t cmd5[64] = {0x52, 0x00, 0x00, 0x00};
    Serial.printf("🚀 Step 5: 0x%02X 0x%02X effect modes\n", cmd5[0], cmd5[1]);
    bool step5 = sendCommand(cmd5, 64);
    success &= step5;
    if (step5) delay(100); // Allow device to process
    
    Serial.printf("🚀 sendActivationSequence overall result: %s\n", success ? "SUCCESS" : "FAILED");
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

// *** ANIMATION SYSTEM: Cycles through buttons with white highlighting ***
class ButtonAnimation {
private:
    static const uint32_t ANIMATION_DELAY_MS = 200; // 200ms per button to reduce conflicts
    uint32_t lastUpdateTime = 0;
    uint8_t currentButton = 0;
    bool animationEnabled = false; // Start disabled
    
    // Base rainbow colors (same as main.cpp)
    ControlPadColor baseColors[24] = {
        {255, 0, 0},     {255, 127, 0},   {255, 255, 0},   {0, 255, 0},     {0, 0, 255},      // Row 1
        {127, 0, 255},   {255, 0, 127},   {255, 255, 255}, {127, 127, 127}, {255, 64, 0},     // Row 2  
        {0, 255, 127},   {127, 255, 0},   {255, 127, 127}, {127, 127, 255}, {255, 255, 127},  // Row 3
        {0, 127, 255},   {255, 0, 255},   {127, 255, 255}, {255, 127, 0},   {127, 0, 127},    // Row 4
        {64, 64, 64},    {128, 128, 128}, {192, 192, 192}, {255, 255, 255}                    // Row 5
    };

public:
    void update() {
        if (!animationEnabled) return;
        
        uint32_t currentTime = millis();
        if (currentTime - lastUpdateTime >= ANIMATION_DELAY_MS) {
            // Get the current ControlPad instance to check button states
            extern ControlPadHardware* globalHardwareInstance;
            ControlPad* currentPad = globalHardwareInstance ? globalHardwareInstance->currentPad : nullptr;
            
            // Create LED array with rainbow base colors + current button states
            ControlPadColor ledColors[24];
            for (int i = 0; i < 24; i++) {
                // Check if this button is currently pressed
                bool isPressed = (currentPad && currentPad->getButtonState(i));
                
                if (isPressed) {
                    // Use white for pressed buttons
                    ledColors[i] = {255, 255, 255};
                } else {
                    // Use rainbow for unpressed buttons
                    ledColors[i] = baseColors[i];
                }
            }
            
            // Apply animation highlight (white) - this will show on unpressed buttons
            // For pressed buttons, they're already white so animation highlight is invisible
            ledColors[currentButton] = {255, 255, 255};
            
            // Send to hardware - when animation is active, it's the ONLY LED updater
            bool success = globalControlPadDriver.updateAllLEDs(ledColors, 24, false);
            if (success) {
                Serial.printf("🎭 Animation: Button %d highlighted\n", currentButton + 1);
            } else {
                Serial.println("⚠️ Animation frame failed to queue");
            }
            
            // Move to next button
            currentButton = (currentButton + 1) % 24;
            lastUpdateTime = currentTime;
        }
    }
    
    void enable() { 
        animationEnabled = true; 
        Serial.println("🎭 Button animation ENABLED - cycling through all buttons");
    }
    
    void disable() { 
        animationEnabled = false; 
        Serial.println("🎭 Button animation DISABLED");
    }
    
    bool isEnabled() const { return animationEnabled; }
    uint8_t getCurrentButton() const { return currentButton; }
};

static ButtonAnimation buttonAnimation;

// *** PUBLIC ANIMATION CONTROL ***
void ControlPadHardware::enableAnimation() {
    buttonAnimation.enable();
}

void ControlPadHardware::disableAnimation() {
    buttonAnimation.disable();
}

void ControlPadHardware::updateAnimation() {
    buttonAnimation.update();
}

bool ControlPadHardware::isAnimationEnabled() const {
    return buttonAnimation.isEnabled();
}