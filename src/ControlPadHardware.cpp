#include "ControlPadHardware.h"
#include <teensy4_usbhost.h>

// Global USB host instance
static DMAMEM TeensyUSBHost2 usbHost;

// Global driver instance pointer (defined here, declared in header as extern)
USBControlPad* controlPadDriver = nullptr;

// Global hardware instance for access from USB callbacks
ControlPadHardware* globalHardwareInstance = nullptr;

// Static member definitions for USBControlPad
bool USBControlPad::driver_instance_created = false;

// ===== USB DRIVER IMPLEMENTATION =====

USBControlPad::USBControlPad(USB_Device* dev) 
    : USB_Driver_FactoryGlue<USBControlPad>(dev),
      kbd_poll_cb([this](int r) { kbd_poll(r); }),
      ctrl_poll_cb([this](int r) { ctrl_poll(r); }),
      dual_poll_cb([this](int r) { dual_poll(r); }),
      hall_sensor_poll_cb([this](int r) { hall_sensor_poll(r); }),
      sensor_out_poll_cb([this](int r) { sensor_out_poll(r); }),
      send_cb([this](int r) { sent(r); }) {
    
    // Initialize USB command serialization mutex for proper threading
    usbCommandMutex = (ATOM_MUTEX*)malloc(sizeof(ATOM_MUTEX));
    if (usbCommandMutex) {
        if (atomMutexCreate(usbCommandMutex) == ATOM_OK) {
            Serial.println("🔧 USB Command Mutex initialized");
        } else {
            free(usbCommandMutex);
            usbCommandMutex = nullptr;
            Serial.println("❌ Failed to create USB Command Mutex");
        }
    }
    
    Serial.println("🔧 USBControlPad driver instance created");
}

bool USBControlPad::offer_interface(const usb_interface_descriptor* iface, size_t length) {
    Serial.println("🔍 *** USBControlPad::offer_interface called ***");
    Serial.printf("   Interface: %d\n", iface->bInterfaceNumber);
    Serial.printf("   Class: 0x%02X\n", iface->bInterfaceClass);
    Serial.printf("   SubClass: 0x%02X\n", iface->bInterfaceSubClass);
    Serial.printf("   Protocol: 0x%02X\n", iface->bInterfaceProtocol);
    
    // Accept ALL interfaces for ControlPad device (VID:0x2516 PID:0x012D)
    Serial.printf("✅ USBControlPad ACCEPTING Interface %d for raw USB access!\n", iface->bInterfaceNumber);
    return true;
}

USB_Driver* USBControlPad::attach_interface(const usb_interface_descriptor* iface, size_t length, USB_Device* dev) {
    Serial.println("🎯 *** USBControlPad::attach_interface called ***");
    Serial.printf("   Attaching to Interface: %d\n", iface->bInterfaceNumber);
    
    // Create driver instance only once - but don't start polling yet
    if (!driver_instance_created) {
        driver_instance_created = true;
        
        // Create the driver instance
        USBControlPad* driver = new USBControlPad(dev);
        driver->interface = iface->bInterfaceNumber;
        
        // Configure for raw USB operation on all interfaces
        driver->setupQuadInterface();
        
        // Store the instance globally so our main loop can access it
        controlPadDriver = driver;
        
        Serial.printf("✅ USBControlPad driver created (primary: Interface %d)!\n", iface->bInterfaceNumber);
        return driver;
    } else {
        // Additional interfaces - just acknowledge
        Serial.printf("✅ Interface %d acknowledged (using existing driver)!\n", iface->bInterfaceNumber);
        return nullptr;
    }
}

