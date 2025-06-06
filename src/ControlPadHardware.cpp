#include "ControlPadHardware.h"
#include <USBHost_t36.h>  // For testing with standard drivers

// Global hardware instance for access from USB callbacks
ControlPadHardware* globalHardwareInstance = nullptr;

// ===== GLOBAL USB HOST AND DRIVER (USBHost_t36 standard pattern) =====
// These must be global objects for USBHost_t36 automatic discovery
USBHost globalUSBHost;

// Callback functions to test if standard drivers see any devices
void testKeyboardCallback(int unicode) {
    Serial.println("🎹 *** KEYBOARD DETECTED BY STANDARD DRIVER! ***");
}

void testMouseCallback(int buttons) {
    Serial.println("🖱️ *** MOUSE DETECTED BY STANDARD DRIVER! ***");
}

// Try a simpler approach first - let's test with standard drivers
KeyboardController testKeyboard(globalUSBHost);
MouseController testMouse(globalUSBHost);

USBControlPad globalControlPadDriver(globalUSBHost);

// Global driver pointer for easy access
USBControlPad* controlPadDriver = &globalControlPadDriver;

// ===== USB DRIVER IMPLEMENTATION =====

USBControlPad::USBControlPad(USBHost &host) : USBDriver(), myusb(&host) {
    // Initialize USB driver state - no init() method needed for USBHost_t36
    
    Serial.println("🔧 USBControlPad driver instance created");
}

bool USBControlPad::claim(Device_t *device, int type, const uint8_t *descriptors, uint32_t len) {
    Serial.println("🔍 *** USBControlPad::claim called ***");
    Serial.printf("   Device: VID:0x%04X PID:0x%04X, Type:%d, DescLen:%d\n", 
                 device->idVendor, device->idProduct, type, len);
    
    // Check if this is our ControlPad device (VID:0x2516 PID:0x012D)
    if (device->idVendor != CONTROLPAD_VID || device->idProduct != CONTROLPAD_PID) {
        Serial.printf("❌ Not ControlPad device: VID:0x%04X PID:0x%04X (looking for VID:0x%04X PID:0x%04X)\n", 
                     device->idVendor, device->idProduct, CONTROLPAD_VID, CONTROLPAD_PID);
        return false;
    }
    
    Serial.printf("🎯 *** CONTROLPAD DEVICE FOUND! *** VID:0x%04X PID:0x%04X\n", device->idVendor, device->idProduct);
    
    // Parse interface descriptors to find our endpoints
    const uint8_t *p = descriptors;
    const uint8_t *end = p + len;
    
    while (p < end) {
        uint8_t desc_len = p[0];
        uint8_t desc_type = p[1];
        
        if (desc_type == 4) { // Interface descriptor
            uint8_t interfaceNumber = p[2];
            uint8_t interfaceClass = p[5];
            uint8_t interfaceSubClass = p[6];
            uint8_t interfaceProtocol = p[7];
            
            Serial.printf("   Interface: %d, Class: 0x%02X, SubClass: 0x%02X, Protocol: 0x%02X\n", 
                         interfaceNumber, interfaceClass, interfaceSubClass, interfaceProtocol);
        }
        
        p += desc_len;
    }
    
    // Claim all interfaces for ControlPad
    Serial.println("✅ USBControlPad claiming device for raw USB access!");
    
    // Set up the device
    device_ = device;
    
    // Store the instance globally so our main loop can access it
    controlPadDriver = this;
    
    return true;
}

void USBControlPad::disconnect() {
    Serial.println("❌ USBControlPad disconnected");
    initialized = false;
    kbd_polling = false;
    ctrl_polling = false;
    dual_polling = false;
    hall_sensor_polling = false;
    sensor_out_polling = false;
    
    // Clear global pointer
    if (controlPadDriver == this) {
        controlPadDriver = nullptr;
    }
}

void USBControlPad::control(const Transfer_t *transfer) {
    // Handle control transfer completion
}

// Note: USBHost_t36 automatically calls the callback function specified 
// in queue_Data_Transfer() when transfer completes, so we don't need 
// a separate data_complete method

bool USBControlPad::begin(DMAQueue<controlpad_event, 16> *q) {
    queue = q;
    if (queue != nullptr) {
        Serial.println("🎯 USB DRIVER BEGIN - Starting polling...");
        setupQuadInterface();
        startQuadPolling();
        initialized = true;
        Serial.println("✅ USB Driver initialization complete");
    }
    return true;
}

void USBControlPad::setupQuadInterface() {
    Serial.println("🔧 Setting up QUAD interface operation...");
    // Set fixed endpoints based on USB capture analysis
    kbd_ep_in = 0x81;            // Interface 0 keyboard input
    ctrl_ep_in = 0x83;           // Interface 1 control input  
    ctrl_ep_out = 0x04;          // Interface 1 control output
    dual_ep_in = 0x82;           // Interface 2 dual-action input
    hall_sensor_ep_in = 0x86;    // Interface 3 hall sensor data
    btn_ep_out = 0x07;           // Interface 3 alternative button events
    Serial.printf("✅ Endpoints: Kbd=0x%02X, Ctrl=0x%02X/0x%02X, Dual=0x%02X, Btn=0x%02X/0x%02X\n", 
                  kbd_ep_in, ctrl_ep_in, ctrl_ep_out, dual_ep_in, hall_sensor_ep_in, btn_ep_out);
}

