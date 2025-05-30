#include <Arduino.h>
#include <teensy4_usbhost.h>

// ===== CONTROLPAD CONSTANTS =====
#define CONTROLPAD_VID  0x2516
#define CONTROLPAD_PID  0x012D

// USB endpoints from packet analysis
#define EP_OUT          0x04  // Interrupt OUT endpoint for commands
#define EP_IN           0x83  // Interrupt IN endpoint for responses

// ===== CONTROLPAD PROTOCOL STRUCTURES =====
struct controlpad_event {
  uint8_t data[64];
  uint8_t len;
};

struct ControlPadPacket {
  uint8_t vendor_id = 0x56;    // Correct vendor ID from USB capture
  uint8_t cmd1;                // Command byte 1 (e.g., 0x83 for LED)
  uint8_t cmd2;                // Command byte 2 (LED index)
  uint8_t data[61] = {0};      // Remaining 61 bytes (total 64 bytes)
};

// ===== GLOBAL VARIABLES =====
static DMAMEM TeensyUSBHost2 usbHost;
ATOM_QUEUE controlpad_queue;
controlpad_event controlpad_queue_data[8];

// Forward declaration for the global driver instance
class USBControlPad;
USBControlPad* controlPadDriver = nullptr;

// ===== CORRECTED CONTROLPAD DRIVER =====
// This fixes the USB_Driver_FactoryGlue template usage with proper static methods

class USBControlPad : public USB_Driver_FactoryGlue<USBControlPad> {
private:
  uint8_t interface = 0;  // Primary interface (will handle both)
  
  // Interface 0 (Keyboard) endpoints and data
  uint8_t kbd_ep_in = 0x81;   // Standard keyboard endpoint
  uint8_t kbd_report[8] __attribute__((aligned(32)));
  
  // Interface 1 (Control/LED) endpoints and data  
  uint8_t ctrl_ep_in = 0x83;   // Control input endpoint
  uint8_t ctrl_ep_out = 0x04;  // Control output endpoint
  uint8_t ctrl_report[64] __attribute__((aligned(32)));
  
  uint8_t report_len = 64;
  bool initialized = false;
  ATOM_QUEUE* queue = nullptr;
  
  // Separate callbacks for different interfaces
  USBCallback kbd_poll_cb;
  USBCallback ctrl_poll_cb;
  USBCallback send_cb;

public:
  // Static initialization test
  static bool factory_registered;
  
  // Static tracking to prevent multiple driver instances
  static bool driver_instance_created;
  
  // Make polling state accessible for monitoring
  bool kbd_polling = false;
  bool ctrl_polling = false;
  
  // Constructor for USB_Driver_FactoryGlue (requires USB_Device*)
  USBControlPad(USB_Device* dev) : USB_Driver_FactoryGlue<USBControlPad>(dev), 
                                   kbd_poll_cb([this](int r) { kbd_poll(r); }),
                                   ctrl_poll_cb([this](int r) { ctrl_poll(r); }),
                                   send_cb([this](int r) { sent(r); }) {
    Serial.println("🔧 USBControlPad DUAL INTERFACE driver instance created");
    factory_registered = true;
  }
  
  // USB_Driver_FactoryGlue REQUIRED STATIC METHODS
  // These are called by the factory system during device enumeration
  
  static bool offer_interface(const usb_interface_descriptor* iface, size_t length) {
    Serial.printf("\n🔍 *** USBControlPad::offer_interface called ***\n");
    Serial.printf("   Interface: %d\n", iface->bInterfaceNumber);
    Serial.printf("   Class: 0x%02X\n", iface->bInterfaceClass);
    Serial.printf("   SubClass: 0x%02X\n", iface->bInterfaceSubClass);
    Serial.printf("   Protocol: 0x%02X\n", iface->bInterfaceProtocol);
    Serial.printf("   NumEndpoints: %d\n", iface->bNumEndpoints);
    Serial.printf("   AltSetting: %d\n", iface->bAlternateSetting);
    
    // Debug endpoint information
    const uint8_t *desc = (const uint8_t*)iface;
    const uint8_t *end = desc + length;
    desc += desc[0]; // Skip interface descriptor
    
    Serial.println("   Endpoints:");
    bool hasInEndpoint = false;
    bool hasOutEndpoint = false;
    
    while (desc < end - 2) {
      if (desc[1] == USB_DT_ENDPOINT) {
        const usb_endpoint_descriptor *ep = (const usb_endpoint_descriptor*)desc;
        Serial.printf("     - EP 0x%02X: type=0x%02X, maxPacket=%d\n", 
                      ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize);
        
        if ((ep->bmAttributes & 0x03) == USB_ENDPOINT_INTERRUPT) {
          if (ep->bEndpointAddress & 0x80) {
            hasInEndpoint = true;
          } else {
            hasOutEndpoint = true;
          }
        }
      }
      desc += desc[0];
    }
    
    // ACCEPT BOTH Interface 0 (input) AND Interface 1 (LED control)
    // Only reject Interface 2 (unknown)
    if (iface->bInterfaceClass == 0x03 && 
        (iface->bInterfaceNumber == 0 || iface->bInterfaceNumber == 1)) {
      Serial.printf("✅ USBControlPad ACCEPTING Interface %d for full control!\n\n", iface->bInterfaceNumber);
      return true;
    }
    
    Serial.printf("❌ USBControlPad rejecting interface %d ", iface->bInterfaceNumber);
    if (iface->bInterfaceNumber == 2) {
      Serial.println("(Interface 2 not needed)");
    } else {
      Serial.println("(not HID or wrong interface)");
    }
    Serial.println();
    
    return false;
  }
  