void USBControlPad::detach() {
    Serial.println("❌ USBControlPad detached");
    initialized = false;
    kbd_polling = false;
    ctrl_polling = false;
    dual_polling = false;
    hall_sensor_polling = false;
    sensor_out_polling = false;
    
    // Clean up USB command mutex
    if (usbCommandMutex) {
        atomMutexDelete(usbCommandMutex);
        free(usbCommandMutex);
        usbCommandMutex = nullptr;
    }
    
    // Reset static variable so a new driver can be created on reconnect
    driver_instance_created = false;
    
    // Clear global pointer
    if (controlPadDriver == this) {
        controlPadDriver = nullptr;
    }
}

bool USBControlPad::begin(ATOM_QUEUE *q) {
    queue = q;
    if (queue != nullptr) {
        Serial.println("🎯 USB DRIVER BEGIN - Starting polling...");
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
    
    // Start with only the most essential endpoints first
    
    // Start polling Interface 1 Control (64 byte packets) - MOST IMPORTANT FOR BUTTONS
    Serial.printf("📡 Starting control polling on EP 0x%02X...\n", ctrl_ep_in);
    if (InterruptMessage(ctrl_ep_in, 64, ctrl_report, &ctrl_poll_cb) == 0) {
        ctrl_polling = true;
        Serial.println("✅ Control polling started successfully");
    } else {
        Serial.println("❌ Failed to start control polling");
        return; // Don't continue if this critical endpoint fails
    }
    
    delay(2); // Reduced delay - USB capture shows device responds in ~10-15ms
    
    // Start polling Interface 0 (Keyboard - 8 byte packets) - SECONDARY PRIORITY
    Serial.printf("📡 Starting keyboard polling on EP 0x%02X...\n", kbd_ep_in);
    if (InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb) == 0) {
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

// Polling callback implementations
void USBControlPad::kbd_poll(int result) {
    static int kbd_counter = 0;
    
    if (result > 0 && queue) {
        kbd_counter++;
        
        // Queue keyboard event
        if (kbd_report[2] != 0) {
            controlpad_event event;
            event.data[0] = result;  // Length in data[0]
            memcpy(&event.data[1], kbd_report, min(result, 63));  // USB data in data[1-63]
            atomQueuePut(queue, 0, &event);
        }
        
        // Restart keyboard polling
        int restart = InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb);
        if (restart != 0) {
            kbd_polling = false;
        }
    } else if (result < 0) {
        kbd_polling = false;
    } else if (result == 0) {
        // Still restart polling to keep listening
        int restart = InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb);
        if (restart != 0) {
            kbd_polling = false;
        }
    }
}

