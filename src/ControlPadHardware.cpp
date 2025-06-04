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
    
    // Create driver instance only once
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
    Serial.println("🔄 Starting QUAD INTERFACE polling...");
    
    // Start polling Interface 0 (Keyboard - 8 byte packets)
    Serial.printf("📡 Starting keyboard polling on EP 0x%02X...\n", kbd_ep_in);
    if (InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb) == 0) {
        kbd_polling = true;
        Serial.println("✅ Keyboard polling started successfully");
    } else {
        Serial.println("❌ Failed to start keyboard polling");
    }
    
    delay(10);
    
    // Start polling Interface 1 Control (64 byte packets) - MOST IMPORTANT
    Serial.printf("📡 Starting control polling on EP 0x%02X...\n", ctrl_ep_in);
    if (InterruptMessage(ctrl_ep_in, 64, ctrl_report, &ctrl_poll_cb) == 0) {
        ctrl_polling = true;
        Serial.println("✅ Control polling started successfully");
    } else {
        Serial.println("❌ Failed to start control polling");
    }
    
    delay(10);
    
    // Start polling Interface 2 Dual (64 byte packets)
    Serial.printf("📡 Starting dual polling on EP 0x%02X...\n", dual_ep_in);
    if (InterruptMessage(dual_ep_in, 64, dual_report, &dual_poll_cb) == 0) {
        dual_polling = true;
        Serial.println("✅ Dual polling started successfully");
    } else {
        Serial.println("❌ Failed to start dual polling");
    }
    
    delay(10);
    
    // Start polling Interface 3 Button (32 byte packets)
    Serial.printf("📡 Starting hall sensor polling on EP 0x%02X...\n", hall_sensor_ep_in);
    if (InterruptMessage(hall_sensor_ep_in, 32, hall_sensor_report, &hall_sensor_poll_cb) == 0) {
        hall_sensor_polling = true;
        Serial.println("✅ Hall sensor polling started successfully");
    } else {
        Serial.println("❌ Failed to start hall sensor polling");
    }
    
    delay(10);
    
    // Start polling Interface 3 Button Output (32 byte packets)
    Serial.printf("📡 Starting button output polling on EP 0x%02X...\n", btn_ep_out);
    if (InterruptMessage(btn_ep_out, 32, sensor_out_report, &sensor_out_poll_cb) == 0) {
        sensor_out_polling = true;
        Serial.println("✅ Button output polling started successfully");
    } else {
        Serial.println("❌ Failed to start button output polling");
    }
    
    delay(50);
    
    // Report polling status
    Serial.printf("📊 Polling Status: kbd=%s, ctrl=%s, dual=%s, btn=%s, sensor_out=%s\n",
                  kbd_polling ? "✅" : "❌",
                  ctrl_polling ? "✅" : "❌", 
                  dual_polling ? "✅" : "❌",
                  hall_sensor_polling ? "✅" : "❌",
                  sensor_out_polling ? "✅" : "❌");
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
    delay(2000); // Let devices enumerate
    
    // 3. If driver was created by USB factory, start it
    if (controlPadDriver) {
        controlPadDriver->begin(controlpad_queue);
        delay(500);
        controlPadDriver->sendUSBInterfaceReSponseActivation();
    }
    
    // 4. Call ControlPad's begin to finish setup
    pad.begin();
    
    return true;
}

void ControlPadHardware::poll() {
    // Poll hardware, handle events, etc.
    // This is optional - the USB callbacks handle most polling
}

void ControlPadHardware::setAllLeds(const ControlPadColor* colors, size_t count) {
    // Send the full LED state to the hardware
    // This is a placeholder - you'll need to implement the actual LED protocol
    // based on the working code from your monolithic file
    
    Serial.printf("🎨 Hardware: Setting %zu LEDs\n", count);
    
    // Example: pack all colors into a buffer and send via USB
    uint8_t ledPacket[75]; // 25 buttons * 3 bytes each
    ledPacket[0] = 0; // Avoid "set but not used" warning
    
    for (size_t i = 0; i < count && i < 25; ++i) {
        // This would be where you pack the LED data according to your protocol
        // For now, just log what we would do
        Serial.printf("  LED %zu: RGB(%d,%d,%d)\n", i, colors[i].r, colors[i].g, colors[i].b);
    }
    
    // TODO: Implement actual LED sending via controlPadDriver
    // if (controlPadDriver) {
    //     controlPadDriver->sendLEDData(ledPacket, sizeof(ledPacket));
    // }
}