  static USB_Driver* attach_interface(const usb_interface_descriptor* iface, size_t length, USB_Device* dev) {
    Serial.printf("\n🎯 *** USBControlPad::attach_interface called ***\n");
    Serial.printf("   Attaching to Interface: %d\n", iface->bInterfaceNumber);
    Serial.printf("   Class: 0x%02X, SubClass: 0x%02X, Protocol: 0x%02X\n", 
                  iface->bInterfaceClass, iface->bInterfaceSubClass, iface->bInterfaceProtocol);
    
    // Create driver instance on FIRST interface encountered (Interface 0 or 1)
    if (!driver_instance_created) {
      // Create a new driver instance
      USBControlPad* driver = new USBControlPad(dev);
      driver->interface = iface->bInterfaceNumber;
      
      // Configure for dual interface operation
      driver->setupDualInterface();
      
      // Store the instance globally so our main loop can access it
      controlPadDriver = driver;
      
      // Start the driver with the global queue
      driver->begin(&controlpad_queue);
      
      driver_instance_created = true;
      Serial.printf("✅ USBControlPad DUAL INTERFACE driver created (primary: Interface %d)!\n\n", iface->bInterfaceNumber);
      return driver;
    } else {
      // Second interface: Don't create a new driver, just acknowledge
      Serial.printf("✅ Interface %d acknowledged (using existing dual-interface driver)!\n\n", iface->bInterfaceNumber);
      return nullptr;  // Don't create a second driver instance
    }
  }
  
  // Instance methods
  void detach() override {
    Serial.println("❌ USBControlPad detached");
    initialized = false;
    kbd_polling = false;
    ctrl_polling = false;
  }
  
  void setupDualInterface() {
    Serial.println("🔧 Setting up dual interface operation...");
    // Set fixed endpoints based on USB capture analysis
    kbd_ep_in = 0x81;    // Interface 0 keyboard input
    ctrl_ep_in = 0x83;   // Interface 1 control input  
    ctrl_ep_out = 0x04;  // Interface 1 control output
    Serial.printf("✅ Keyboard EP: 0x%02X, Control EP IN: 0x%02X, OUT: 0x%02X\n", 
                  kbd_ep_in, ctrl_ep_in, ctrl_ep_out);
  }
  
  bool begin(ATOM_QUEUE *q) {
    queue = q;
    if (queue != nullptr) {
      initializeDevice();
      startDualPolling();
    }
    return true;
  }
  
  void findEndpoints(const usb_interface_descriptor* iface, size_t length) {
    Serial.printf("🔍 Finding endpoints in interface %d...\n", iface->bInterfaceNumber);
    
    const uint8_t *desc = (const uint8_t*)iface;
    const uint8_t *end = desc + length;
    
    // Skip interface descriptor
    desc += desc[0];
    
    while (desc < end - 2) {
      if (desc[1] == USB_DT_ENDPOINT) {
        const usb_endpoint_descriptor *ep = (const usb_endpoint_descriptor*)desc;
        Serial.printf("📍 Found endpoint: 0x%02X, type: 0x%02X, maxPacket: %d\n", 
                      ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize);
        
        if ((ep->bmAttributes & 0x03) == USB_ENDPOINT_INTERRUPT) {
          if (ep->bEndpointAddress & 0x80) {
            kbd_ep_in = ep->bEndpointAddress;
            Serial.printf("✅ Set EP_IN: 0x%02X\n", kbd_ep_in);
          } else {
            ctrl_ep_out = ep->bEndpointAddress;
            Serial.printf("✅ Set EP_OUT: 0x%02X\n", ctrl_ep_out);
          }
        }
      }
      desc += desc[0];
    }
  }
  