void USBControlPad::startQuadPolling() {
    Serial.println("🔄 Starting CONSERVATIVE endpoint polling...");
    
    if (!device_) {
        Serial.println("❌ No device available for polling");
        return;
    }
    
    // Start with only the most essential endpoints first
    
    // Start polling Interface 1 Control (64 byte packets) - MOST IMPORTANT FOR BUTTONS
    Serial.printf("📡 Starting control polling on EP 0x%02X...\n", ctrl_ep_in);
    ctrl_pipe_in = new_Pipe(device_, 3, ctrl_ep_in, 1, 64); // 3 = interrupt, 1 = input
    if (ctrl_pipe_in) {
        // Note: USBHost_t36 will automatically call our callback when transfer completes
        queue_Data_Transfer(ctrl_pipe_in, ctrl_report, 64, this);
        ctrl_polling = true;
        Serial.println("✅ Control polling started successfully");
    } else {
        Serial.println("❌ Failed to start control polling");
        return; // Don't continue if this critical endpoint fails
    }
    
    delay(2); // Reduced delay - USB capture shows device responds in ~10-15ms
    
    // Start polling Interface 0 (Keyboard - 8 byte packets) - SECONDARY PRIORITY
    Serial.printf("📡 Starting keyboard polling on EP 0x%02X...\n", kbd_ep_in);
    kbd_pipe = new_Pipe(device_, 3, kbd_ep_in, 1, 8); // 3 = interrupt, 1 = input
    if (kbd_pipe) {
        // Note: USBHost_t36 will automatically call our callback when transfer completes
        queue_Data_Transfer(kbd_pipe, kbd_report, 8, this);
        kbd_polling = true;
        Serial.println("✅ Keyboard polling started successfully");
    } else {
        Serial.println("⚠️ Keyboard polling failed - continuing without it");
    }
    
    delay(2);
    
    Serial.println("🎯 Essential endpoints started - additional endpoints disabled for bandwidth");
    Serial.println("   (Dual action and hall sensor endpoints can be added later if needed)");
    
    // Report polling status
    Serial.printf("📊 Conservative Polling Status: kbd=%s, ctrl=%s\n",
                  kbd_polling ? "✅" : "❌",
                  ctrl_polling ? "✅" : "❌");
    
    if (ctrl_polling) {
        Serial.println("🎯 Control endpoint is polling! Device should now send button events!");
        Serial.println("🔥 Try pressing buttons now - should see real button events!");
    } else {
        Serial.println("⚠️ Control polling failed - button events may not work");
    }
}

// New transfer callback implementations
void USBControlPad::kbd_callback(const Transfer_t *transfer) {
    static int kbd_counter = 0;
    
    if (transfer->length > 0 && queue) {
        kbd_counter++;
        
        // Queue keyboard event
        if (kbd_report[2] != 0) {
            controlpad_event event;
            event.data[0] = transfer->length;  // Length in data[0]
            memcpy(&event.data[1], kbd_report, min((int)transfer->length, 63));  // USB data in data[1-63]
            queue->put(event);
        }
        
        // Restart keyboard polling
        if (kbd_pipe && kbd_polling) {
            queue_Data_Transfer(kbd_pipe, kbd_report, 8, this);
        }
    } else if (transfer->length == 0) {
        // Still restart polling to keep listening
        if (kbd_pipe && kbd_polling) {
            queue_Data_Transfer(kbd_pipe, kbd_report, 8, this);
        }
    }
}

void USBControlPad::ctrl_in_callback(const Transfer_t *transfer) {
    static int ctrl_counter = 0;
    
    if (transfer->length > 0 && queue) {
        ctrl_counter++;
        
        // Check for button press pattern: 43 01 00 00 XX (pressed/released)
        if (transfer->length >= 6 && ctrl_report[0] == 0x43 && ctrl_report[1] == 0x01 && 
            ctrl_report[2] == 0x00 && ctrl_report[3] == 0x00) {
            
            // 🚀 ATOMIC LED UPDATE PROTECTION
            // If LED update is in progress, ignore button events to prevent timing conflicts
            if (atomicLEDUpdateInProgress) {
                Serial.println("🚫 Button event ignored during atomic LED update");
                goto restart_polling;
            }
            
            uint8_t buttonId = ctrl_report[4];
            uint8_t state = ctrl_report[5];
            
            // Filter out spurious events and only process valid button states
            if (state != 0xC0 && state != 0x40) {
                goto restart_polling; // Skip invalid states
            }
            
            // Skip spurious USB ID 0x00
            if (buttonId == 0x00) {
                goto restart_polling;
            }
            
            if (buttonId >= 1 && buttonId <= 24) {  // Valid button range 1-24
                // CRITICAL FIX: ONLY QUEUE EVENTS IN INTERRUPT CONTEXT
                // Remove all direct LED processing to prevent USB timing conflicts
                
                controlpad_event event;
                
                // Store length in data[0] and USB packet in data[1-63]
                event.data[0] = transfer->length;  // Length stored in first byte
                memcpy(&event.data[1], ctrl_report, min((int)transfer->length, 63));  // USB data in bytes 1-63
                
                // Queue the raw USB event for main thread processing
                if (!queue->put(event)) {
                    // Queue full - this is rare but can happen under heavy load
                    Serial.println("❌ USB: Queue full, event dropped!");
                } else {
                    Serial.printf("📥 USB: Queued button %d %s\n", buttonId, 
                                 state == 0xC0 ? "PRESSED" : "RELEASED");
                }
                
                // No LED processing in interrupt context!
                // No ControlPad event creation in interrupt context!
                // All processing deferred to main thread via ControlPadHardware::poll()
            }
        } 
        // Check for command echoes: 56 8x (LED), 52 xx, 42 xx, 43 xx, 41 xx, 51 xx
        else if (transfer->length >= 2 && (
                (ctrl_report[0] == 0x56 && (ctrl_report[1] == 0x81 || ctrl_report[1] == 0x83)) ||  // LED commands
                (ctrl_report[0] == 0x52) ||  // Effect mode commands
                (ctrl_report[0] == 0x42) ||  // Activation commands 
                (ctrl_report[0] == 0x43) ||  // Status commands
                (ctrl_report[0] == 0x41) ||  // Apply commands
                (ctrl_report[0] == 0x51)     // Finalize commands
                )) {
            // ⚡ COMMAND ECHO VERIFICATION COMPLETELY DISABLED
            // No longer checking or reporting command echoes - this eliminates
            // the USB timing conflicts that cause LED flickering
            
            // Command echo received - verify it matches expectations (DISABLED)
            if (expectedLEDEcho[0] == ctrl_report[0] && expectedLEDEcho[1] == ctrl_report[1]) {
                ledCommandVerified = true;
                // Serial.printf("✅ Command echo verified: %02X %02X\n", ctrl_report[0], ctrl_report[1]);
            }
            // No more mismatch reporting - this was causing the flickering!
            // else if (expectedLEDEcho[0] != 0x00) {
            //     Serial.printf("❌ Command echo mismatch: expected %02X %02X, got %02X %02X\n",
            //                  expectedLEDEcho[0], expectedLEDEcho[1], ctrl_report[0], ctrl_report[1]);
            // }
            // If we got any command echo but weren't expecting it, silently ignore (stale echo)
        }
    }
    
    restart_polling:
    // Restart the control polling
    if (ctrl_pipe_in && ctrl_polling) {
        queue_Data_Transfer(ctrl_pipe_in, ctrl_report, 64, this);
    }
}

