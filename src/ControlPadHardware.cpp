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
            memcpy(event.data, kbd_report, result);
            event.len = result;
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
            uint8_t buttonId = ctrl_report[4];
            uint8_t state = ctrl_report[5];
            
            // Filter out spurious events and only process valid button states
            if (state != 0xC0 && state != 0x40) {
                goto restart_polling; // Skip invalid states
            }
            
            // Only log valid button events (minimal output)
            if (state == 0xC0) {
                // Serial.printf("🔴 Button pressed: ID=0x%02X\n", buttonId);
            } else if (state == 0x40) {
                // Serial.printf("🔘 Button released: ID=0x%02X\n", buttonId);
            }
            
            // IMMEDIATE PROCESSING: Convert USB button event to ControlPad event right here
            // This bypasses the problematic USB queue entirely
            
            // Skip spurious USB ID 0x00
            if (buttonId == 0x00) {
                goto restart_polling;
            }
            
            if (buttonId >= 1 && buttonId <= 24) {  // Valid button range 1-24
                // CORRECT COLUMN-MAJOR MAPPING based on LED data structure:
                // Physical layout is 5x5 grid, but LED data is sent column-wise
                // Column 1: buttons 1,6,11,16,21 → LED indices 0,1,2,3,4
                // Column 2: buttons 2,7,12,17,22 → LED indices 5,6,7,8,9
                // Column 3: buttons 3,8,13,18,23 → LED indices 10,11,12,13,14
                // Column 4: buttons 4,9,14,19,24 → LED indices 15,16,17,18,19
                // Column 5: buttons 5,10,15,20 → LED indices 20,21,22,23
                
                //uint8_t physicalButton = buttonId;  // USB sends 1-24
                //uint8_t buttonZeroBased = physicalButton;  // Keep as-is since other buttons work
                uint8_t row = buttonId / 5;  // Row (0-4)
                uint8_t col = buttonId % 5;  // Column (0-4)
                
                // Direct mapping without offsets since user's adjustments work
                uint8_t verticalIndex = col * 5 + row;  // LED index in column-major order
                
                // Special fix for button 1 - it should map to LED index 0
                //if (physicalButton == 1) {
                 //   verticalIndex = 1;  // Force button 1 to LED 1 (index 0)
                //}
                
                // Minimal logging - show the mapping calculation
                // if (state == 0xC0) {
                //     Serial.printf("BTN %d PRESS (USB:0x%02X, row:%d, col:%d → LED:%d)\n", 
                //                  buttonId, buttonId, row, col, verticalIndex + 1);
                // }
                
                ControlPadEvent event;
                event.type = ControlPadEventType::Button;
                event.button.button = verticalIndex;
                
                if (state == 0xC0) {
                    event.button.pressed = true;
                    // Serial.printf("🎯 IMMEDIATE Event: Physical Button %d PRESSED (USB ID: 0x%02X, vertical index: %d)\n", 
                    //              verticalIndex + 1, buttonId, verticalIndex);
                } else if (state == 0x40) {
                    event.button.pressed = false;
                    // Serial.printf("🎯 IMMEDIATE Event: Physical Button %d RELEASED (USB ID: 0x%02X, vertical index: %d)\n", 
                    //              verticalIndex + 1, buttonId, verticalIndex);
                } else {
                    // Unknown state, skip
                    // Serial.printf("❌ Unknown button state: 0x%02X\n", state);
                    goto restart_polling; // Skip to restart polling
                }
                
                // Get the global ControlPadHardware instance to access currentPad
                if (globalHardwareInstance && globalHardwareInstance->currentPad) {
                    // Serial.println("🔧 IMMEDIATE: About to push event directly to ControlPad");
                    globalHardwareInstance->currentPad->pushEvent(event);
                    // Serial.println("🔧 IMMEDIATE: Event pushed successfully");
                } else {
                    // Serial.println("❌ IMMEDIATE: No ControlPad instance available");
                }
            } else {
                Serial.printf("❌ Invalid button ID: 0x%02X (valid range: 1-24)\n", buttonId);
            }
        } 
        // Check for command echoes: 56 8x (LED), 52 xx, 42 xx, 43 xx, 41 xx
        else if (result >= 2 && (
                (ctrl_report[0] == 0x56 && (ctrl_report[1] == 0x81 || ctrl_report[1] == 0x83)) ||  // LED commands
                (ctrl_report[0] == 0x52) ||  // Effect mode commands
                (ctrl_report[0] == 0x42) ||  // Activation commands 
                (ctrl_report[0] == 0x43) ||  // Status commands
                (ctrl_report[0] == 0x41)     // Finalize commands
                )) {
            // Command echo received - verify it matches expectations
            if (expectedLEDEcho[0] == ctrl_report[0] && expectedLEDEcho[1] == ctrl_report[1]) {
                ledCommandVerified = true;
                // Serial.printf("✅ Command echo verified: %02X %02X\n", ctrl_report[0], ctrl_report[1]);
            } else if (expectedLEDEcho[0] != 0x00) {  // Only report mismatch if we're expecting something
                Serial.printf("❌ Command echo mismatch: expected %02X %02X, got %02X %02X\n",
                             expectedLEDEcho[0], expectedLEDEcho[1], ctrl_report[0], ctrl_report[1]);
            }
            // If we got any command echo but weren't expecting it, silently ignore (stale echo)
        } else {
            // Don't process non-button events
            // Serial.printf("🚫 Ignoring non-button event (len=%d, header: %02X %02X %02X %02X)\n", 
            //              result, 
            //              result > 0 ? ctrl_report[0] : 0,
            //              result > 1 ? ctrl_report[1] : 0,
            //              result > 2 ? ctrl_report[2] : 0,
            //              result > 3 ? ctrl_report[3] : 0);
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
        memcpy(event.data, dual_report, result);
        event.len = result;
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
        memcpy(event.data, hall_sensor_report, result);
        event.len = result;
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
        memcpy(event.data, sensor_out_report, result);
        event.len = result;
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
    setCustomMode();
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
        // Fallback to unverified send
        int result = InterruptMessage(ctrl_ep_out, 64, cmd, &send_cb);
        delay(10);  // Longer delay on failure
        return (result == 0);
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
    
    // Send via interrupt transfer to control endpoint
    int result = InterruptMessage(ctrl_ep_out, length, const_cast<uint8_t*>(data), &send_cb);
    
    // PROPER USB ACK TIMING: Wait for device to process command
    // Based on USB capture: Windows uses 8-15ms delays between commands
    // This prevents device buffer overflow and system hangs
    delay(1); // Optimized timing - reduced from 2ms for faster LED updates
    
    return (result == 0);
}