  bool sendCompleteRedSequence() {
    Serial.println("🔥 Sending COMPLETE sequence from USB capture to set button 1 red");
    
    // Based on the capture, I see several commands. Let me send the key ones:
    
    // First command: 568100000100000002000000bbbbbbbb...
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Second command: 568300000100000080010000ff0000000000ffff00000000ff000000...
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Third command: 56830100000000000000000000...
    uint8_t cmd3[64] = {
      0x56, 0x83, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Fourth command: 41800000000000000000...
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Fifth command: 51280000ff0000000000...
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    Serial.println("🔄 Step 1: Setup command (56 81...)");
    int result1 = InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb);
    delay(50);
    
    Serial.println("🔄 Step 2: Main LED command (56 83 00...)");
    int result2 = InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb);
    delay(50);
    
    Serial.println("🔄 Step 3: LED index command (56 83 01...)");
    int result3 = InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb);
    delay(50);
    
    Serial.println("🔄 Step 4: Mode command (41 80...)");
    int result4 = InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb);
    delay(50);
    
    Serial.println("🔄 Step 5: Final red command (51 28...)");
    int result5 = InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    Serial.printf("📊 Results: %d %d %d %d %d\n", result1, result2, result3, result4, result5);
    
    return (result1 == 0 && result2 == 0 && result3 == 0 && result4 == 0 && result5 == 0);
  }

  bool sendExactLEDCommand() {
    // Try the complete sequence first
    if (sendCompleteRedSequence()) {
      Serial.println("✅ Complete sequence sent successfully!");
      return true;
    }
    
    // Fallback to single command
    // Copy the CORRECT byte pattern from the new USB capture for setting button 1 to red
    // From: host to 1.8.4 568300000100000080010000ff0000000000ffff00000000ff000000000000000000000000000000000000000000000000000000000000000000000000000000
    
    uint8_t exactPattern[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    Serial.println("📤 Sending CORRECT pattern from new USB capture for button 1 RED");
    Serial.print("   Pattern: ");
    for (int i = 0; i < 16; i++) {  // Show first 16 bytes
      Serial.printf("%02X ", exactPattern[i]);
    }
    Serial.println("...");
    
    // Send exactly as captured
    int result = InterruptMessage(ctrl_ep_out, 64, exactPattern, &send_cb);
    if (result == 0) {
      Serial.println("✅ CORRECT pattern sent successfully!");
      return true;
    } else {
      Serial.printf("❌ CORRECT pattern failed: %d\n", result);
      return false;
    }
  }

  bool sendLEDCommand(uint8_t ledIndex, uint8_t r, uint8_t g, uint8_t b) {
    // For now, if trying to set LED 1 to red, use the exact pattern
    if (ledIndex == 1 && r == 255 && g == 0 && b == 0) {
      Serial.println("🔴 Using EXACT red pattern for button 1");
      return sendExactLEDCommand();
    }
    
    ControlPadPacket packet;
    packet.cmd1 = 0x83;  // LED command from USB capture
    packet.cmd2 = ledIndex;  // LED index (1-25)
    
    // Based on USB capture: 56 83 01 00 ff 00 ...
    packet.data[0] = 0x00;  // Sub-command
    packet.data[1] = r;     // Red value
    packet.data[2] = g;     // Green value
    packet.data[3] = b;     // Blue value (if supported)
    
    // Fill in some pattern observed in the capture
    packet.data[4] = 0xD7;
    packet.data[5] = 0x52;
    packet.data[6] = 0xFF;
    packet.data[7] = 0x00;
    
    Serial.printf("📤 Sending NEW LED command: 0x%02X 0x%02X 0x%02X [%02X %02X %02X] to EP 0x%02X\n", 
                  packet.vendor_id, packet.cmd1, packet.cmd2, r, g, b, ctrl_ep_out);
    
    // Add retry logic for LED commands
    int maxRetries = 3;
    for (int attempt = 0; attempt < maxRetries; attempt++) {
      // Send via interrupt transfer to the LED control OUT endpoint
      int result = InterruptMessage(ctrl_ep_out, 64, &packet, &send_cb);
      if (result == 0) {
        if (attempt > 0) {
          Serial.printf("✅ NEW LED Command succeeded on attempt %d\n", attempt + 1);
        } else {
          Serial.println("✅ NEW LED Command queued successfully");
        }
        return true;
      } else {
        Serial.printf("⚠️ NEW LED Command attempt %d failed: %d\n", attempt + 1, result);
        if (attempt < maxRetries - 1) {
          delay(20);  // Wait before retry
        }
      }
    }
    
    Serial.printf("❌ NEW LED Command failed after %d attempts\n", maxRetries);
    return false;
  }
  
  bool sendCommand(uint8_t cmd1, uint8_t cmd2, uint8_t* extraData = nullptr, size_t extraLen = 0) {
    ControlPadPacket packet;
    packet.vendor_id = 0x52;  // Keep old protocol for non-LED commands
    packet.cmd1 = cmd1;
    packet.cmd2 = cmd2;
    
    // Copy any extra data
    if (extraData && extraLen > 0) {
      size_t copyLen = (extraLen > 61) ? 61 : extraLen;
      memcpy(packet.data, extraData, copyLen);
    }
    
    // LED commands go to Interface 1 control OUT endpoint
    Serial.printf("📤 Sending OLD command: 0x52 0x%02X 0x%02X to EP 0x%02X\n", cmd1, cmd2, ctrl_ep_out);
    
    // Add retry logic for LED commands
    int maxRetries = 3;
    for (int attempt = 0; attempt < maxRetries; attempt++) {
      // Send via interrupt transfer to the LED control OUT endpoint
      int result = InterruptMessage(ctrl_ep_out, 64, &packet, &send_cb);
      if (result == 0) {
        if (attempt > 0) {
          Serial.printf("✅ OLD Command succeeded on attempt %d\n", attempt + 1);
        } else {
          Serial.println("✅ OLD Command queued successfully");
        }
        return true;
      } else {
        Serial.printf("⚠️ OLD Command attempt %d failed: %d\n", attempt + 1, result);
        if (attempt < maxRetries - 1) {
          delay(20);  // Wait before retry
        }
      }
    }
    
    Serial.printf("❌ OLD Command failed after %d attempts\n", maxRetries);
    return false;
  }
  
  bool checkDeviceHealth() {
    Serial.println("🔍 Checking device health...");
    
    // Check if we have valid endpoints
    if (ctrl_ep_out == 0) {
      Serial.println("❌ Control OUT endpoint not set!");
      return false;
    }
    
    if (ctrl_ep_in == 0) {
      Serial.println("❌ Control IN endpoint not set!");
      return false;
    }
    
    // Check polling status
    if (!kbd_polling && !ctrl_polling) {
      Serial.println("❌ No polling active!");
      return false;
    }
    
    Serial.printf("✅ Device appears healthy:\n");
    Serial.printf("   - Control OUT EP: 0x%02X\n", ctrl_ep_out);
    Serial.printf("   - Control IN EP: 0x%02X\n", ctrl_ep_in);
    Serial.printf("   - Keyboard polling: %s\n", kbd_polling ? "ACTIVE" : "INACTIVE");
    Serial.printf("   - Control polling: %s\n", ctrl_polling ? "ACTIVE" : "INACTIVE");
    Serial.printf("   - Initialized: %s\n", initialized ? "YES" : "NO");
    
    return true;
  }
  
  bool setLEDs(uint8_t r, uint8_t g, uint8_t b) {
    Serial.printf("🎯 Setting LED color using NEW PROTOCOL: R=%d G=%d B=%d\n", r, g, b);
    
    // Check device health first
    if (!checkDeviceHealth()) {
      Serial.println("❌ Device health check failed, aborting LED operation");
      return false;
    }
    
    if (!initialized) {
      Serial.println("⚠️ Device not initialized yet, initializing now...");
      initializeDevice();
      delay(500);  // Give initialization time to complete
    }
    
    // Test with NEW protocol - just set button 1 to the specified color
    Serial.println("🔧 Using NEW LED protocol from USB capture...");
    
    if (sendLEDCommand(1, r, g, b)) {
      Serial.println("✅ NEW LED Protocol command sent successfully!");
      return true;
    } else {
      Serial.println("❌ NEW LED Protocol failed, trying old approach...");
      
      // Fallback to old protocol if new one fails
      Serial.println("📡 Fallback: Using old LED protocol...");
      
      // STEP 1: Switch to custom mode (if not already done)
      if (!sendCommand(0x1E, 0x00)) {  // Disable effects
        Serial.println("❌ Failed to disable effects, continuing anyway...");
      }
      delay(100);
      
      // STEP 2: Set custom mode with proper timing
      uint8_t customModeData[] = {0x01, 0x00, 0x00, 0x00};
      if (!sendCommand(0x1C, 0x01, customModeData, 4)) {
        Serial.println("❌ Failed to set custom mode, continuing anyway...");
      }
      delay(200);  // Longer delay for mode switch
      
      // STEP 3: Try setting just one LED with old protocol
      uint8_t colorData[] = {r, g, b, 0xFF};
      if (sendCommand(0x18, 1, colorData, 4)) {
        Serial.println("✅ Old protocol LED command sent");
        // Apply changes
        delay(100);
        if (sendCommand(0x1F, 0x01)) {
          Serial.println("✅ Old protocol LED changes applied");
          return true;
        }
      }
      
      return false;
    }
  }
  
  bool initializeDevice() {
    Serial.println("\n🔥 === ENHANCED CONTROLPAD INITIALIZATION === 🔥");
    
    // Extended initialization based on USB captures
    Serial.println("📡 Phase 1: Device Reset & Mode Setup");
    sendCommand(0x00, 0x00);  // Reset
    delay(100);
    
    sendCommand(0x01, 0x03);  // Device configuration
    delay(50);
    
    sendCommand(0x28, 0x00);  // Mode setup
    delay(50);
    
    uint8_t modeData[] = {0x04, 0x00, 0x00};
    sendCommand(0x28, 0x00, modeData, 3);  // Enhanced mode setup
    delay(100);
    
    // Phase 2: Complete key mapping sequence
    Serial.println("📡 Phase 2: Complete Key Mapping");
    uint8_t keySequence[][2] = {
      {0x20, 0x35}, {0x20, 0x1E}, {0x20, 0x1F}, {0x20, 0x20}, {0x20, 0x21}, {0x20, 0x22},
      {0x20, 0x14}, {0x20, 0x1A}, {0x20, 0x08}, {0x20, 0x15}, {0x20, 0x17}, 
      {0x20, 0x04}, {0x20, 0x16}, {0x20, 0x07}, {0x20, 0x09}, {0x20, 0x0A},
      {0x20, 0x1D}, {0x20, 0x1B}, {0x20, 0x06}, {0x20, 0x19}, {0x20, 0x05}
    };
    
    for (size_t i = 0; i < sizeof(keySequence) / sizeof(keySequence[0]); i++) {
      sendCommand(keySequence[i][0], keySequence[i][1]);
      delay(20);
    }
    
    // Phase 3: Enhanced LED initialization 
    Serial.println("📡 Phase 3: Enhanced LED Initialization");
    
    // Global LED setup
    sendCommand(0x18, 0x00);
    delay(50);
    uint8_t globalLED[] = {0xFF, 0x3F, 0x00, 0x00};
    sendCommand(0x18, 0x00, globalLED, 4);
    delay(50);
    
    // Initialize all LED positions
    for (int led = 1; led <= 25; led++) {
      sendCommand(0x18, led);
      delay(10);
      
      uint8_t ledInit[] = {0xFF, 0x3F, 0x00, 0x00};
      sendCommand(0x18, led, ledInit, 4);
      delay(10);
    }
    
    // Final LED mode setup
    uint8_t finalMode[] = {0x01, 0x00, 0x00, 0xFF};
    sendCommand(0x1C, 0x01, finalMode, 4);
    delay(100);
    
    initialized = true;
    Serial.println("✅ Enhanced ControlPad initialization complete!");
    
    // Test with a quick color flash
    Serial.println("🌈 Testing with rainbow flash...");
    setLEDs(255, 0, 0);    // Red
    delay(500);
    setLEDs(0, 255, 0);    // Green  
    delay(500);
    setLEDs(0, 0, 255);    // Blue
    delay(500);
    setLEDs(0, 0, 0);      // Off
    
    return true;
  }
  
  void kbd_poll(int result) {
    static int kbd_counter = 0;
    
    if (result > 0 && queue) {
      kbd_counter++;
      
      // Debug: Show what's actually in the keyboard packet
      if (kbd_counter % 50 == 1) {  // Only print occasionally to avoid spam
        Serial.printf("⌨️ Keyboard poll #%d (8 bytes): ", kbd_counter);
        for (int i = 0; i < 8; i++) {
          Serial.printf("0x%02X ", kbd_report[i]);
        }
        Serial.println();
      }
      
      // Check if there's actually a key press (non-zero data)
      bool hasKeyPress = false;
      for (int i = 0; i < 8; i++) {
        if (kbd_report[i] != 0) {
          hasKeyPress = true;
          break;
        }
      }
      
      if (hasKeyPress) {
        controlpad_event event;
        if (result > 8) result = 8;
        memcpy(&event.data, kbd_report, result);
        event.len = (uint8_t)result;
        atomQueuePut(queue, 0, &event);  // Non-blocking
        
        Serial.printf("🎯 ACTUAL KEY PRESS detected! Packet: ");
        for (int i = 0; i < 8; i++) {
          Serial.printf("0x%02X ", kbd_report[i]);
        }
        Serial.println();
      }
      
      // Restart keyboard polling
      int restart = InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb);
      if (restart != 0) {
        Serial.printf("⚠️ Failed to restart keyboard polling: %d\n", restart);
        kbd_polling = false;
      }
    } else if (result < 0) {
      Serial.printf("⚠️ Keyboard poll failed: %d\n", result);
      kbd_polling = false;
      // Retry after delay in main loop
    }
  }
  
  void ctrl_poll(int result) {
    static int ctrl_counter = 0;
    
    if (result > 0 && queue) {
      ctrl_counter++;
      
      // Only show control poll results occasionally and when there's actual data
      bool hasControlData = false;
      for (int i = 0; i < min(result, 64); i++) {
        if (ctrl_report[i] != 0) {
          hasControlData = true;
          break;
        }
      }
      
      if (hasControlData) {
        controlpad_event event;
        if (result > 64) result = 64;
        memcpy(&event.data, ctrl_report, result);
        event.len = (uint8_t)result;
        atomQueuePut(queue, 0, &event);  // Non-blocking
        
        Serial.printf("🎮 CONTROL Event #%d: ", ctrl_counter);
        for (int i = 0; i < min(8, result); i++) {
          Serial.printf("0x%02X ", ctrl_report[i]);
        }
        Serial.println();
      } else if (ctrl_counter % 100 == 1) {
        // Occasional heartbeat to show control polling is working
        Serial.printf("🎮 Control poll #%d (empty data)\n", ctrl_counter);
      }
      
      // Restart control polling
      int restart = InterruptMessage(ctrl_ep_in, 64, ctrl_report, &ctrl_poll_cb);
      if (restart != 0) {
        Serial.printf("⚠️ Failed to restart control polling: %d\n", restart);
        ctrl_polling = false;
      }
    } else if (result < 0) {
      Serial.printf("⚠️ Control poll failed: %d\n", result);
      ctrl_polling = false;
      // Retry after delay in main loop
    }
  }
  
  void sent(int result) {
    static int commandCounter = 0;
    commandCounter++;
    
    if (result >= 0) {
      // Only show every 10th success to reduce spam, but always show first few
      if (commandCounter <= 5 || commandCounter % 10 == 0) {
        Serial.printf("📤 Command #%d sent successfully (result: %d)\n", commandCounter, result);
      }
    } else {
      // Always show failures with detailed error codes
      Serial.printf("❌ Command #%d FAILED: %d", commandCounter, result);
      
      // Provide more context for common USB error codes
      switch (result) {
        case -1: Serial.println(" (USB_ERROR_TIMEOUT)"); break;
        case -2: Serial.println(" (USB_ERROR_STALL)"); break;
        case -3: Serial.println(" (USB_ERROR_NAK)"); break;
        case -4: Serial.println(" (USB_ERROR_DATA_TOGGLE)"); break;
        case -5: Serial.println(" (USB_ERROR_BABBLE)"); break;
        case -6: Serial.println(" (USB_ERROR_BUFFER_OVERRUN)"); break;
        case -7: Serial.println(" (USB_ERROR_BUFFER_UNDERRUN)"); break;
        case -8: Serial.println(" (USB_ERROR_NOT_ACCESSED)"); break;
        case -9: Serial.println(" (USB_ERROR_FIFO)"); break;
        case -10: Serial.println(" (USB_ERROR_UNKNOWN)"); break;
        default: Serial.println(" (Unknown error code)"); break;
      }
      
      // Suggest possible solutions
      if (result == -1) {
        Serial.println("💡 Timeout suggests device may not be ready for LED commands");
      } else if (result == -2) {
        Serial.println("💡 Stall suggests endpoint or command not supported");
      } else if (result == -3) {
        Serial.println("💡 NAK suggests device is busy, try slowing down commands");
      }
    }
  }
  
  void startDualPolling() {
    Serial.println("🔄 Starting DUAL INTERFACE polling...");
    
    // Start polling Interface 0 (Keyboard - 8 byte packets)
    Serial.printf("📡 Starting keyboard polling on EP 0x%02X...\n", kbd_ep_in);
    int kbd_result = InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb);
    if (kbd_result == 0) {
      kbd_polling = true;
      Serial.println("✅ Keyboard polling started successfully");
    } else {
      Serial.printf("❌ Failed to start keyboard polling: %d\n", kbd_result);
    }
    
    // Start polling Interface 1 (Control - 64 byte packets) 
    Serial.printf("📡 Starting control polling on EP 0x%02X...\n", ctrl_ep_in);
    int ctrl_result = InterruptMessage(ctrl_ep_in, 64, ctrl_report, &ctrl_poll_cb);
    if (ctrl_result == 0) {
      ctrl_polling = true;
      Serial.println("✅ Control polling started successfully");
    } else {
      Serial.printf("❌ Failed to start control polling: %d\n", ctrl_result);
    }
  }
  
  void restartKeyboardPolling() {
    if (!kbd_polling) {
      int result = InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb);
      if (result == 0) {
        kbd_polling = true;
        Serial.println("✅ Keyboard polling restarted");
      } else {
        Serial.printf("❌ Failed to restart keyboard polling: %d\n", result);
      }
    }
  }
  
  void restartControlPolling() {
    if (!ctrl_polling) {
      int result = InterruptMessage(ctrl_ep_in, 64, ctrl_report, &ctrl_poll_cb);
      if (result == 0) {
        ctrl_polling = true;
        Serial.println("✅ Control polling restarted");
      } else {
        Serial.printf("❌ Failed to restart control polling: %d\n", result);
      }
    }
  }
};