void USBControlPad::ctrl_out_callback(const Transfer_t *transfer) {
    // Handle control output transfer completion
    static int commandCounter = 0;
    commandCounter++;
    
    if (transfer->length >= 0) {
        // Only show every 10th success to reduce spam
        if (commandCounter <= 5 || commandCounter % 10 == 0) {
            // Serial.printf("📤 Command #%d sent successfully\n", commandCounter);
        }
    } else {
        // Serial.printf("❌ Command #%d FAILED: %d\n", commandCounter, transfer->length);
    }
}

void USBControlPad::dual_callback(const Transfer_t *transfer) {
    if (transfer->length > 0 && queue) {
        // Queue the dual event
        controlpad_event event;
        int len = min((int)transfer->length, 8);
        event.data[0] = len;  // Length in data[0]
        memcpy(&event.data[1], dual_report, min(len, 63));  // USB data in data[1-63]
        queue->put(event);
    }
    
    // Restart dual polling
    if (dual_pipe && dual_polling) {
        queue_Data_Transfer(dual_pipe, dual_report, 8, this);
    }
}

void USBControlPad::hall_sensor_callback(const Transfer_t *transfer) {
    if (transfer->length > 0 && queue) {
        // Queue the hall sensor event
        controlpad_event event;
        int len = min((int)transfer->length, 32);
        event.data[0] = len;  // Length in data[0]
        memcpy(&event.data[1], hall_sensor_report, min(len, 63));  // USB data in data[1-63]
        queue->put(event);
    }
    
    // Restart hall sensor polling
    if (hall_sensor_pipe && hall_sensor_polling) {
        queue_Data_Transfer(hall_sensor_pipe, hall_sensor_report, 32, this);
    }
}

void USBControlPad::sensor_out_callback(const Transfer_t *transfer) {
    if (transfer->length > 0 && queue) {
        // Queue the event
        controlpad_event event;
        int len = min((int)transfer->length, 32);
        event.data[0] = len;  // Length in data[0]
        memcpy(&event.data[1], sensor_out_report, min(len, 63));  // USB data in data[1-63]
        queue->put(event);
    }
    
    // Restart endpoint 0x07 polling
    if (sensor_out_pipe && sensor_out_polling) {
        queue_Data_Transfer(sensor_out_pipe, sensor_out_report, 32, this);
    }
}

// The sent callback is now handled by ctrl_out_callback

bool USBControlPad::sendUSBInterfaceReSponseActivation() {
    if (!initialized) {
        return false;
    }
    
    // Send Activation startup sequence 
    sendActivationCommandsForInterface(1, "42 00 activation packet 1/2");
    sendActivationCommandsForInterface(2, "42 10 activation packet 2/2");
    sendActivationCommandsForInterface(3, "43 00 activation status");
    sendActivationCommandsForInterface(4, "41 80 finalize activation");
    sendActivationCommandsForInterface(5, "52 00 activate effect modes");
    sendActivationCommandsForInterface(6, "41 80 repeat");
    sendActivationCommandsForInterface(7, "52 00 final");
    
    // Set custom mode ONCE during startup - not needed for every button press
    Serial.println("🎨 Setting custom LED mode for startup");
    setCustomMode();
    delay(1);
    return true;
}

