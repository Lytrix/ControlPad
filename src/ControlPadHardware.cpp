#include "ControlPadHardware.h"
#include <teensy4_usbhost.h>

// Global USB host instance
static DMAMEM TeensyUSBHost2 usbHost;

// Global driver instance pointer (defined here, declared in header as extern)
USBControlPad* controlPadDriver = nullptr;

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
    
    delay(100); // Longer delay between endpoint starts
    
    // Start polling Interface 0 (Keyboard - 8 byte packets) - SECONDARY PRIORITY
    Serial.printf("📡 Starting keyboard polling on EP 0x%02X...\n", kbd_ep_in);
    if (InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb) == 0) {
        kbd_polling = true;
        Serial.println("✅ Keyboard polling started successfully");
    } else {
        Serial.println("⚠️ Keyboard polling failed - continuing without it");
    }
    
    delay(100);
    
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
            
            if (state == 0xC0) {
                Serial.printf("🔴 Button pressed: ID=0x%02X\n", buttonId);
            } else if (state == 0x40) {
                Serial.printf("🔘 Button released: ID=0x%02X\n", buttonId);
            }
        }
        
        // Queue the control event
        controlpad_event event;
        result = min(result, 64);
        memcpy(event.data, ctrl_report, result);
        event.len = result;
        atomQueuePut(queue, 0, &event);
    }
    
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
            Serial.printf("📤 Command #%d sent successfully\n", commandCounter);
        }
    } else {
        Serial.printf("❌ Command #%d FAILED: %d\n", commandCounter, result);
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
    
    // Send the command
    int result = InterruptMessage(ctrl_ep_out, 64, commands[step-1], &send_cb);
    delay(50);  // Small delay between commands
    
    return (result == 0);
}

// ===== LED CONTROL COMMANDS =====

bool USBControlPad::sendCommand(const uint8_t* data, size_t length) {
    if (!initialized || length > 64) {
        return false;
    }
    
    // Send via interrupt transfer to control endpoint
    int result = InterruptMessage(ctrl_ep_out, length, const_cast<uint8_t*>(data), &send_cb);
    delay(10); // Small delay between commands
    
    return (result == 0);
}

bool USBControlPad::setCustomMode() {
    uint8_t cmd[64] = {
        0x56, 0x81, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0xbb, 0xbb, 0xbb, 0xbb  // Custom mode pattern
        // Rest filled with zeros
    };
    
    Serial.println("🎨 Setting custom LED mode");
    return sendCommand(cmd, 64);
}