// Static variable definition
bool USBControlPad::factory_registered = false;
bool USBControlPad::driver_instance_created = false;

// ===== MAIN SETUP AND LOOP =====

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  
  Serial.println("\n🚀 Teensy4 USB Host ControlPad LED Controller 🚀");
  Serial.println("==================================================");
  Serial.println("🎯 DUAL INTERFACE POLLING - Complete Button Detection");
  Serial.println("✅ Interface 0: Keyboard input (8-byte HID packets)");
  Serial.println("✅ Interface 1: Control data + LED commands (64-byte packets)");
  Serial.println("✅ Simultaneous polling on both endpoints");
  Serial.println("Press keys 1-5 to trigger colored LED responses!");
  
  // Initialize queue for ControlPad events
  atomQueueCreate(&controlpad_queue, (uint8_t*)malloc(10 * sizeof(controlpad_event)), 10, sizeof(controlpad_event));
  
  Serial.println("📋 Queue created successfully");
  
  // Initialize USB Host
  Serial.println("🔌 Starting USB Host...");
  usbHost.begin();
  
  delay(1000);  // Give devices time to enumerate
  
  Serial.println("✅ USB Host initialized successfully");
  Serial.println("🔧 Dual interface driver factory registered");
  Serial.println("📊 Watch for BOTH keyboard AND control events...");
  Serial.println("🎯 Should detect ALL button presses now!");
}