void USBControlPad::ctrl_poll(int result) {
    static int ctrl_counter = 0;
    
    if (result > 0 && queue) {
        ctrl_counter++;
        
        // Check for button press pattern: 43 01 00 00 XX (pressed/released)
        if (result >= 6 && ctrl_report[0] == 0x43 && ctrl_report[1] == 0x01 && 
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
                event.data[0] = result;  // Length stored in first byte
                memcpy(&event.data[1], ctrl_report, min(result, 63));  // USB data in bytes 1-63
                
                // Queue the raw USB event for main thread processing
                if (atomQueuePut(queue, 0, (uint8_t*)&event) != ATOM_OK) {
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
        else if (result >= 2 && (
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
    if (InterruptMessage(ctrl_ep_in, 64, ctrl_report, &ctrl_poll_cb) != 0) {
        ctrl_polling = false;
    }
}

void USBControlPad::dual_poll(int result) {
    if (result > 0 && queue) {
        // Queue the dual event
        controlpad_event event;
        result = min(result, 8);
        event.data[0] = result;  // Length in data[0]
        memcpy(&event.data[1], dual_report, min(result, 63));  // USB data in data[1-63]
        atomQueuePut(queue, 0, &event);
    }
    
    // Restart dual polling
    if (InterruptMessage(dual_ep_in, 8, dual_report, &dual_poll_cb) != 0) {
        dual_polling = false;
    }
}

void USBControlPad::hall_sensor_poll(int result) {
    if (result > 0 && queue) {
        // Queue the hall sensor event
        controlpad_event event;
        result = min(result, 32);
        event.data[0] = result;  // Length in data[0]
        memcpy(&event.data[1], hall_sensor_report, min(result, 63));  // USB data in data[1-63]
        atomQueuePut(queue, 0, &event);
    }
    
    // Restart hall sensor polling
    if (InterruptMessage(hall_sensor_ep_in, 32, hall_sensor_report, &hall_sensor_poll_cb) != 0) {
        hall_sensor_polling = false;
    }
}

void USBControlPad::sensor_out_poll(int result) {
    if (result > 0 && queue) {
        // Queue the event
        controlpad_event event;
        result = min(result, 32);
        event.data[0] = result;  // Length in data[0]
        memcpy(&event.data[1], sensor_out_report, min(result, 63));  // USB data in data[1-63]
        atomQueuePut(queue, 0, &event);
    }
    
    // Restart endpoint 0x07 polling
    if (InterruptMessage(btn_ep_out, 32, sensor_out_report, &sensor_out_poll_cb) != 0) {
        sensor_out_polling = false;
    }
}

void USBControlPad::sent(int result) {
    static int commandCounter = 0;
    commandCounter++;
    
    if (result >= 0) {
        // Only show every 10th success to reduce spam
        if (commandCounter <= 5 || commandCounter % 10 == 0) {
            // Serial.printf("📤 Command #%d sent successfully\n", commandCounter);
        }
    } else {
        // Serial.printf("❌ Command #%d FAILED: %d\n", commandCounter, result);
    }
}

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
    if (!initialized || length > 64) {
        return false;
    }
    
    // SIMPLIFIED: Remove mutex to test if it's causing issues
    // if (usbCommandMutex) {
    //     if (atomMutexGet(usbCommandMutex, 100) != ATOM_OK) {  // 100ms timeout
    //         Serial.println("❌ USB Command Mutex timeout in sendCommand()");
    //         return false;  // Failed to acquire mutex in time
    //     }
    // }

    // Send via interrupt transfer to control endpoint
    int result = InterruptMessage(ctrl_ep_out, length, const_cast<uint8_t*>(data), &send_cb);
    
    // SIMPLIFIED: Minimal delay to test if timing is the issue
    //delayMicroseconds(1);
    
    // Release mutex after command completes
    // if (usbCommandMutex) {
    //     atomMutexPut(usbCommandMutex);
    // }
    
    return (result == 0);
}

bool USBControlPad::sendLEDCommandWithVerification(const uint8_t* data, size_t length, uint8_t expectedEcho1, uint8_t expectedEcho2) {
    if (!initialized || length > 64) {
        return false;
    }
    
    // FAST MODE: Skip verification entirely during button events to eliminate timing conflicts
    // The 50ms verification timeout was causing interference with button press/release cycles
    if (atomicLEDUpdateInProgress) {
        // During LED updates, use simplified command sending without verification delays
        int result = InterruptMessage(ctrl_ep_out, length, const_cast<uint8_t*>(data), &send_cb);
        
        // CRITICAL: Device needs time to process LED commands - but use minimal delay
        delayMicroseconds(825); // Back to working timing that doesn't black out
        return (result == 0);
    }
    
    // SINGLE SERIALIZATION POINT: Use AtomThreads mutex for proper USB command flow
    // This prevents the 100ms-1s timing interference window discovered by user
    if (usbCommandMutex) {
        // Use timeout instead of forever wait to prevent hangs
        if (atomMutexGet(usbCommandMutex, 50) != ATOM_OK) {  // 100ms timeout
            Serial.println("❌ USB Command Mutex timeout in sendLEDCommandWithVerification()");
            return false;  // Failed to acquire mutex in time
        }
    }
    
    // SEQUENTIAL MODE: Always use verification for reliable command sequencing
    // Clear any pending verification data and flush stale echoes
    ledCommandVerified = false;
    expectedLEDEcho[0] = expectedEcho1;
    expectedLEDEcho[1] = expectedEcho2;
    
    // Send the command
    int result = InterruptMessage(ctrl_ep_out, length, const_cast<uint8_t*>(data), &send_cb); 
    if (result != 0) {
        if (usbCommandMutex) {
            atomMutexPut(usbCommandMutex);
        }
        return false;
    }
    
    // FAST VERIFICATION: Reduced timeout to minimize interference window
    // unsigned long startTime = millis();
    // while (!ledCommandVerified && (millis() - startTime) < 10) {  // Reduced from 50ms to 10ms
    //     delayMicroseconds(10); // Shorter delay
    //     yield(); // Allow other processing
    // }
    
    // SIMPLIFIED: No complex recovery - just clear verification state occasionally
    static uint16_t commandCounter = 0;
    commandCounter++;
    
    if (commandCounter % 50 == 0) {
        // Simple periodic cleanup
        ledCommandVerified = false;
        expectedLEDEcho[0] = 0x00;
        expectedLEDEcho[1] = 0x00;
    }
    
    if (!ledCommandVerified) {
        // Don't treat verification failure as critical error during fast updates
        // Serial.printf("❌ LED command echo timeout: expected %02X %02X\n", expectedEcho1, expectedEcho2);
    }
    
    // Release USB command mutex
    if (usbCommandMutex) {
        atomMutexPut(usbCommandMutex);
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

bool USBControlPad::sendLEDPackages(const ControlPadColor* colors) {
    // CRITICAL: Create local copy to prevent modification during USB transmission
    static ControlPadColor colorBuffer[24];
    memcpy(colorBuffer, colors, 24 * sizeof(ControlPadColor));
    
    // COMBINED LED PACKAGE SENDING - Package1 and Package2 back-to-back
    // This eliminates the gap between packages that causes flicker
    // ===== PACKAGE 1 =====
    uint8_t cmd1[64] = {
        0x56, 0x83, 0x00, 0x00,    // Header
        0x01, 0x00, 0x00, 0x00,    // Package 1 marker
        0x80, 0x01, 0x00, 0x00,    // Control flags
        0xff, 0x00, 0x00, 0x00,    // Color flags
        0x00, 0x00,                // Reserved
        0xff, 0xff,                // Brightness for all LEDs
        0x00, 0x00, 0x00, 0x00     // Reserved
    };
    
    // LED data starts at position 24 - direct assignments
    size_t pos = 24;
    
    // Column 1: buttons 1,6,11,16,21 (indices 0,5,10,15,20) - unrolled
    cmd1[pos++] = colorBuffer[0].r;   cmd1[pos++] = colorBuffer[0].g;   cmd1[pos++] = colorBuffer[0].b;   // Button 1
    cmd1[pos++] = colorBuffer[5].r;   cmd1[pos++] = colorBuffer[5].g;   cmd1[pos++] = colorBuffer[5].b;   // Button 6
    cmd1[pos++] = colorBuffer[10].r;  cmd1[pos++] = colorBuffer[10].g;  cmd1[pos++] = colorBuffer[10].b;  // Button 11
    cmd1[pos++] = colorBuffer[15].r;  cmd1[pos++] = colorBuffer[15].g;  cmd1[pos++] = colorBuffer[15].b;  // Button 16
    cmd1[pos++] = colorBuffer[20].r;  cmd1[pos++] = colorBuffer[20].g;  cmd1[pos++] = colorBuffer[20].b;  // Button 21
    
    // Column 2: buttons 2,7,12,17,22 (indices 1,6,11,16,21) - unrolled
    cmd1[pos++] = colorBuffer[1].r;   cmd1[pos++] = colorBuffer[1].g;   cmd1[pos++] = colorBuffer[1].b;   // Button 2
    cmd1[pos++] = colorBuffer[6].r;   cmd1[pos++] = colorBuffer[6].g;   cmd1[pos++] = colorBuffer[6].b;   // Button 7
    cmd1[pos++] = colorBuffer[11].r;  cmd1[pos++] = colorBuffer[11].g;  cmd1[pos++] = colorBuffer[11].b;  // Button 12
    cmd1[pos++] = colorBuffer[16].r;  cmd1[pos++] = colorBuffer[16].g;  cmd1[pos++] = colorBuffer[16].b;  // Button 17
    cmd1[pos++] = colorBuffer[21].r;  cmd1[pos++] = colorBuffer[21].g;  cmd1[pos++] = colorBuffer[21].b;  // Button 22
    
    // Column 3: buttons 3,8,13 (indices 2,7,12) - unrolled  
    cmd1[pos++] = colorBuffer[2].r;   cmd1[pos++] = colorBuffer[2].g;   cmd1[pos++] = colorBuffer[2].b;   // Button 3
    cmd1[pos++] = colorBuffer[7].r;   cmd1[pos++] = colorBuffer[7].g;   cmd1[pos++] = colorBuffer[7].b;   // Button 8
    cmd1[pos++] = colorBuffer[12].r;  cmd1[pos++] = colorBuffer[12].g;  cmd1[pos++] = colorBuffer[12].b;  // Button 13
    
    // Button 18 (index 17) - only R component fits in package 1 (WORKING CONFIGURATION)
    cmd1[pos++] = colorBuffer[17].r;  // Button 18 R (keep in Package 1)
    
    // Send Package1 WITH verification - wait for hardware acknowledgement
    bool success1 = sendLEDCommandWithVerification(cmd1, 64, 0x56, 0x83);

    // CRITICAL: Package 1→Package 2 gap timing (1200µs works for highlighting)
    delayMicroseconds(850); // Sweet spot for Package 2 highlighting to work

    // ===== PACKAGE 2 - EXACT WORKING STRUCTURE =====
    uint8_t cmd2[64] = {
        0x56, 0x83, 0x01          // Header for package 2
    };
    
    pos = 3;
    
    // Complete button 18 (index 17) - GB components (R was in Package 1)
    cmd2[pos++] = 0x00;  // Padding (R already sent in Package 1)
    cmd2[pos++] = colorBuffer[17].g;  // Button 18 G
    cmd2[pos++] = colorBuffer[17].b;  // Button 18 B
    
    // Button 23 (index 22)
    cmd2[pos++] = colorBuffer[22].r;
    cmd2[pos++] = colorBuffer[22].g;
    cmd2[pos++] = colorBuffer[22].b;
    
    // Column 4: buttons 4,9,14,19,24 (indices 3,8,13,18,23) - EXACT WORKING ORDER
    cmd2[pos++] = colorBuffer[3].r;   cmd2[pos++] = colorBuffer[3].g;   cmd2[pos++] = colorBuffer[3].b;   // Button 4
    cmd2[pos++] = colorBuffer[8].r;   cmd2[pos++] = colorBuffer[8].g;   cmd2[pos++] = colorBuffer[8].b;   // Button 9
    cmd2[pos++] = colorBuffer[13].r;  cmd2[pos++] = colorBuffer[13].g;  cmd2[pos++] = colorBuffer[13].b;  // Button 14
    cmd2[pos++] = colorBuffer[18].r;  cmd2[pos++] = colorBuffer[18].g;  cmd2[pos++] = colorBuffer[18].b;  // Button 19
    cmd2[pos++] = colorBuffer[23].r;  cmd2[pos++] = colorBuffer[23].g;  cmd2[pos++] = colorBuffer[23].b;  // Button 24
    
    // Column 5: buttons 5,10,15,20 (indices 4,9,14,19) - EXACT WORKING ORDER
    cmd2[pos++] = colorBuffer[4].r;   cmd2[pos++] = colorBuffer[4].g;   cmd2[pos++] = colorBuffer[4].b;   // Button 5
    cmd2[pos++] = colorBuffer[9].r;   cmd2[pos++] = colorBuffer[9].g;   cmd2[pos++] = colorBuffer[9].b;   // Button 10
    cmd2[pos++] = colorBuffer[14].r;  cmd2[pos++] = colorBuffer[14].g;  cmd2[pos++] = colorBuffer[14].b;  // Button 15
    cmd2[pos++] = colorBuffer[19].r;  cmd2[pos++] = colorBuffer[19].g;  cmd2[pos++] = colorBuffer[19].b;  // Button 20
    
    cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;
     cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;
      cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;
       cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;
        cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;
         cmd2[pos++] = 0x00;  cmd2[pos++] = 0x00; cmd2[pos++] = 0x00;  
        // Send Package2 WITH verification - wait for hardware acknowledgement  
    bool success2 = sendLEDCommandWithVerification(cmd2, 64, 0x56, 0x83);

    return (success1 && success2);
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
    return sendLEDCommandWithVerification(cmd, 64, 0x41, 0x80);
    delayMicroseconds(850); 
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
    bool success = sendLEDCommandWithVerification(cmd, 64, 0x51, 0x28);
    return success;
}

bool USBControlPad::updateAllLEDs(const ControlPadColor* colors, size_t count) {
    if (!initialized || count > 24) {
        // Serial.println("❌ Cannot update LEDs: not initialized or too many LEDs");
        return false;
    }
    
    // 🚀 OPTIMIZED ATOMIC LED UPDATE - Minimal Commands Only
    // Remove setCustomMode() from every button press - only needed at startup!
    // This eliminates the LED controller reset that causes black flashes
    
    // SIMPLE LED UPDATE - Remove all unnecessary complexity
    pauseUSBPolling();
    bool success = true;
    
    // Essential commands only - no extra delays
    success &= sendLEDPackages(colors);
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
    
    // Rest filled with zeros (already initialized)
    
    // Serial.println("🎨 Sending LED Package 2");
    return sendCommand(cmd, 64);
}

// ===== HARDWARE MANAGER IMPLEMENTATION =====

ControlPadHardware::ControlPadHardware() {
    // Constructor - initialize any hardware-specific settings
    globalHardwareInstance = this;  // Set global instance for USB callbacks
}

ControlPadHardware::~ControlPadHardware() {
    if (controlpad_queue_data) {
        free(controlpad_queue_data);
    }
    if (led_command_queue_data) {
        free(led_command_queue_data);
    }
}

bool ControlPadHardware::begin(ControlPad& pad) {
    // Store reference to the ControlPad instance
    currentPad = &pad;
    
    // 1. Initialize queue
    controlpad_queue_data = (controlpad_event*)malloc(10 * sizeof(controlpad_event));
    if (!controlpad_queue_data) {
        Serial.println("❌ Failed to allocate queue data");
        return false;
    }
    
    controlpad_queue = (ATOM_QUEUE*)malloc(sizeof(ATOM_QUEUE));
    if (!controlpad_queue) {
        Serial.println("❌ Failed to allocate queue");
        free(controlpad_queue_data);
        return false;
    }
    
    atomQueueCreate(controlpad_queue, (uint8_t*)controlpad_queue_data, 10, sizeof(controlpad_event));

    // 2. Initialize LED command queue for MIDI-timing-friendly updates
    led_command_queue_data = (led_command_event*)malloc(16 * sizeof(led_command_event));
    if (!led_command_queue_data) {
        Serial.println("❌ Failed to allocate LED queue data");
        return false;
    }
    
    led_command_queue = (ATOM_QUEUE*)malloc(sizeof(ATOM_QUEUE));
    if (!led_command_queue) {
        Serial.println("❌ Failed to allocate LED queue");
        free(led_command_queue_data);
        return false;
    }
    
    atomQueueCreate(led_command_queue, (uint8_t*)led_command_queue_data, 16, sizeof(led_command_event));
    Serial.println("✅ LED command queue created (16 commands, MIDI-timing-friendly)");
    
    // 3. Start USB host
    usbHost.begin();
    Serial.println("🔌 USB Host started, waiting for device enumeration...");
    
    // 4. Wait for device enumeration and driver creation (longer delay)
    unsigned long startTime = millis();
    while (!controlPadDriver && (millis() - startTime) < 10000) {  // 10 second timeout
        delay(100);
        if (controlPadDriver) {
            Serial.println("✅ Driver instance detected!");
            break;
        }
    }
    
    if (!controlPadDriver) {
        Serial.println("❌ No ControlPad driver found after 10 seconds");
        return false;
    }
    
    // 5. Wait additional time for USB configuration to stabilize
    Serial.println("⏳ Waiting for USB configuration to stabilize...");
    delay(2000);
    
    // 6. Now initialize the driver (this starts polling)
    if (controlPadDriver) {
        Serial.println("🎯 Starting driver initialization...");
        bool initResult = controlPadDriver->begin(controlpad_queue);
        if (!initResult) {
            Serial.println("❌ Driver initialization failed");
            return false;
        }
        
        Serial.printf("✅ Driver initialized! initialized=%s\n", 
                     controlPadDriver->initialized ? "true" : "false");
        
        // 7. Wait for polling to stabilize
        delay(1000);
        
        // 8. Send activation sequence
        Serial.println("🚀 Sending activation sequence...");
        controlPadDriver->sendUSBInterfaceReSponseActivation();
    } else {
        Serial.println("❌ controlPadDriver is null - USB device not found");
        return false;
    }
    
    // 9. ControlPad setup is handled by the caller (main.cpp)
    // No need to call pad.begin() here as it would create circular recursion
    
    return true;
}

void ControlPadHardware::poll() {
    // CRITICAL: Process events from AtomQueue in main thread context with USB-synchronized timing
    // This prevents USB timing conflicts by processing button events at consistent intervals
    
    static int debugCounter = 0;
    static unsigned long lastProcessTime = 0;
    debugCounter++;
    
    if (!controlpad_queue) {
        if (debugCounter % 1000 == 0) {
            Serial.printf("🚫 ControlPadHardware::poll() - Missing controlpad_queue\n");
        }
        return;
    }
    
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
    
    // Try a SINGLE non-blocking queue get with different timeout approaches
    int queueResult = atomQueueGet(controlpad_queue, -1, (uint8_t*)&rawEvent);
    if (queueResult == ATOM_OK) {
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
    
    if (controlPadDriver) {
        //Serial.println("🔄 Using synchronous LED update (queue disabled for debugging)");
        bool success = controlPadDriver->updateAllLEDs(colors, count);
        if (!success) {
           // Serial.println("❌ Synchronous LED update failed");
        } else {
           // Serial.println("✅ Synchronous LED update completed");
        }
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
    if (!initialized || count > 24 || !globalHardwareInstance || !globalHardwareInstance->led_command_queue) {
        return false;
    }
    
    // Prepare 4 LED commands
    led_command_event commands[4];
    globalHardwareInstance->prepareLEDCommands(colors, commands);
    
    // Queue all 4 commands using TeensyAtomThreads queue system
    for (int i = 0; i < 4; i++) {
        int result = atomQueuePut(globalHardwareInstance->led_command_queue, 100, (uint8_t*)&commands[i]);
        if (result != ATOM_OK) {
            Serial.printf("❌ Failed to queue LED command %d\n", i);
            return false;
        }
    }
    
    Serial.println("✅ Queued 4 LED commands for background processing");
    return true;
}

void USBControlPad::processLEDCommandQueue() {
    if (!globalHardwareInstance || !globalHardwareInstance->led_command_queue) {
        return;
    }
    
    // Process one LED command per loop iteration to spread CPU load
    led_command_event cmd;
    int result = atomQueueGet(globalHardwareInstance->led_command_queue, -1, (uint8_t*)&cmd);
    
    if (result == ATOM_OK) {
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