bool USBControlPad::setStaticMode() {
    uint8_t cmd[64] = {
        0x56, 0x81, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x55, 0x55, 0x55, 0x55  // Static mode pattern
        // Rest filled with zeros
    };
    
    Serial.println("🎨 Setting static LED mode");
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendLEDPackage1(const ControlPadColor* colors) {
    uint8_t cmd[64] = {
        0x56, 0x83, 0x00, 0x00,    // Header
        0x01, 0x00, 0x00, 0x00,    // Package 1 marker
        0x80, 0x01, 0x00, 0x00,    // Control flags
        0xff, 0x00, 0x00, 0x00,    // Color flags
        0x00, 0x00,                // Reserved
        0xff, 0xff,                // Brightness for all LEDs
        0x00, 0x00, 0x00, 0x00     // Reserved
    };
    
    // LED data starts at position 24
    size_t pos = 24;
    
    // Column 1: buttons 1,6,11,16,21 (indices 0,5,10,15,20)
    for (int i = 0; i < 5 && pos + 2 < 64; i++) {
        int btnIdx = i * 5;  // 0,5,10,15,20
        cmd[pos++] = colors[btnIdx].r;
        cmd[pos++] = colors[btnIdx].g;
        cmd[pos++] = colors[btnIdx].b;
    }
    
    // Column 2: buttons 2,7,12,17,22 (indices 1,6,11,16,21)
    for (int i = 0; i < 5 && pos + 2 < 64; i++) {
        int btnIdx = 1 + i * 5;  // 1,6,11,16,21
        cmd[pos++] = colors[btnIdx].r;
        cmd[pos++] = colors[btnIdx].g;
        cmd[pos++] = colors[btnIdx].b;
    }
    
    // Column 3: buttons 3,8,13 (indices 2,7,12) + partial button 18 (index 17)
    for (int i = 0; i < 3 && pos + 2 < 64; i++) {
        int btnIdx = 2 + i * 5;  // 2,7,12
        cmd[pos++] = colors[btnIdx].r;
        cmd[pos++] = colors[btnIdx].g;
        cmd[pos++] = colors[btnIdx].b;
    }
    
    // Button 18 (index 17) - only R component fits in package 1
    if (pos < 64) {
        cmd[pos++] = colors[17].r;  // Button 18 R
    }
    
    Serial.println("🎨 Sending LED Package 1");
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendLEDPackage2(const ControlPadColor* colors) {
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
    
    // Column 4: buttons 4,9,14,19,24 (indices 3,8,13,18,23)
    for (int i = 0; i < 5 && pos + 2 < 64; i++) {
        int btnIdx = 3 + i * 5;  // 3,8,13,18,23
        cmd[pos++] = colors[btnIdx].r;
        cmd[pos++] = colors[btnIdx].g;
        cmd[pos++] = colors[btnIdx].b;
    }
    
    // Column 5: buttons 5,10,15,20 (indices 4,9,14,19) - button 24 is double width
    for (int i = 0; i < 4 && pos + 2 < 64; i++) {
        int btnIdx = 4 + i * 5;  // 4,9,14,19
        cmd[pos++] = colors[btnIdx].r;
        cmd[pos++] = colors[btnIdx].g;
        cmd[pos++] = colors[btnIdx].b;
    }
    
    // Rest filled with zeros (already initialized)
    
    Serial.println("🎨 Sending LED Package 2");
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendApplyCommand() {
    uint8_t cmd[64] = {
        0x41, 0x80  // Apply/confirm command
        // Rest filled with zeros
    };
    
    Serial.println("🎨 Sending LED apply command");
    return sendCommand(cmd, 64);
}

bool USBControlPad::sendFinalizeCommand() {
    uint8_t cmd[64] = {
        0x51, 0x28, 0x00, 0x00,
        0xff, 0x00  // Finalize pattern
        // Rest filled with zeros
    };
    
    Serial.println("🎨 Sending LED finalize command");
    return sendCommand(cmd, 64);
}

bool USBControlPad::updateAllLEDs(const ControlPadColor* colors, size_t count) {
    if (!initialized || count > 24) {
        Serial.println("❌ Cannot update LEDs: not initialized or too many LEDs");
        return false;
    }
    
    Serial.printf("🌈 Updating all %zu LEDs with 5-command sequence\n", count);
    
    // Ensure we have 24 colors (pad with black if necessary)
    ControlPadColor fullColors[24] = {};
    for (size_t i = 0; i < count && i < 24; i++) {
        fullColors[i] = colors[i];
    }
    
    // Execute the 5-command sequence
    bool success = true;
    
    success &= setCustomMode();
    delay(12);
    
    success &= sendLEDPackage1(fullColors);
    delay(11);
    
    success &= sendLEDPackage2(fullColors);
    delay(12);
    
    success &= sendApplyCommand();
    delay(9);
    
    success &= sendFinalizeCommand();
    delay(10);
    
    if (success) {
        Serial.println("✅ LED update sequence completed successfully");
    } else {
        Serial.println("❌ LED update sequence failed");
    }
    
    return success;
}

// ===== HARDWARE MANAGER IMPLEMENTATION =====

ControlPadHardware::ControlPadHardware() {
    // Constructor - initialize any hardware-specific settings
}

ControlPadHardware::~ControlPadHardware() {
    if (controlpad_queue_data) {
        free(controlpad_queue_data);
    }
}

bool ControlPadHardware::begin(ControlPad& pad) {
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
    // Poll hardware, handle events, etc.
    // This is optional - the USB callbacks handle most polling
}

void ControlPadHardware::setAllLeds(const ControlPadColor* colors, size_t count) {
    // Send the full LED state to the hardware using the new 5-command sequence
    Serial.printf("🎨 Hardware: Setting %zu LEDs\n", count);
    
    // Use the USB driver to send the LED update
    if (controlPadDriver) {
        bool success = controlPadDriver->updateAllLEDs(colors, count);
        if (success) {
            Serial.println("✅ LED update completed via USB driver");
        } else {
            Serial.println("❌ LED update failed via USB driver");
        }
    } else {
        Serial.println("❌ No USB driver available for LED update");
    }
}