void loop() {
  // Process USB events - use Task from atom library 
  // (no usbHost.Task() needed with teensy4_usbhost)
  
  // Rate limiting for main loop to prevent overwhelming the system
  static unsigned long lastLoopTime = 0;
  if (millis() - lastLoopTime < 20) {  // 50Hz update rate
    return;
  }
  lastLoopTime = millis();
  
  // Check and restart polling if needed
  if (controlPadDriver) {
    static unsigned long lastPollCheck = 0;
    if (millis() - lastPollCheck > 1000) {  // Check every second
      if (!controlPadDriver->kbd_polling) {
        Serial.println("🔄 Restarting keyboard polling...");
        controlPadDriver->restartKeyboardPolling();
      }
      if (!controlPadDriver->ctrl_polling) {
        Serial.println("🔄 Restarting control polling...");
        controlPadDriver->restartControlPolling();
      }
      lastPollCheck = millis();
    }
  }
  
  // Process any pending controlpad events from the queue
  controlpad_event event;
  if (atomQueueGet(&controlpad_queue, 0, &event) == ATOM_OK) {
    // Distinguish between keyboard events (8 bytes) and control events (64 bytes)
    if (event.len == 8) {
      // Standard HID keyboard event from Interface 0
      if (event.data[2] != 0 && event.data[2] < 0x80) {
        uint8_t key = event.data[2];
        Serial.printf("⌨️ KEYBOARD Key Press: 0x%02X (%d) - Interface 0\n", key, key);
        
        // Trigger LED response for ALL 25 key presses
        if (controlPadDriver) {
          // Test the EXACT pattern from USB capture
          Serial.printf("🔥 KEY PRESSED: 0x%02X - Testing EXACT LED pattern!\n", key);
          controlPadDriver->sendExactLEDCommand();
          
          /* OLD rainbow mapping - disabled for testing
          // Create a rainbow mapping for all 25 buttons
          switch(key) {
            // Row 1: 1-5 (Red spectrum)
            case 0x1E: controlPadDriver->setLEDs(255, 0, 0); break;     // Button 1: Red
            case 0x1F: controlPadDriver->setLEDs(255, 64, 0); break;    // Button 2: Red-Orange
            case 0x20: controlPadDriver->setLEDs(255, 128, 0); break;   // Button 3: Orange
            case 0x21: controlPadDriver->setLEDs(255, 192, 0); break;   // Button 4: Yellow-Orange
            case 0x22: controlPadDriver->setLEDs(255, 255, 0); break;   // Button 5: Yellow
            
            // Row 2: QWERT (Green spectrum)
            case 0x14: controlPadDriver->setLEDs(192, 255, 0); break;   // Q: Yellow-Green
            case 0x1A: controlPadDriver->setLEDs(128, 255, 0); break;   // W: Light Green
            case 0x08: controlPadDriver->setLEDs(64, 255, 0); break;    // E: Green
            case 0x15: controlPadDriver->setLEDs(0, 255, 0); break;     // R: Pure Green
            case 0x17: controlPadDriver->setLEDs(0, 255, 64); break;    // T: Green-Cyan
            
            // Row 3: ASDFG (Cyan spectrum)
            case 0x04: controlPadDriver->setLEDs(0, 255, 128); break;   // A: Cyan
            case 0x16: controlPadDriver->setLEDs(0, 255, 192); break;   // S: Light Cyan
            case 0x07: controlPadDriver->setLEDs(0, 255, 255); break;   // D: Pure Cyan
            case 0x09: controlPadDriver->setLEDs(0, 192, 255); break;   // F: Cyan-Blue
            case 0x0A: controlPadDriver->setLEDs(0, 128, 255); break;   // G: Light Blue
            
            // Row 4: ZXCVB (Blue spectrum)
            case 0x1D: controlPadDriver->setLEDs(0, 64, 255); break;    // Z: Blue
            case 0x1B: controlPadDriver->setLEDs(0, 0, 255); break;     // X: Pure Blue
            case 0x06: controlPadDriver->setLEDs(64, 0, 255); break;    // C: Blue-Purple
            case 0x19: controlPadDriver->setLEDs(128, 0, 255); break;   // V: Purple
            case 0x05: controlPadDriver->setLEDs(192, 0, 255); break;   // B: Purple-Magenta
            
            // Row 5: Additional keys (Magenta/Pink spectrum)
            case 0x1C: controlPadDriver->setLEDs(255, 0, 255); break;   // Enter: Magenta
            case 0x18: controlPadDriver->setLEDs(255, 0, 192); break;   // Key: Pink
            case 0x0C: controlPadDriver->setLEDs(255, 0, 128); break;   // Key: Hot Pink
            case 0x2C: controlPadDriver->setLEDs(255, 255, 255); break; // Space: White
            case 0x35: controlPadDriver->setLEDs(128, 128, 128); break; // Tilde: Gray
            
            // Default for any unmapped keys
            default: 
              Serial.printf("🔍 Unknown key: 0x%02X - using white\n", key);
              controlPadDriver->setLEDs(255, 255, 255); 
              break;
          }
          */
        }
      }
    } else if (event.len == 64) {
      // Control event from Interface 1 - reduce spam
      static int controlEventCounter = 0;
      if (++controlEventCounter % 20 == 1) {  // Show every 20th event
        Serial.printf("🎮 CONTROL Event #%d: ", controlEventCounter);
        for (int i = 0; i < min(8, (int)event.len); i++) {
          Serial.printf("0x%02X ", event.data[i]);
        }
        Serial.println();
      }
    }
  }
  
  // Test LED commands periodically (every 15 seconds)
  static unsigned long lastLEDTest = 0;
  if (millis() - lastLEDTest > 15000) {
    if (controlPadDriver) {
      Serial.println("\n🔴 Periodic LED test - RED...");
      controlPadDriver->setLEDs(255, 0, 0);
    } else {
      Serial.println("\n⚠️ No controlPadDriver instance - factory system may not be working");
      Serial.printf("Factory registered: %s\n", USBControlPad::factory_registered ? "YES" : "NO");
    }
    lastLEDTest = millis();
  }
} 