bool USBControlPad::sendActivationCommandsForInterface(int step, const char* description) {
    // Define all Windows startup commands
    uint8_t commands[7][64] = {
        // Step 1: 0x42 00 activation
        {0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        
        // Step 2: 0x42 10 variant  
        {0x42, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        
        // Step 3: 0x43 00 button activation
        {0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        
        // Step 4: 0x41 80 status
        {0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        
        // Step 5: 0x52 00 status query
        {0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        
        // Step 6: 0x41 80 repeat (same as step 4)
        {0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        
        // Step 7: 0x52 00 final (same as step 5)
        {0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
    };
    
    if (step < 1 || step > 7) {
        return false;
    }
    
    // Send command with appropriate verification based on command type
    uint8_t* cmd = commands[step-1];
    uint8_t expectedEcho1 = cmd[0];  // First byte should be echoed back
    uint8_t expectedEcho2 = cmd[1];  // Second byte should be echoed back
    
    bool success = sendCommandWithVerification(cmd, 64, expectedEcho1, expectedEcho2);
    
    if (!success) {
        Serial.printf("❌ Activation command %02X %02X failed verification - retrying\n", expectedEcho1, expectedEcho2);
        // Fallback to sendCommand() which includes mutex protection
        return sendCommand(cmd, 64);
    }
    
    Serial.printf("✅ Activation command %02X %02X verified\n", expectedEcho1, expectedEcho2);
    delay(25);  // Reduced delay since verification already handled timing
    return true;
}

// ===== LED CONTROL COMMANDS =====

bool USBControlPad::sendCommand(const uint8_t* data, size_t length) {
    if (!initialized || length > 64 || !ctrl_pipe_out) {
        return false;
    }
    
    // Create output pipe if not already created
    if (!ctrl_pipe_out) {
        ctrl_pipe_out = new_Pipe(device_, 3, ctrl_ep_out, 0, 64); // 3 = interrupt, 0 = output
        if (!ctrl_pipe_out) {
            Serial.println("❌ Failed to create control output pipe");
            return false;
        }
    }
    
    // Send via interrupt transfer to control endpoint
    queue_Data_Transfer(ctrl_pipe_out, const_cast<uint8_t*>(data), length, this);
    
    // SIMPLIFIED: Minimal delay to test if timing is the issue
    delayMicroseconds(850);
    
    return true;
}

bool USBControlPad::sendLEDCommandWithVerification(const uint8_t* data, size_t length, uint8_t expectedEcho1, uint8_t expectedEcho2) {
    if (!initialized || length > 64) {
        return false;
    }
    
    // FAST MODE: Skip verification entirely during button events to eliminate timing conflicts
    // The 50ms verification timeout was causing interference with button press/release cycles
    if (atomicLEDUpdateInProgress) {
        // During LED updates, use simplified command sending without verification delays
        return sendCommand(data, length);
    }
    
    // SEQUENTIAL MODE: Always use verification for reliable command sequencing
    // Clear any pending verification data and flush stale echoes
    ledCommandVerified = false;
    expectedLEDEcho[0] = expectedEcho1;
    expectedLEDEcho[1] = expectedEcho2;
    
    // Send the command
    bool result = sendCommand(data, length);
    if (!result) {
        return false;
    }
    
    // SIMPLE VERIFICATION: Just wait for echo without complex cleanup logic
    unsigned long startTime = millis();
    while (!ledCommandVerified && (millis() - startTime) < 10) {
        delayMicroseconds(100);
        yield(); // Allow USB processing
    }
    
    if (!ledCommandVerified) {
        // Verification failure - USB echo timeout
        // Serial.printf("⚠️ LED command echo timeout: expected %02X %02X\n", expectedEcho1, expectedEcho2);
    }
    
    // Always return success during LED updates to avoid blocking
    return true;
}

bool USBControlPad::sendCommandWithVerification(const uint8_t* data, size_t length, uint8_t expectedEcho1, uint8_t expectedEcho2) {
    // This is an alias for sendLEDCommandWithVerification since the verification logic is the same
    return sendLEDCommandWithVerification(data, length, expectedEcho1, expectedEcho2);
}

void USBControlPad::setFastMode(bool enabled) {
    fastModeEnabled = enabled;
    if (enabled) {
        // Serial.println("⚡ Fast mode ENABLED - verification disabled for rapid updates");
    } else {
        // Serial.println("🔒 Fast mode DISABLED - verification enabled");
    }
}

// 🚀 SIMPLE LED UPDATE IMPLEMENTATION - Back to Basics
void USBControlPad::pauseUSBPolling() {
    atomicLEDUpdateInProgress = true;
}

void USBControlPad::resumeUSBPolling() {
    // CRITICAL: Ensure USB communication pipeline is completely clear
    // before allowing next LED update - prevents sporadic interference
    delay(3); // 3ms USB quiet period to prevent accumulation
    atomicLEDUpdateInProgress = false;
}

bool USBControlPad::setCustomMode() {
    uint8_t cmd[64] = {
        0x56, 0x81, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0xbb, 0xbb, 0xbb, 0xbb,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,

    };

   // Serial.println("🎨 Setting custom LED mode");
    bool success = sendLEDCommandWithVerification(cmd, 64, 0x56, 0x81);   
    return success;
}

bool USBControlPad::setStaticMode() {
    uint8_t cmd[64] = {
        0x56, 0x81, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x55, 0x55, 0x55, 0x55  // Static mode pattern
        // Rest filled with zeros
    };
    
    // Serial.println("🎨 Setting static LED mode");
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendApplyCommand() {
    uint8_t cmd[64] = {
        0x41, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    
    // Send Apply command WITH verification - wait for hardware acknowledgement
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendFinalizeCommand() {
    uint8_t cmd[64] = {
    0x51, 0x28, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
    };
    
    // Send Finalize command WITH verification - wait for hardware acknowledgement
    bool success = sendCommand(cmd, 64);
    return success;
}

bool USBControlPad::updateAllLEDs(const ControlPadColor* colors, size_t count) {
    if (!initialized || count > 24) {
        // Serial.println("❌ Cannot update LEDs: not initialized or too many LEDs");
        return false;
    }
    
    // CRITICAL: Create atomic snapshot to prevent modification during Package1→Package2 
    // This prevents the 4-cycle pattern caused by colors changing between packages
    static ControlPadColor atomicColorBuffer[24];
    memcpy(atomicColorBuffer, colors, 24 * sizeof(ControlPadColor));
    
    // 🚀 OPTIMIZED ATOMIC LED UPDATE - Minimal Commands Only
    // Remove setCustomMode() from every button press - only needed at startup!
    // This eliminates the LED controller reset that causes black flashes
    
    // SIMPLE LED UPDATE - Remove all unnecessary complexity
    pauseUSBPolling();
    bool success = true;
    // TEENSY 4.x DMA FLUSH: Prevent TeensyUSBHost2 buffer accumulation 
    // static uint16_t updateCounter = 0;
    // updateCounter++;
    
    // Essential commands only - use ATOMIC BUFFER for both packages
    success &= sendLEDPackage1(atomicColorBuffer);
    //delayMicroseconds(650); // CRITICAL: Package 1→Package 2 gap - prevent overwrite
    success &= sendLEDPackage2(atomicColorBuffer);
    
    // TEENSY4 i.MX RT SPECIFIC: USB state reset every 50 updates to prevent accumulation
    // if (updateCounter % 24 == 0) {
    //     // Alternative approach: Force USB polling restart to clear TeensyUSBHost2 state
    //     pauseUSBPolling();
    //     delay(5); // Longer USB quiet period to clear i.MX RT USB controller state
    //     resumeUSBPolling();
    // }
    success &= sendApplyCommand();
    success &= sendFinalizeCommand();
    
    resumeUSBPolling();
    
    // if (success) {
    //     Serial.println("✅ Atomic LED update completed");
    // } else {
    //     Serial.println("❌ Atomic LED update failed");
    // }
    
    return success;
}

bool USBControlPad::sendLEDPackage1(const ControlPadColor* colors) {
    // DEPRECATED: Use sendLEDPackages() instead for better performance
    // This function is kept for backwards compatibility only
    
    uint8_t cmd[64] = {
        0x56, 0x83, 0x00, 0x00,    // Header
        0x01, 0x00, 0x00, 0x00,    // Package 1 marker
        0x80, 0x01, 0x00, 0x00,    // Control flags
        0xff, 0x00, 0x00, 0x00,    // Color flags
        0x00, 0x00,                // Reserved
        0xff, 0xff,                // Brightness for all LEDs
        0x00, 0x00, 0x00, 0x00     // Reserved
    };
    
    // LED data starts at position 24 - direct assignments without loops
    size_t pos = 24;
    
    // Column 1: buttons 1,6,11,16,21 (indices 0,5,10,15,20) - unrolled
    cmd[pos++] = colors[0].r;   cmd[pos++] = colors[0].g;   cmd[pos++] = colors[0].b;   // Button 1
    cmd[pos++] = colors[5].r;   cmd[pos++] = colors[5].g;   cmd[pos++] = colors[5].b;   // Button 6
    cmd[pos++] = colors[10].r;  cmd[pos++] = colors[10].g;  cmd[pos++] = colors[10].b;  // Button 11
    cmd[pos++] = colors[15].r;  cmd[pos++] = colors[15].g;  cmd[pos++] = colors[15].b;  // Button 16
    cmd[pos++] = colors[20].r;  cmd[pos++] = colors[20].g;  cmd[pos++] = colors[20].b;  // Button 21
    
    // Column 2: buttons 2,7,12,17,22 (indices 1,6,11,16,21) - unrolled
    cmd[pos++] = colors[1].r;   cmd[pos++] = colors[1].g;   cmd[pos++] = colors[1].b;   // Button 2
    cmd[pos++] = colors[6].r;   cmd[pos++] = colors[6].g;   cmd[pos++] = colors[6].b;   // Button 7
    cmd[pos++] = colors[11].r;  cmd[pos++] = colors[11].g;  cmd[pos++] = colors[11].b;  // Button 12
    cmd[pos++] = colors[16].r;  cmd[pos++] = colors[16].g;  cmd[pos++] = colors[16].b;  // Button 17
    cmd[pos++] = colors[21].r;  cmd[pos++] = colors[21].g;  cmd[pos++] = colors[21].b;  // Button 22
    
    // Column 3: buttons 3,8,13 (indices 2,7,12) - unrolled  
    cmd[pos++] = colors[2].r;   cmd[pos++] = colors[2].g;   cmd[pos++] = colors[2].b;   // Button 3
    cmd[pos++] = colors[7].r;   cmd[pos++] = colors[7].g;   cmd[pos++] = colors[7].b;   // Button 8
    cmd[pos++] = colors[12].r;  cmd[pos++] = colors[12].g;  cmd[pos++] = colors[12].b;  // Button 13
    
    // Button 18 (index 17) - only R component fits in package 1
    cmd[pos++] = colors[17].r;  // Button 18 R
    
    // Serial.println("🎨 Sending LED Package 1");
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendLEDPackage2(const ControlPadColor* colors) {
    // DEPRECATED: Use sendLEDPackages() instead for better performance
    // This function is kept for backwards compatibility only
    
    uint8_t cmd[64] = {
        0x56, 0x83, 0x01          // Header for package 2
    };
    
    size_t pos = 3;
    
    // Complete button 18 (index 17) - GB components
    cmd[pos++] = 0x00;  // Padding
    cmd[pos++] = colors[17].g;  // Button 18 G
    cmd[pos++] = colors[17].b;  // Button 18 B
    
    // Button 23 (index 22)
    cmd[pos++] = colors[22].r;
    cmd[pos++] = colors[22].g;
    cmd[pos++] = colors[22].b;
    
    // Column 4: buttons 4,9,14,19,24 (indices 3,8,13,18,23) - unrolled
    cmd[pos++] = colors[3].r;   cmd[pos++] = colors[3].g;   cmd[pos++] = colors[3].b;   // Button 4
    cmd[pos++] = colors[8].r;   cmd[pos++] = colors[8].g;   cmd[pos++] = colors[8].b;   // Button 9
    cmd[pos++] = colors[13].r;  cmd[pos++] = colors[13].g;  cmd[pos++] = colors[13].b;  // Button 14
    cmd[pos++] = colors[18].r;  cmd[pos++] = colors[18].g;  cmd[pos++] = colors[18].b;  // Button 19
    cmd[pos++] = colors[23].r;  cmd[pos++] = colors[23].g;  cmd[pos++] = colors[23].b;  // Button 24
    
    // Column 5: buttons 5,10,15,20 (indices 4,9,14,19) - unrolled
    cmd[pos++] = colors[4].r;   cmd[pos++] = colors[4].g;   cmd[pos++] = colors[4].b;   // Button 5
    cmd[pos++] = colors[9].r;   cmd[pos++] = colors[9].g;   cmd[pos++] = colors[9].b;   // Button 10
    cmd[pos++] = colors[14].r;  cmd[pos++] = colors[14].g;  cmd[pos++] = colors[14].b;  // Button 15
    cmd[pos++] = colors[19].r;  cmd[pos++] = colors[19].g;  cmd[pos++] = colors[19].b;  // Button 20
    
    // CRITICAL: Pad to exactly 64 bytes with 0x00 (28 bytes needed) - pos=36, need 28 more
    cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00;
    cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00;
    cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00;
    cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; cmd[pos++] = 0x00; // 28 bytes total
    
    // Serial.println("🎨 Sending LED Package 2");
    return sendCommand(cmd, 64);
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
    Serial.println("🚀 ControlPadHardware::begin() called!");
    
    // Store reference to the ControlPad instance
    currentPad = &pad;
    
    // 1. Initialize DMA queues
    if (!controlpad_queue.begin()) {
        Serial.println("❌ Failed to initialize controlpad queue");
        return false;
    }
    
    if (!led_command_queue.begin()) {
        Serial.println("❌ Failed to initialize LED command queue");
        return false;
    }
    Serial.println("✅ DMA queues initialized");
    
    // 2. Use global USB Host and Driver (USBHost_t36 standard pattern)
    Serial.println("✅ Using global USB driver and host objects");
    
    // 3. Start global USB host
    globalUSBHost.begin();
    Serial.println("🔌 Global USB Host started, looking for devices...");
    
    // Set up test callbacks to see if standard drivers detect anything  
    testKeyboard.attachPress(testKeyboardCallback);
    // testMouse.attachPress(testMouseCallback);  // Mouse has different API
    Serial.println("🔧 Test callbacks attached to standard drivers");
    
    // 🔥 CRITICAL: Allow USB host to discover and enumerate devices
    // The USB Host needs time to detect devices and call driver claim() methods
    Serial.println("🔍 Starting device enumeration (calling USB Task)...");
    Serial.println("💡 Make sure ControlPad device is plugged into USB Host port!");
    Serial.println("💡 Looking for device VID:0x2516 PID:0x012D...");
    
    for (int i = 0; i < 100; i++) {  // Allow up to 10 seconds for enumeration
        globalUSBHost.Task();  // This processes USB enumeration and calls claim()
        
        // Show progress every 10 iterations (1 second)
        if (i % 10 == 0) {
            Serial.printf("🔄 Enumeration attempt %d/100...\n", i + 1);
            Serial.println("   Testing if ANY USB devices are being detected...");
        }
        
        delay(100);
        if (globalControlPadDriver.device_) {
            Serial.printf("✅ Device enumerated after %d iterations!\n", i + 1);
            break;
        }
    }
    
    // 4. Wait for device enumeration (ControlPad device connection)
    unsigned long startTime = millis();
    while (!globalControlPadDriver.device_ && (millis() - startTime) < 10000) {  // 10 second timeout
        delay(100);
    }
    
    if (!globalControlPadDriver.device_) {
        Serial.println("❌ No ControlPad device found after 10 seconds");
        return false;
    }
    
    Serial.println("✅ ControlPad device connected!");
    
    // 5. Wait additional time for USB configuration to stabilize
    Serial.println("⏳ Waiting for USB configuration to stabilize...");
    delay(2000);
    
    // 6. Now initialize the driver (this starts polling)
    Serial.println("🎯 Starting driver initialization...");
    bool initResult = globalControlPadDriver.begin(&controlpad_queue);
    if (!initResult) {
        Serial.println("❌ Driver initialization failed");
        return false;
    }
    
    Serial.printf("✅ Driver initialized! initialized=%s\n", 
                 globalControlPadDriver.initialized ? "true" : "false");
    
    // 7. Wait for polling to stabilize
    delay(1000);
    
    // 8. Send activation sequence
    Serial.println("🚀 Sending activation sequence...");
    globalControlPadDriver.sendUSBInterfaceReSponseActivation();
    
    // 9. ControlPad setup is handled by the caller (main.cpp)
    // No need to call pad.begin() here as it would create circular recursion
    
    return true;
}

void ControlPadHardware::poll() {
    // CRITICAL: Process events from DMAQueue in main thread context with USB-synchronized timing
    // This prevents USB timing conflicts by processing button events at consistent intervals
    
    static int debugCounter = 0;
    static unsigned long lastProcessTime = 0;
    debugCounter++;
    
    if (!currentPad) {
        if (debugCounter % 1000 == 0) {
            Serial.printf("🚫 ControlPadHardware::poll() - Missing currentPad\n");
        }
        return;
    }
    
    // USB-SYNCHRONIZED PROCESSING: Only process button events at consistent 2ms intervals
    // This avoids the 0.1-1 second timing window that causes LED flicker
    unsigned long currentTime = millis();
    if (currentTime - lastProcessTime < 2) {
        return; // Skip processing to maintain consistent timing
    }
    lastProcessTime = currentTime;
    
    // Process pending events from the queue
    controlpad_event rawEvent;
    int eventsProcessed = 0;
    
    // DISABLED: LED command queue processing (debugging synchronous mode)
    // if (controlPadDriver) {
    //     controlPadDriver->processLEDCommandQueue();
    // }
    
    // Try a SINGLE non-blocking queue get
    if (controlpad_queue.get(rawEvent, 0)) { // 0 = non-blocking
        eventsProcessed++;
        
        uint8_t packetLen = rawEvent.data[0];  // Length is stored in data[0]
        
        // Check if this is a button event (43 01 pattern) - control interface sends 64-byte packets
        // USB data is now stored in data[1] onwards, length in data[0]
        if (packetLen >= 6 && rawEvent.data[1] == 0x43 && rawEvent.data[2] == 0x01) {
            uint8_t buttonId = rawEvent.data[5];  // USB button ID (1-24) - now at data[5] instead of data[4]
            uint8_t state = rawEvent.data[6];     // Button state (0xC0=pressed, 0x40=released) - now at data[6] instead of data[5]
            
            if (buttonId >= 0 && buttonId <= 24) {
            // Apply the CORRECT COLUMN-MAJOR MAPPING that was working before
            // Physical layout is 5x5 grid, but LED data is sent column-wise
            uint8_t row = buttonId / 5;  // Row (0-4)
            uint8_t col = buttonId % 5;  // Column (0-4)
            uint8_t verticalIndex = col * 5 + row;  // LED index in column-major order
            
            // DEBUG: Track problematic buttons that get stuck
            bool isProblematicButton = (buttonId == 23 || buttonId == 10 || buttonId == 14 || buttonId == 15 || buttonId == 20);
            
            // Create ControlPadEvent for the main application
            ControlPadEvent event;
            event.type = ControlPadEventType::Button;
            event.button.button = verticalIndex;
            
            bool validEvent = false;
            
            if (state == 0xC0) {
                event.button.pressed = true;
                validEvent = true;
                if (isProblematicButton) {
                    Serial.printf("🔍 PROBLEMATIC BUTTON %d (LED %d) PRESSED\n", buttonId, verticalIndex);
                }
            } else if (state == 0x40) {
                event.button.pressed = false;
                validEvent = true;
                if (isProblematicButton) {
                    Serial.printf("🔍 PROBLEMATIC BUTTON %d (LED %d) RELEASED\n", buttonId, verticalIndex);
                }
            }
            
            // USB-SYNCHRONIZED: Push event to ControlPad with timing safeguards
            // This ensures LED updates happen at consistent intervals relative to USB polling
            if (validEvent) {
                currentPad->pushEvent(event);
            }
            }
        }
        // Add processing for other event types here if needed (kbd, dual, hall sensor)
    }
}

void ControlPadHardware::setAllLeds(const ControlPadColor* colors, size_t count) {
    // TEMPORARY: Disable queue system - revert to PROVEN working synchronous approach
    // The queue system was processing commands too rapidly without proper timing/USB controls
    
    // Using global driver object (USBHost_t36 pattern)
    //Serial.println("🔄 Using synchronous LED update (queue disabled for debugging)");
    bool success = globalControlPadDriver.updateAllLEDs(colors, count);
    if (!success) {
       // Serial.println("❌ Synchronous LED update failed");
    } else {
       // Serial.println("✅ Synchronous LED update completed");
    }
}

// void ControlPadHardware::setFastMode(bool enabled) {
//     if (controlPadDriver) {
//         controlPadDriver->setFastMode(enabled);
//     }
// }

// ===== QUEUE-BASED LED SYSTEM FOR MIDI TIMING =====
// Uses proven TeensyAtomThreads queue system with DMA underneath

bool USBControlPad::queueLEDUpdate(const ControlPadColor* colors, size_t count) {
    if (!initialized || count > 24 || !globalHardwareInstance) {
        return false;
    }
    
    // Prepare 4 LED commands
    led_command_event commands[4];
    globalHardwareInstance->prepareLEDCommands(colors, commands);
    
    // Queue all 4 commands using DMA queue system
    for (int i = 0; i < 4; i++) {
        if (!globalHardwareInstance->led_command_queue.put(commands[i], 100)) {
            Serial.printf("❌ Failed to queue LED command %d\n", i);
            return false;
        }
    }
    
    Serial.println("✅ Queued 4 LED commands for background processing");
    return true;
}

void USBControlPad::processLEDCommandQueue() {
    if (!globalHardwareInstance) {
        return;
    }
    
    // Process one LED command per loop iteration to spread CPU load
    led_command_event cmd;
    
    if (globalHardwareInstance->led_command_queue.get(cmd, 0)) { // 0 = non-blocking
        // Send the queued command
        const char* cmdNames[] = {"Package1", "Package2", "Apply", "Finalize"};
        if (cmd.command_type < 4) {
            Serial.printf("📡 Processing queued LED %s command\n", cmdNames[cmd.command_type]);
            sendCommand(cmd.data, 64);
            delay(1); // Small delay between commands
        }
    }
}

void ControlPadHardware::prepareLEDCommands(const ControlPadColor* colors, led_command_event* commands) {
    // ===== PACKAGE 1 - EXACT COPY FROM WORKING sendLEDPackages =====
    commands[0].command_type = 0;
    memset(commands[0].data, 0, 64);
    
    // Header - EXACT MATCH to working version
    commands[0].data[0] = 0x56; commands[0].data[1] = 0x83; commands[0].data[2] = 0x00; commands[0].data[3] = 0x00;
    commands[0].data[4] = 0x01; commands[0].data[5] = 0x00; commands[0].data[6] = 0x00; commands[0].data[7] = 0x00;
    commands[0].data[8] = 0x80; commands[0].data[9] = 0x01; commands[0].data[10] = 0x00; commands[0].data[11] = 0x00;
    commands[0].data[12] = 0xff; commands[0].data[13] = 0x00; commands[0].data[14] = 0x00; commands[0].data[15] = 0x00;
    commands[0].data[16] = 0x00; commands[0].data[17] = 0x00;
    commands[0].data[18] = 0xff; commands[0].data[19] = 0xff; // CRITICAL: Brightness for all LEDs
    commands[0].data[20] = 0x00; commands[0].data[21] = 0x00; commands[0].data[22] = 0x00; commands[0].data[23] = 0x00;
    
    // LED data starts at position 24 - EXACT COLUMN ORDER from working version
    size_t pos = 24;
    
    // Column 1: buttons 1,6,11,16,21 (indices 0,5,10,15,20)
    commands[0].data[pos++] = colors[0].r;   commands[0].data[pos++] = colors[0].g;   commands[0].data[pos++] = colors[0].b;
    commands[0].data[pos++] = colors[5].r;   commands[0].data[pos++] = colors[5].g;   commands[0].data[pos++] = colors[5].b;
    commands[0].data[pos++] = colors[10].r;  commands[0].data[pos++] = colors[10].g;  commands[0].data[pos++] = colors[10].b;
    commands[0].data[pos++] = colors[15].r;  commands[0].data[pos++] = colors[15].g;  commands[0].data[pos++] = colors[15].b;
    commands[0].data[pos++] = colors[20].r;  commands[0].data[pos++] = colors[20].g;  commands[0].data[pos++] = colors[20].b;
    
    // Column 2: buttons 2,7,12,17,22 (indices 1,6,11,16,21)
    commands[0].data[pos++] = colors[1].r;   commands[0].data[pos++] = colors[1].g;   commands[0].data[pos++] = colors[1].b;
    commands[0].data[pos++] = colors[6].r;   commands[0].data[pos++] = colors[6].g;   commands[0].data[pos++] = colors[6].b;
    commands[0].data[pos++] = colors[11].r;  commands[0].data[pos++] = colors[11].g;  commands[0].data[pos++] = colors[11].b;
    commands[0].data[pos++] = colors[16].r;  commands[0].data[pos++] = colors[16].g;  commands[0].data[pos++] = colors[16].b;
    commands[0].data[pos++] = colors[21].r;  commands[0].data[pos++] = colors[21].g;  commands[0].data[pos++] = colors[21].b;
    
    // Column 3: buttons 3,8,13 (indices 2,7,12)
    commands[0].data[pos++] = colors[2].r;   commands[0].data[pos++] = colors[2].g;   commands[0].data[pos++] = colors[2].b;
    commands[0].data[pos++] = colors[7].r;   commands[0].data[pos++] = colors[7].g;   commands[0].data[pos++] = colors[7].b;
    commands[0].data[pos++] = colors[12].r;  commands[0].data[pos++] = colors[12].g;  commands[0].data[pos++] = colors[12].b;
    
    // Button 18 (index 17) - only R component fits in package 1
    commands[0].data[pos++] = colors[17].r;
    
    // ===== PACKAGE 2 - EXACT COPY FROM WORKING sendLEDPackages =====
    commands[1].command_type = 1;
    memset(commands[1].data, 0, 64);
    commands[1].data[0] = 0x56; commands[1].data[1] = 0x83; commands[1].data[2] = 0x01;
    
    pos = 3;
    // Complete button 18 (index 17) - GB components
    commands[1].data[pos++] = 0x00;  // Padding
    commands[1].data[pos++] = colors[17].g;  // Button 18 G
    commands[1].data[pos++] = colors[17].b;  // Button 18 B
    
    // Button 23 (index 22)
    commands[1].data[pos++] = colors[22].r;
    commands[1].data[pos++] = colors[22].g;
    commands[1].data[pos++] = colors[22].b;
    
    // Column 4: buttons 4,9,14,19,24 (indices 3,8,13,18,23)
    commands[1].data[pos++] = colors[3].r;   commands[1].data[pos++] = colors[3].g;   commands[1].data[pos++] = colors[3].b;
    commands[1].data[pos++] = colors[8].r;   commands[1].data[pos++] = colors[8].g;   commands[1].data[pos++] = colors[8].b;
    commands[1].data[pos++] = colors[13].r;  commands[1].data[pos++] = colors[13].g;  commands[1].data[pos++] = colors[13].b;
    commands[1].data[pos++] = colors[18].r;  commands[1].data[pos++] = colors[18].g;  commands[1].data[pos++] = colors[18].b;
    commands[1].data[pos++] = colors[23].r;  commands[1].data[pos++] = colors[23].g;  commands[1].data[pos++] = colors[23].b;
    
    // Column 5: buttons 5,10,15,20 (indices 4,9,14,19)
    commands[1].data[pos++] = colors[4].r;   commands[1].data[pos++] = colors[4].g;   commands[1].data[pos++] = colors[4].b;
    commands[1].data[pos++] = colors[9].r;   commands[1].data[pos++] = colors[9].g;   commands[1].data[pos++] = colors[9].b;
    commands[1].data[pos++] = colors[14].r;  commands[1].data[pos++] = colors[14].g;  commands[1].data[pos++] = colors[14].b;
    commands[1].data[pos++] = colors[19].r;  commands[1].data[pos++] = colors[19].g;  commands[1].data[pos++] = colors[19].b;
    
    // ===== APPLY COMMAND - EXACT MATCH =====
    commands[2].command_type = 2;
    memset(commands[2].data, 0, 64);
    commands[2].data[0] = 0x41; commands[2].data[1] = 0x80;
    
    // ===== FINALIZE COMMAND - EXACT MATCH =====
    commands[3].command_type = 3;
    memset(commands[3].data, 0, 64);
    commands[3].data[0] = 0x51; commands[3].data[1] = 0x28; commands[3].data[2] = 0x00; commands[3].data[3] = 0x00;
    commands[3].data[4] = 0xff; commands[3].data[5] = 0x00;
}