bool USBControlPad::sendLEDCommandWithVerification(const uint8_t* data, size_t length, uint8_t expectedEcho1, uint8_t expectedEcho2) {
    if (!initialized || length > 64) {
        return false;
    }
    
    // SEQUENTIAL MODE: Always use verification for reliable command sequencing
    // Clear any pending verification data and flush stale echoes
    ledCommandVerified = false;
    expectedLEDEcho[0] = expectedEcho1;
    expectedLEDEcho[1] = expectedEcho2;
    
    // Brief delay to ensure any previous command echoes are processed
   // delay(8);
    
    // Send the command
    int result = InterruptMessage(ctrl_ep_out, length, const_cast<uint8_t*>(data), &send_cb);
    
    if (result != 0) {
        return false;
    }
    
    // Wait for echo verification with proper timeout for sequential processing
    unsigned long startTime = millis();
    while (!ledCommandVerified && (millis() - startTime) < 50) {  // Extended timeout for reliable ACK
       // delay(1); // Small delay to prevent overwhelming USB polling
        yield(); // Allow other processing
    }
    
    if (!ledCommandVerified) {
        // Serial.printf("❌ LED command echo timeout: expected %02X %02X\n", expectedEcho1, expectedEcho2);
        return false;
    }
    
    // Proper delay for device processing to ensure ACK is complete
    //delay(2);  // Sufficient time for device to process and prepare for next command
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

bool USBControlPad::setCustomMode() {
    uint8_t cmd[64] = {
        0x56, 0x81, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0xbb, 0xbb, 0xbb, 0xbb  // Custom mode pattern
        // Rest filled with zeros
    };
    
    // Serial.println("🎨 Setting custom LED mode");
    return sendLEDCommandWithVerification(cmd, 64, 0x56, 0x81);
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
    cmd1[pos++] = colors[0].r;   cmd1[pos++] = colors[0].g;   cmd1[pos++] = colors[0].b;   // Button 1
    cmd1[pos++] = colors[5].r;   cmd1[pos++] = colors[5].g;   cmd1[pos++] = colors[5].b;   // Button 6
    cmd1[pos++] = colors[10].r;  cmd1[pos++] = colors[10].g;  cmd1[pos++] = colors[10].b;  // Button 11
    cmd1[pos++] = colors[15].r;  cmd1[pos++] = colors[15].g;  cmd1[pos++] = colors[15].b;  // Button 16
    cmd1[pos++] = colors[20].r;  cmd1[pos++] = colors[20].g;  cmd1[pos++] = colors[20].b;  // Button 21
    
    // Column 2: buttons 2,7,12,17,22 (indices 1,6,11,16,21) - unrolled
    cmd1[pos++] = colors[1].r;   cmd1[pos++] = colors[1].g;   cmd1[pos++] = colors[1].b;   // Button 2
    cmd1[pos++] = colors[6].r;   cmd1[pos++] = colors[6].g;   cmd1[pos++] = colors[6].b;   // Button 7
    cmd1[pos++] = colors[11].r;  cmd1[pos++] = colors[11].g;  cmd1[pos++] = colors[11].b;  // Button 12
    cmd1[pos++] = colors[16].r;  cmd1[pos++] = colors[16].g;  cmd1[pos++] = colors[16].b;  // Button 17
    cmd1[pos++] = colors[21].r;  cmd1[pos++] = colors[21].g;  cmd1[pos++] = colors[21].b;  // Button 22
    
    // Column 3: buttons 3,8,13 (indices 2,7,12) - unrolled  
    cmd1[pos++] = colors[2].r;   cmd1[pos++] = colors[2].g;   cmd1[pos++] = colors[2].b;   // Button 3
    cmd1[pos++] = colors[7].r;   cmd1[pos++] = colors[7].g;   cmd1[pos++] = colors[7].b;   // Button 8
    cmd1[pos++] = colors[12].r;  cmd1[pos++] = colors[12].g;  cmd1[pos++] = colors[12].b;  // Button 13
    
    // Button 18 (index 17) - only R component fits in package 1
    cmd1[pos++] = colors[17].r;  // Button 18 R
    
    // Send Package1 with verification (expect echo: 56 83) - WAIT for ACK
    bool success1 = sendLEDCommandWithVerification(cmd1, 64, 0x56, 0x83);
    
    if (!success1) {
        // Serial.println("❌ Package 1 (56 83 00) failed verification - retrying");
        // Retry once more with verification
        //delay(2); // Brief pause before retry
        success1 = sendLEDCommandWithVerification(cmd1, 64, 0x56, 0x83);
    }
    
    // CRITICAL: Wait for Package 1 to be fully processed before sending Package 2
    if (success1) {
       // delay(5); // Longer gap between packages to prevent flicker during fast button transitions
    }

    // ===== PACKAGE 2 =====
    uint8_t cmd2[64] = {
        0x56, 0x83, 0x01          // Header for package 2
    };
    
    pos = 3;
    
    // Complete button 18 (index 17) - GB components
    cmd2[pos++] = 0x00;  // Padding
    cmd2[pos++] = colors[17].g;  // Button 18 G
    cmd2[pos++] = colors[17].b;  // Button 18 B
    
    // Button 23 (index 22)
    cmd2[pos++] = colors[22].r;
    cmd2[pos++] = colors[22].g;
    cmd2[pos++] = colors[22].b;
    
    // Column 4: buttons 4,9,14,19,24 (indices 3,8,13,18,23) - unrolled
    cmd2[pos++] = colors[3].r;   cmd2[pos++] = colors[3].g;   cmd2[pos++] = colors[3].b;   // Button 4
    cmd2[pos++] = colors[8].r;   cmd2[pos++] = colors[8].g;   cmd2[pos++] = colors[8].b;   // Button 9
    cmd2[pos++] = colors[13].r;  cmd2[pos++] = colors[13].g;  cmd2[pos++] = colors[13].b;  // Button 14
    cmd2[pos++] = colors[18].r;  cmd2[pos++] = colors[18].g;  cmd2[pos++] = colors[18].b;  // Button 19
    cmd2[pos++] = colors[23].r;  cmd2[pos++] = colors[23].g;  cmd2[pos++] = colors[23].b;  // Button 24
    
    // Column 5: buttons 5,10,15,20 (indices 4,9,14,19) - unrolled
    cmd2[pos++] = colors[4].r;   cmd2[pos++] = colors[4].g;   cmd2[pos++] = colors[4].b;   // Button 5
    cmd2[pos++] = colors[9].r;   cmd2[pos++] = colors[9].g;   cmd2[pos++] = colors[9].b;   // Button 10
    cmd2[pos++] = colors[14].r;  cmd2[pos++] = colors[14].g;  cmd2[pos++] = colors[14].b;  // Button 15
    cmd2[pos++] = colors[19].r;  cmd2[pos++] = colors[19].g;  cmd2[pos++] = colors[19].b;  // Button 20
        
    // Send Package2 with verification (expect echo: 56 83) - SEQUENTIAL AFTER Package1
    bool success2 = false;
    if (success1) { // Only send Package2 if Package1 succeeded
        success2 = sendLEDCommandWithVerification(cmd2, 64, 0x56, 0x83);
        
        if (!success2) {
            // Serial.println("❌ Package 2 (56 83 01) failed verification - retrying");
            // Retry once more with verification
            delay(2); // Brief pause before retry
            success2 = sendLEDCommandWithVerification(cmd2, 64, 0x56, 0x83);
            
            // If Package 2 still failed, this is critical for buttons 5,15,20,24
            if (!success2) {
                Serial.println("❌ CRITICAL: Package 2 failed after retry - Column 5 LEDs may flicker");
            }
        }
    } else {
        Serial.println("❌ CRITICAL: Package 1 failed - skipping Package 2 to prevent USB corruption");
    }
    
    return (success1 && success2);
}

bool USBControlPad::sendApplyCommand() {
    uint8_t cmd[64] = {
        0x41, 0x80  // Apply/confirm command
        // Rest filled with zeros
    };
    
    // Serial.println("🎨 Sending LED apply command with verification");
    return sendCommandWithVerification(cmd, 64, 0x41, 0x80);
}

bool USBControlPad::sendFinalizeCommand() {
    uint8_t cmd[64] = {
        0x51, 0x28, 0x00, 0x00,
        0xff, 0x00  // Finalize pattern
        // Rest filled with zeros
    };
    
    // Serial.println("🎨 Sending LED finalize command");
    return sendCommand(cmd, 64);
}

bool USBControlPad::updateAllLEDs(const ControlPadColor* colors, size_t count) {
    if (!initialized || count > 24) {
        // Serial.println("❌ Cannot update LEDs: not initialized or too many LEDs");
        return false;
    }
    
    // ULTRA-FAST 3-COMMAND SEQUENCE - Minimizes USB traffic and flicker
    // 1. Custom Mode
    // 2. Combined LED Packages (Package1 + Package2 back-to-back)  
    // 3. Apply + Finalize back-to-back
    
    bool success = true;
    
    // Command 1: Set custom mode (done once during initialization)
    // success &= setCustomMode();
    
          // Command 2: Send both LED packages consecutively 
      success &= sendLEDPackages(colors);
      
      // Small delay before apply command to prevent USB command conflicts during fast button transitions
      //delay(3);
      
      // Command 3: Apply command
      success &= sendApplyCommand();
    
    // Command 4: Finalize command  
    success &= sendFinalizeCommand();
    
    // if (success) {
    //     Serial.println("✅ Ultra-fast LED update sequence completed");
    // } else {
    //     Serial.println("❌ Ultra-fast LED update sequence failed");
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
    
    // 2. Start USB host
    usbHost.begin();
    Serial.println("🔌 USB Host started, waiting for device enumeration...");
    
    // 3. Wait for device enumeration and driver creation (longer delay)
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
    
    // 4. Wait additional time for USB configuration to stabilize
    Serial.println("⏳ Waiting for USB configuration to stabilize...");
    delay(2000);
    
    // 5. Now initialize the driver (this starts polling)
    if (controlPadDriver) {
        Serial.println("🎯 Starting driver initialization...");
        bool initResult = controlPadDriver->begin(controlpad_queue);
        if (!initResult) {
            Serial.println("❌ Driver initialization failed");
            return false;
        }
        
        Serial.printf("✅ Driver initialized! initialized=%s\n", 
                     controlPadDriver->initialized ? "true" : "false");
        
        // 6. Wait for polling to stabilize
        delay(1000);
        
        // 7. Send activation sequence
        Serial.println("🚀 Sending activation sequence...");
        controlPadDriver->sendUSBInterfaceReSponseActivation();
    } else {
        Serial.println("❌ controlPadDriver is null - USB device not found");
        return false;
    }
    
    // 8. ControlPad setup is handled by the caller (main.cpp)
    // No need to call pad.begin() here as it would create circular recursion
    
    return true;
}

void ControlPadHardware::poll() {
    // Events are now processed immediately in USB callbacks
    // This method is kept for compatibility but doesn't need to do anything
    // The USB driver handles everything automatically
    
    // Optional: Just verify the driver is still running
    if (controlPadDriver && controlPadDriver->initialized) {
        // USB driver is running, events are being processed in callbacks
        return;
    } else {
        Serial.println("🔧 ControlPadHardware::poll() - Driver not available");
    }
}

void ControlPadHardware::setAllLeds(const ControlPadColor* colors, size_t count) {
    // Send the full LED state to the hardware using the new 5-command sequence
    // Serial.printf("🎨 Hardware: Setting %zu LEDs\n", count);
    
    // Use the USB driver to send the LED update
    if (controlPadDriver) {
        bool success = controlPadDriver->updateAllLEDs(colors, count);
        // if (success) {
        //     Serial.println("✅ LED update completed via USB driver");
        // } else {
        //     Serial.println("❌ LED update failed via USB driver");
        // }
    } else {
        // Serial.println("❌ No USB driver available for LED update");
    }
}

void ControlPadHardware::setFastMode(bool enabled) {
    if (controlPadDriver) {
        controlPadDriver->setFastMode(enabled);
    }
}