#include <Arduino.h>
#include <teensy4_usbhost.h>
#include <string.h>  // For memset

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
  
  // State buffers to hold full 25-button RGBW values for custom mode
  uint8_t statePacket1[64];  // Buttons 1-13
  uint8_t statePacket2[64];  // Buttons 14-25
  
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
    delay(12);  // Match USB capture timing: ~10-12ms
    
    Serial.println("🔄 Step 2: Main LED command (56 83 00...)");
    int result2 = InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb);
    delay(11);  // Match USB capture timing
    
    Serial.println("🔄 Step 3: LED index command (56 83 01...)");
    int result3 = InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb);
    delay(12);  // Match USB capture timing
    
    Serial.println("🔄 Step 4: Mode command (41 80...)");
    int result4 = InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb);
    delay(9);   // Match USB capture timing
    
    Serial.println("🔄 Step 5: Final red command (51 28...)");
    int result5 = InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    Serial.printf("📊 Results: %d %d %d %d %d\n", result1, result2, result3, result4, result5);
    
    return (result1 == 0 && result2 == 0 && result3 == 0 && result4 == 0 && result5 == 0);
  }

  // NEW: 5-Command LED Protocol based on breakdown analysis
  bool send5CommandLEDSequence(uint8_t buttonNumber, uint8_t r, uint8_t g, uint8_t b) {
    Serial.printf("🎯 Setting LED for button %d to RGB(%d,%d,%d) using 5-command protocol\n", buttonNumber, r, g, b);
    
    // Command 1: Set effect (custom mode)
    uint8_t cmd1_data[62] = {0};
    cmd1_data[0] = 0x01; cmd1_data[1] = 0x00; cmd1_data[2] = 0x00; cmd1_data[3] = 0x00;
    cmd1_data[4] = 0x02; cmd1_data[5] = 0x00; cmd1_data[6] = 0x00; cmd1_data[7] = 0x00;
    // Custom mode bytes
    cmd1_data[8] = 0xbb; cmd1_data[9] = 0xbb; cmd1_data[10] = 0xbb; cmd1_data[11] = 0xbb;
    cmd1_data[12] = 0xbb; cmd1_data[13] = 0xbb; cmd1_data[14] = 0xbb; cmd1_data[15] = 0xbb;
    
    if (!sendRawCommand(0x56, 0x81, cmd1_data, 62)) {
      Serial.println("❌ Command 1 failed");
      return false;
    }
    delay(12);  // Match USB capture timing: ~10-12ms
    
    // Command 2: Package 1 of 2 - LED data part 1
    uint8_t cmd2_data[62] = {0};
    // EXACT pattern from breakdown: 568300000100000080010000ff0000000000ffff00000000
    cmd2_data[0] = 0x00; cmd2_data[1] = 0x00; cmd2_data[2] = 0x01; cmd2_data[3] = 0x00; 
    cmd2_data[4] = 0x00; cmd2_data[5] = 0x00; cmd2_data[6] = 0x80; cmd2_data[7] = 0x01; 
    cmd2_data[8] = 0x00; cmd2_data[9] = 0x00; cmd2_data[10] = 0xff; cmd2_data[11] = 0x00; 
    cmd2_data[12] = 0x00; cmd2_data[13] = 0x00; cmd2_data[14] = 0x00; cmd2_data[15] = 0x00; 
    cmd2_data[16] = 0xff; cmd2_data[17] = 0xff; cmd2_data[18] = 0x00; cmd2_data[19] = 0x00; 
    cmd2_data[20] = 0x00; cmd2_data[21] = 0x00;
    
    // Initialize LED data area with zeros first (all LEDs off)
    for (int i = 22; i < 62; i++) {
      cmd2_data[i] = 0x00;
    }
    
    // Set LED data based on EXACT breakdown file structure
    // Command 2 contains: buttons 1,2,3,6,7,8,11,12,13,16,17,18(partial),21,22,23
    
    // Column 1: buttons 1,2,3 (at offsets 22,25,28)
    if (buttonNumber == 1) {
      cmd2_data[22] = r; cmd2_data[23] = g; cmd2_data[24] = b;  // Button 1
    } else {
      cmd2_data[22] = 0x00; cmd2_data[23] = 0x00; cmd2_data[24] = 0x00;
    }
    
    if (buttonNumber == 2) {
      cmd2_data[25] = r; cmd2_data[26] = g; cmd2_data[27] = b;  // Button 2
    } else {
      cmd2_data[25] = 0x00; cmd2_data[26] = 0x00; cmd2_data[27] = 0x00;
    }
    
    if (buttonNumber == 3) {
      cmd2_data[28] = r; cmd2_data[29] = g; cmd2_data[30] = b;  // Button 3
    } else {
      cmd2_data[28] = 0x00; cmd2_data[29] = 0x00; cmd2_data[30] = 0x00;
    }
    
    // Column 2: buttons 6,7,8 (at offsets 31,34,37)
    if (buttonNumber == 6) {
      cmd2_data[31] = r; cmd2_data[32] = g; cmd2_data[33] = b;  // Button 6
    } else {
      cmd2_data[31] = 0x00; cmd2_data[32] = 0x00; cmd2_data[33] = 0x00;
    }
    
    if (buttonNumber == 7) {
      cmd2_data[34] = r; cmd2_data[35] = g; cmd2_data[36] = b;  // Button 7
    } else {
      cmd2_data[34] = 0x00; cmd2_data[35] = 0x00; cmd2_data[36] = 0x00;
    }
    
    if (buttonNumber == 8) {
      cmd2_data[37] = r; cmd2_data[38] = g; cmd2_data[39] = b;  // Button 8
    } else {
      cmd2_data[37] = 0x00; cmd2_data[38] = 0x00; cmd2_data[39] = 0x00;
    }
    
    // Continue for other buttons in command 2 (11,12,13,16,17,18,21,22,23)
    // For now, just handle the first 8 buttons to test the fix
    
    // Debug: Show the exact packet for verification
    Serial.print("📦 Command 2 packet (first 32 bytes): ");
    for (int i = 0; i < 32; i++) {
      Serial.printf("%02X", cmd2_data[i]);
    }
    Serial.println();
    
    if (!sendRawCommand(0x56, 0x83, cmd2_data, 62)) {
      Serial.println("❌ Command 2 failed");
      return false;
    }
    delay(11);  // Match USB capture timing
    
    // Command 3: Package 2 of 2 - remaining LED data
    uint8_t cmd3_data[62] = {0x01, 0x00}; // Only the data part, not command bytes
    
    // Initialize with zeros
    for (int i = 2; i < 62; i++) {
      cmd3_data[i] = 0x00;
    }
    
    // EXACT structure from breakdown file:
    // cmd3 starts: 56 83 01 00
    // Then: 00 98 99 (GB of button 18)  -> offsets 2,3,4
    // Then: 97 98 99 (RGB button 23)   -> offsets 5,6,7  
    // Then: 65 66 67 (RGB button 4)    -> offsets 8,9,10   <-- BUTTON 4!
    // Then: 65 66 67 (RGB button 9)    -> offsets 11,12,13
    // Then: 65 66 67 (RGB button 14)   -> offsets 14,15,16
    // Then: 65 66 67 (RGB button 19)   -> offsets 17,18,19
    // Then: 65 66 67 (RGB button 24)   -> offsets 20,21,22
    // Then: 33 34 35 (RGB button 5)    -> offsets 23,24,25  <-- BUTTON 5!
    
    // Set button 18 GB part (fixed values for now)
    cmd3_data[2] = 0x00; cmd3_data[3] = 0x98; cmd3_data[4] = 0x99;
    
    // Set button 23 (fixed values for now) 
    cmd3_data[5] = 0x97; cmd3_data[6] = 0x98; cmd3_data[7] = 0x99;
    
    // Button 4 is at offsets 8,9,10
    if (buttonNumber == 4) {
      cmd3_data[8] = r; cmd3_data[9] = g; cmd3_data[10] = b;  // Button 4
      Serial.printf("🎨 Setting Button 4 at cmd3 offset 8-10: RGB(%d,%d,%d)\n", r, g, b);
    } else {
      cmd3_data[8] = 0x65; cmd3_data[9] = 0x66; cmd3_data[10] = 0x67; // Default values
    }
    
    // Set other intermediate buttons to default values
    cmd3_data[11] = 0x65; cmd3_data[12] = 0x66; cmd3_data[13] = 0x67; // Button 9
    cmd3_data[14] = 0x65; cmd3_data[15] = 0x66; cmd3_data[16] = 0x67; // Button 14
    cmd3_data[17] = 0x65; cmd3_data[18] = 0x66; cmd3_data[19] = 0x67; // Button 19
    cmd3_data[20] = 0x65; cmd3_data[21] = 0x66; cmd3_data[22] = 0x67; // Button 24
    
    // Button 5 is at offsets 23,24,25
    if (buttonNumber == 5) {
      cmd3_data[23] = r; cmd3_data[24] = g; cmd3_data[25] = b;  // Button 5
      Serial.printf("🎨 Setting Button 5 at cmd3 offset 23-25: RGB(%d,%d,%d)\n", r, g, b);
    } else {
      cmd3_data[23] = 0x33; cmd3_data[24] = 0x34; cmd3_data[25] = 0x35; // Default values
    }
    
    if (!sendRawCommand(0x56, 0x83, cmd3_data, 62)) {
      Serial.println("❌ Command 3 failed");
      return false;
    }
    delay(12);  // Match USB capture timing
    
    // Command 4: Apply command
    uint8_t cmd4_data[62] = {0};
    if (!sendRawCommand(0x41, 0x80, cmd4_data, 62)) {
      Serial.println("❌ Command 4 failed");
      return false;
    }
    delay(9);   // Match USB capture timing
    
    // Command 5: Final command
    uint8_t cmd5_data[62] = {0xff, 0x00};
    if (!sendRawCommand(0x51, 0x28, cmd5_data, 62)) {
      Serial.println("❌ Command 5 failed");
      return false;
    }
    
    Serial.println("✅ 5-command LED sequence completed!");
    return true;
  }

  // NEW: Simplified test function using exact breakdown mapping
  bool sendSimpleLEDTest(uint8_t buttonNumber, uint8_t r, uint8_t g, uint8_t b) {
    Serial.printf("🧪 COMPLETE STATE LED Protocol: Button %d = RGB(%d,%d,%d)\n", buttonNumber, r, g, b);
    
    // Command 1: EXACT custom mode pattern from working capture
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Command 2: Complete LED state from working capture with ALL buttons defined
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 
      // LED data starts here (position 24) - ALL 24 buttons get proper values
      0xfb, 0xfc, 0xfd,  // Button 1 (positions 24-26) - default or target
      0xfb, 0xfc, 0xfd,  // Button 6 (positions 27-29)
      0xfb, 0xfc, 0xfd,  // Button 11 (positions 30-32)
      0xfb, 0xfc, 0xfd,  // Button 16 (positions 33-35)
      0xfb, 0xfc, 0xfd,  // Button 21 (positions 36-38)
      0xc9, 0xca, 0xcb,  // Button 2 (positions 39-41)
      0xc9, 0xca, 0xcb,  // Button 7 (positions 42-44)
      0xc9, 0xca, 0xcb,  // Button 12 (positions 45-47)
      0xc9, 0xca, 0xcb,  // Button 17 (positions 48-50)
      0xc9, 0xca, 0xcb,  // Button 22 (positions 51-53)
      0x97, 0x98, 0x99,  // Button 3 (positions 54-56)
      0x97, 0x98, 0x99,  // Button 8 (positions 57-59)
      0x97, 0x98, 0x99,  // Button 13 (positions 60-62)
      0x97               // Button 18 R component (position 63)
    };
    
    // Override the specific button with bright target color
    if (buttonNumber == 1) {
      cmd2[24] = r; cmd2[25] = g; cmd2[26] = b;  // Button 1
    } else if (buttonNumber == 6) {
      cmd2[27] = r; cmd2[28] = g; cmd2[29] = b;  // Button 6
    } else if (buttonNumber == 11) {
      cmd2[30] = r; cmd2[31] = g; cmd2[32] = b;  // Button 11   
    } else if (buttonNumber == 16) {
      cmd2[33] = r; cmd2[34] = g; cmd2[35] = b;  // Button 16
    } else if (buttonNumber == 21) {
      cmd2[36] = r; cmd2[37] = g; cmd2[38] = b;  // Button 21
    } else if (buttonNumber == 2) {
      cmd2[39] = r; cmd2[40] = g; cmd2[41] = b;  // Button 2
    } else if (buttonNumber == 7) {
      cmd2[42] = r; cmd2[43] = g; cmd2[44] = b;  // Button 7
    } else if (buttonNumber == 12) {
      cmd2[45] = r; cmd2[46] = g; cmd2[47] = b;  // Button 12
    } else if (buttonNumber == 17) {
      cmd2[48] = r; cmd2[49] = g; cmd2[50] = b;  // Button 17
     } else if (buttonNumber == 22) {
      cmd2[51] = r; cmd2[52] = g; cmd2[53] = b;  // Button 22
    } else if (buttonNumber == 3) {
      cmd2[54] = r; cmd2[55] = g; cmd2[56] = b;  // Button 3
    } else if (buttonNumber == 8) {
      cmd2[57] = r; cmd2[58] = g; cmd2[59] = b;  // Button 8
  } else if (buttonNumber == 13) {
      cmd2[60] = r; cmd2[61] = g; cmd2[62] = b;  // Button 13
    } else if (buttonNumber == 18) {
      cmd2[63] = r;  // Button 18 R component (continues in cmd3)
    }
    
    // Command 3: Complete remaining LED state from working capture
    uint8_t cmd3[64] = {
      0x56, 0x83, 0x01,
      0x00, 0x98, 0x99,        // Button 18 GB (positions 4-5) - continues from cmd2 R
      0x97, 0x98, 0x99,        // Button 23 (positions 6-8)
      0x65, 0x66, 0x67,        // Button 4 (positions 9-11)
      0x65, 0x66, 0x67,        // Button 9 (positions 12-14)
      0x65, 0x66, 0x67,        // Button 14 (positions 15-17)
      0x65, 0x66, 0x67,        // Button 19 (positions 18-20)
      0x65, 0x66, 0x67,        // Button 24 (positions 21-23)
      0x33, 0x34, 0x35,        // Button 5 (positions 24-26) *** CORRECT POSITION ***
      0x33, 0x34, 0x35,        // Button 10 (positions 27-29)
      0x33, 0x34, 0x35,        // Button 15 (positions 30-32)
      0x33, 0x34, 0x35,        // Button 20 (positions 33-35)
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Override specific buttons in cmd3 with target colors
    if (buttonNumber == 18) {
      // Complete Button 18 GB components (only 2 bytes in cmd3)
      cmd3[4] = g; cmd3[5] = b;  // Button 18 GB (positions 4-5)
    } else if (buttonNumber == 23) {
      cmd3[6] = r; cmd3[7] = g; cmd3[8] = b;     // Button 23 (positions 6-8)
    } else if (buttonNumber == 4) {
      cmd3[9] = r; cmd3[10] = g; cmd3[11] = b;   // Button 4 (positions 9-11)
    } else if (buttonNumber == 9) {
      cmd3[12] = r; cmd3[13] = g; cmd3[14] = b;  // Button 9 (positions 12-14)
    } else if (buttonNumber == 14) {
      cmd3[15] = r; cmd3[16] = g; cmd3[17] = b;  // Button 14 (positions 15-17)
    } else if (buttonNumber == 19) {
      cmd3[18] = r; cmd3[19] = g; cmd3[20] = b;  // Button 19 (positions 18-20)
    } else if (buttonNumber == 24) {
      cmd3[21] = r; cmd3[22] = g; cmd3[23] = b;  // Button 24 (positions 21-23)
     } else if (buttonNumber == 5) {
      cmd3[24] = r; cmd3[25] = g; cmd3[26] = b;  // Button 5 (positions 24-26) *** FIXED ***
    } else if (buttonNumber == 10) {
      cmd3[27] = r; cmd3[28] = g; cmd3[29] = b;  // Button 10 (positions 27-29)
    } else if (buttonNumber == 15) {
      cmd3[30] = r; cmd3[31] = g; cmd3[32] = b;  // Button 15 (positions 30-32)
    } else if (buttonNumber == 20) {
      cmd3[33] = r; cmd3[34] = g; cmd3[35] = b;  // Button 20 (positions 33-35)
    }
    
    // Commands 4 and 5: EXACT patterns from working capture
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    Serial.printf("🎯 Sending COMPLETE LED state for button %d with all 24 buttons defined\n", buttonNumber);
    
    // Send the complete 5-command sequence
    Serial.println("📤 Command 1: Custom mode");
    int result1 = InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb);
    if (result1 != 0) {
      Serial.printf("❌ Command 1 failed: %d\n", result1);
      return false;
    }
    delay(12);
    
    Serial.println("📤 Command 2: Complete LED state package 1");
    int result2 = InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb);
    if (result2 != 0) {
      Serial.printf("❌ Command 2 failed: %d\n", result2);
      return false;
    }
    delay(11);
    
    Serial.println("📤 Command 3: Complete LED state package 2");
    int result3 = InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb);
    if (result3 != 0) {
      Serial.printf("❌ Command 3 failed: %d\n", result3);
      return false;
    }
    delay(12);
    
    Serial.println("📤 Command 4: Apply");
    int result4 = InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb);
    if (result4 != 0) {
      Serial.printf("❌ Command 4 failed: %d\n", result4);
      return false;
    }
    delay(9);
    
    Serial.println("📤 Command 5: Finalize");
    int result5 = InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    if (result5 != 0) {
      Serial.printf("❌ Command 5 failed: %d\n", result5);
      return false;
    }
    
    Serial.printf("✅ COMPLETE LED state sent - Button %d highlighted!\n", buttonNumber);
    return true;
  }

  bool sendExactLEDCommand() {
    // Call the complete red sequence
    return sendCompleteRedSequence();
  }

  bool sendGreenPattern() {
    Serial.println("🟢 Sending COMPLETE GREEN sequence (5 steps)");
    
    // Step 1: Setup command (same as red)
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 2: Main LED command with GREEN color
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 3: LED index command (same as red)
    uint8_t cmd3[64] = {
      0x56, 0x83, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 4: Mode command (same as red)
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 5: Final GREEN command (modified from red)
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    Serial.println("🔄 GREEN Step 1: Setup command");
    int result1 = InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb);
    delay(50);
    
    Serial.println("🔄 GREEN Step 2: Main LED command");
    int result2 = InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb);
    delay(50);
    
    Serial.println("🔄 GREEN Step 3: LED index command");
    int result3 = InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb);
    delay(50);
    
    Serial.println("🔄 GREEN Step 4: Mode command");
    int result4 = InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb);
    delay(50);
    
    Serial.println("🔄 GREEN Step 5: Final green command");
    int result5 = InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    Serial.printf("📊 GREEN Results: %d %d %d %d %d\n", result1, result2, result3, result4, result5);
    
    return (result1 == 0 && result2 == 0 && result3 == 0 && result4 == 0 && result5 == 0);
  }

  bool sendBluePattern() {
    Serial.println("🔵 Sending COMPLETE BLUE sequence (5 steps)");
    
    // Steps 1, 3, 4 same as red - only step 2 and 5 change for blue
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd3[64] = {
      0x56, 0x83, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    return true;
  }

  bool sendYellowPattern() {
    Serial.println("🟡 Sending COMPLETE YELLOW sequence (5 steps)");
    
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd3[64] = {
      0x56, 0x83, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    return true;
  }

  bool sendPurplePattern() {
    Serial.println("🟣 Sending COMPLETE PURPLE sequence (5 steps)");
    
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd3[64] = {
      0x56, 0x83, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    return true;
  }

  bool sendWhitePattern() {
    Serial.println("⚪ Sending COMPLETE WHITE sequence (5 steps)");
    
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd3[64] = {
      0x56, 0x83, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb); delay(50);
    InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    return true;
  }

  bool sendButtonSpecificRed(uint8_t buttonIndex) {
    Serial.printf("🔴 Sending RED to specific button %d (5 steps)\n", buttonIndex);
    
    // Step 1: Setup command (same)
    uint8_t cmd1[64] = {
      0x56, 0x81, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 2: Main LED command (same)
    uint8_t cmd2[64] = {
      0x56, 0x83, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 3: LED index command - TARGET SPECIFIC BUTTON
    uint8_t cmd3[64] = {
      0x56, 0x83, buttonIndex, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 4: Mode command (same)
    uint8_t cmd4[64] = {
      0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Step 5: Final red command (same)
    uint8_t cmd5[64] = {
      0x51, 0x28, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    Serial.printf("🔄 RED Step 1: Setup command\n");
    InterruptMessage(ctrl_ep_out, 64, cmd1, &send_cb); delay(50);
    
    Serial.printf("🔄 RED Step 2: Main LED command\n");
    InterruptMessage(ctrl_ep_out, 64, cmd2, &send_cb); delay(50);
    
    Serial.printf("🔄 RED Step 3: LED index command (button %d)\n", buttonIndex);
    InterruptMessage(ctrl_ep_out, 64, cmd3, &send_cb); delay(50);
    
    Serial.printf("🔄 RED Step 4: Mode command\n");
    InterruptMessage(ctrl_ep_out, 64, cmd4, &send_cb); delay(50);
    
    Serial.printf("🔄 RED Step 5: Final red command\n");
    InterruptMessage(ctrl_ep_out, 64, cmd5, &send_cb);
    
    return true;
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
  
  // NEW: Raw command function for 91-byte packets (5-command protocol)
  bool sendRawCommand(uint8_t cmd1, uint8_t cmd2, uint8_t* data, size_t dataLen) {
    // Create 64-byte packet (USB interrupt endpoint standard)
    uint8_t packet[64] = {0};
    packet[0] = cmd1;
    packet[1] = cmd2;
    
    // Copy data starting from byte 2
    if (data && dataLen > 0) {
      size_t copyLen = (dataLen > 62) ? 62 : dataLen;  // 64 - 2 header bytes
      memcpy(&packet[2], data, copyLen);
    }
    
    Serial.printf("📤 Sending 64-byte command: 0x%02X 0x%02X (data len: %zu)\n", cmd1, cmd2, dataLen);
    
    // Send raw 64-byte packet to control endpoint
    int maxRetries = 3;
    for (int attempt = 0; attempt < maxRetries; attempt++) {
      int result = InterruptMessage(ctrl_ep_out, 64, packet, &send_cb);
      if (result == 0) {
        if (attempt > 0) {
          Serial.printf("✅ 64-byte command succeeded on attempt %d\n", attempt + 1);
        }
        return true;
      } else {
        Serial.printf("⚠️ 64-byte command attempt %d failed: %d\n", attempt + 1, result);
        if (attempt < maxRetries - 1) {
          delay(20);
        }
      }
    }
    
    Serial.printf("❌ 64-byte command failed after %d attempts\n", maxRetries);
    return false;
  }

  // NEW: Test both packet sizes to see which works
  bool sendTestPacket(uint8_t cmd1, uint8_t cmd2, uint8_t* data, size_t dataLen) {
    Serial.printf("🧪 Testing packet sizes for command 0x%02X 0x%02X\n", cmd1, cmd2);
    
    // Try 64-byte first (traditional USB interrupt packet size)
    Serial.println("   Trying 64-byte packet...");
    uint8_t packet64[64] = {0};
    packet64[0] = cmd1;
    packet64[1] = cmd2;
    if (data && dataLen > 0) {
      size_t copyLen = (dataLen > 62) ? 62 : dataLen;
      memcpy(&packet64[2], data, copyLen);
    }
    
    int result64 = InterruptMessage(ctrl_ep_out, 64, packet64, &send_cb);
    Serial.printf("   64-byte result: %d\n", result64);
    
    if (result64 == 0) {
      Serial.println("✅ 64-byte packet worked!");
      return true;
    }
    
    delay(50);
    
    // Try 91-byte if 64 failed
    Serial.println("   Trying 91-byte packet...");
    uint8_t packet91[91] = {0};
    packet91[0] = cmd1;
    packet91[1] = cmd2;
    if (data && dataLen > 0) {
      size_t copyLen = (dataLen > 89) ? 89 : dataLen;
      memcpy(&packet91[2], data, copyLen);
    }
    
    int result91 = InterruptMessage(ctrl_ep_out, 91, packet91, &send_cb);
    Serial.printf("   91-byte result: %d\n", result91);
    
    if (result91 == 0) {
      Serial.println("✅ 91-byte packet worked!");
      return true;
    }
    
    Serial.println("❌ Both packet sizes failed!");
    return false;
  }

  bool sendCommand(uint8_t cmd1, uint8_t cmd2, uint8_t* extraData = nullptr, size_t extraLen = 0) {
    if (!initialized) {
      Serial.println("❌ Device not initialized");
      return false;
    }
    
    ControlPadPacket packet;
    packet.cmd1 = cmd1;
    packet.cmd2 = cmd2;
    
    if (extraData && extraLen > 0) {
      size_t copyLen = (extraLen > 61) ? 61 : extraLen;
      memcpy(packet.data, extraData, copyLen);
    }
    
    Serial.printf("📤 Sending cmd [%02X %02X] to EP 0x%02X\n", cmd1, cmd2, ctrl_ep_out);
    
    // Use callback-based transfer
    int result = InterruptMessage(ctrl_ep_out, 64, (uint8_t*)&packet, &send_cb);
    
    if (result != 0) {
      Serial.printf("❌ Command failed with result: %d\n", result);
    }
    
    return (result == 0);
  }

  // Helper function for sending raw control data
  int sendControlData(uint8_t* data, size_t length) {
    if (length > 64) {
      Serial.printf("❌ Data too large: %d bytes (max 64)\n", length);
      return -2;
    }
    Serial.printf("📤 Sending %d bytes to EP 0x%02X\n", length, ctrl_ep_out);
    return InterruptMessage(ctrl_ep_out, length, data, &send_cb);
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
  
  bool initializeProfiles() {
    Serial.println("🎮 INITIALIZING DEVICE PROFILES...");
    
    // Profile enumeration sequence from working capture: 0x52 0x80 0x00-0x17
    // This appears to be required before LED control becomes functional
    for (uint8_t profile = 0x00; profile <= 0x17; profile++) {
      uint8_t profileCmd[64] = {0};
      profileCmd[0] = 0x52;
      profileCmd[1] = 0x80;
      profileCmd[2] = profile;
      
      Serial.printf("📋 Setting profile %02X...\n", profile);
      sendControlData(profileCmd, 64);
      delay(10); // Small delay between profile commands
    }
    
    Serial.println("✅ Profile initialization complete");
    return true;
  }

  bool initializeDevice() {
    if (initialized) return true;
    
    Serial.println("🔧 INITIALIZING CONTROLPAD DEVICE...");
    
    if (!checkDeviceHealth()) {
      Serial.println("❌ Device health check failed");
      return false;
    }
    
    // Start dual interface polling first
    startDualPolling();
    delay(100);
    
    // CRITICAL: Device initialization commands (from working capture)
    Serial.println("🔧 Sending device initialization commands...");
    
    // Command 1: 42000000010000010000...
    uint8_t initCmd1[64] = {0};
    initCmd1[0] = 0x42; initCmd1[1] = 0x00; initCmd1[2] = 0x00; initCmd1[3] = 0x00;
    initCmd1[4] = 0x01; initCmd1[5] = 0x00; initCmd1[6] = 0x00; initCmd1[7] = 0x01;
    sendControlData(initCmd1, 64);
    delay(20);
    
    // Command 2: 42100000010000010000...
    uint8_t initCmd2[64] = {0};
    initCmd2[0] = 0x42; initCmd2[1] = 0x10; initCmd2[2] = 0x00; initCmd2[3] = 0x00;
    initCmd2[4] = 0x01; initCmd2[5] = 0x00; initCmd2[6] = 0x00; initCmd2[7] = 0x01;
    sendControlData(initCmd2, 64);
    delay(20);
    
    // Command 3: 43000000010000000000...
    uint8_t initCmd3[64] = {0};
    initCmd3[0] = 0x43; initCmd3[1] = 0x00; initCmd3[2] = 0x00; initCmd3[3] = 0x00;
    initCmd3[4] = 0x01; initCmd3[5] = 0x00; initCmd3[6] = 0x00; initCmd3[7] = 0x00;
    sendControlData(initCmd3, 64);
    delay(20);
    
    // Commit initialization
    sendCommitCommand();
    delay(50);

    // CRITICAL: Initialize profiles first (required for LED control)
    Serial.println("🎮 Initializing device profiles...");
    if (!initializeProfiles()) {
      Serial.println("❌ Failed to initialize profiles");
      return false;
    }
    delay(100);
    
    // Set to custom mode for LED control
    Serial.println("🎨 Setting device to CUSTOM MODE for LED control...");
    if (!switchToCustomMode()) {
      Serial.println("❌ Failed to switch to custom mode");
      return false;
    }
    delay(100);
    
    // Initialize state buffers with exact headers from working capture
    memset(statePacket1, 0, sizeof(statePacket1));
    memset(statePacket2, 0, sizeof(statePacket2));
    
    // Packet 1: 568300000100000080010000...RGB data... 
    statePacket1[0]=0x56; statePacket1[1]=0x83; statePacket1[2]=0x00; statePacket1[3]=0x00;
    statePacket1[4]=0x01; statePacket1[5]=0x00; statePacket1[6]=0x00; statePacket1[7]=0x00;
    statePacket1[8]=0x80; statePacket1[9]=0x01; statePacket1[10]=0x00; statePacket1[11]=0x00;
    
    // Packet 2: 56830100...RGB data...
    statePacket2[0]=0x56; statePacket2[1]=0x83; statePacket2[2]=0x01; statePacket2[3]=0x00;
    initialized = true;
    
    Serial.println("✅ ControlPad device initialized successfully");
    Serial.println("🎯 Ready for LED commands!");
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
        
        Serial.printf("📤 QUEUE PUT: Added keyboard event (len=%d) to queue\n", event.len);
        Serial.printf("🎯 ACTUAL KEY PRESS detected! Packet: ");
        for (int i = 0; i < 8; i++) {
          Serial.printf("0x%02X ", kbd_report[i]);
        }
        Serial.println();
        
        // Process button mapping immediately in kbd_poll (most reliable)
        if (controlPadDriver) {
          Serial.printf("🔥 KEY PRESSED: 0x%02X - Processing in kbd_poll\n", kbd_report[2]);
          
          // Map HID codes to button numbers based on breakdown file
          uint8_t buttonNumber = 0;
          uint8_t r = 255, g = 0, b = 0; // Default red
          
          switch(kbd_report[2]) {
            // Row 1: Buttons 1-5
            case 0x1E: buttonNumber = 1; r = 255; g = 0; b = 0; break;     // Button 1: Red
            case 0x1F: buttonNumber = 2; r = 0; g = 255; b = 0; break;     // Button 2: Green  
            case 0x20: buttonNumber = 3; r = 0; g = 0; b = 255; break;     // Button 3: Blue
            case 0x21: buttonNumber = 4; r = 255; g = 255; b = 0; break;   // Button 4: Yellow
            case 0x22: buttonNumber = 5; r = 255; g = 125; b = 255; break; // Button 5: Magenta
            
            // Row 2: Buttons 6-10 (based on breakdown mapping)
            case 0x23: buttonNumber = 6; r = 0; g = 255; b = 255; break;   // Button 6: Cyan
            case 0x24: buttonNumber = 7; r = 255; g = 128; b = 0; break;   // Button 7: Orange
            case 0x25: buttonNumber = 8; r = 128; g = 0; b = 255; break;   // Button 8: Purple
            case 0x26: buttonNumber = 9; r = 255; g = 255; b = 255; break; // Button 9: White
            case 0x27: buttonNumber = 10; r = 255; g = 128; b = 128; break; // Button 10: Light Red
            
            // Row 3: Buttons 11-15
            case 0x04: buttonNumber = 11; r = 255; g = 64; b = 64; break;   // Button 11: Light Red
            case 0x05: buttonNumber = 12; r = 64; g = 255; b = 64; break;   // Button 12: Light Green
            case 0x06: buttonNumber = 13; r = 64; g = 64; b = 255; break;   // Button 13: Light Blue
            case 0x07: buttonNumber = 14; r = 192; g = 192; b = 0; break;   // Button 14: Dark Yellow
            case 0x08: buttonNumber = 15; r = 192; g = 0; b = 192; break;   // Button 15: Dark Magenta
            
            // Row 4: Buttons 16-20
            case 0x09: buttonNumber = 16; r = 0; g = 192; b = 192; break;   // Button 16: Dark Cyan
            case 0x0A: buttonNumber = 17; r = 255; g = 192; b = 128; break; // Button 17: Peach
            case 0x0B: buttonNumber = 18; r = 128; g = 255; b = 192; break; // Button 18: Mint (split RGB!)
            case 0x0C: buttonNumber = 19; r = 192; g = 128; b = 255; break; // Button 19: Lavender
            case 0x0D: buttonNumber = 20; r = 255; g = 255; b = 128; break; // Button 20: Light Yellow
            
            // Row 5: Buttons 21-24 (button 24 is 2-wide)
            case 0x00: buttonNumber = 21; r = 128; g = 255; b = 255; break; // Button 21: Light Cyan
            case 0x0F: buttonNumber = 22; r = 255; g = 128; b = 255; break; // Button 22: Light Magenta
            case 0x10: buttonNumber = 23; r = 255; g = 255; b = 192; break; // Button 23: Cream
            case 0x11: buttonNumber = 24; r = 64; g = 128; b = 192; break;  // Button 24: Steel Blue
            
            default:
              Serial.printf("🔍 Unknown key: 0x%02X - using button 1 with white\n", kbd_report[2]);
              buttonNumber = 1; r = 255; g = 255; b = 255;
              break;
          }
          
          if (buttonNumber > 0) {
            Serial.printf("🎯 Mapping HID 0x%02X to Button %d with RGB(%d,%d,%d)\n", kbd_report[2], buttonNumber, r, g, b);
            controlPadDriver->sendSimpleLEDTest(buttonNumber, r, g, b);
          }
        }
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

  bool sendRealLEDCommand(uint8_t buttonIndex, uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0) {
    Serial.printf("🎯 REAL LED COMMAND: Button %d -> RGB(%d,%d,%d)\n", buttonIndex, r, g, b);

    if (buttonIndex < 1 || buttonIndex > 25) {
      Serial.printf("❌ Invalid button index: %d\n", buttonIndex);
      return false;
    }

    // Column-based mapping: button1, button6, button11, button16, button21, button2, button7, etc.
    // 5x5 grid arranged by columns: [1,6,11,16,21], [2,7,12,17,22], [3,8,13,18,23], [4,9,14,19,24], [5,10,15,20,25]
    uint8_t idx = buttonIndex - 1;  // 0-based index
    uint8_t row = idx % 5;          // Row within column (0-4)
    uint8_t col = idx / 5;          // Column number (0-4) 
    uint8_t pos = row * 5 + col;    // Column-major position
    
    Serial.printf("🗺️ Button %d -> row=%d, col=%d, pos=%d\n", buttonIndex, row, col, pos);

    // Switch to custom mode if not already done
    switchToCustomMode();
    delay(20);
    
    // Update full LED state buffers at the mapped position (RGB, no white)
    if (pos < 13) {
      size_t off = 12 + pos * 3;
      statePacket1[off]     = r;
      statePacket1[off + 1] = g;
      statePacket1[off + 2] = b;
      Serial.printf("📦 Packet1[%d:%d] = RGB(%d,%d,%d)\n", off, off+2, r, g, b);
    } else {
      size_t off = 4 + (pos - 13) * 3;
      statePacket2[off]     = r;
      statePacket2[off + 1] = g;
      statePacket2[off + 2] = b;
      Serial.printf("📦 Packet2[%d:%d] = RGB(%d,%d,%d)\n", off, off+2, r, g, b);
    }
    // Send full-state packets to apply LED changes
    Serial.println("📤 Sending LED state packets...");
    sendControlData(statePacket1, 64);
    delay(10);
    sendControlData(statePacket2, 64);
    delay(10);
    
    // CRITICAL: Send commit command to apply changes  
    sendCommitCommand();
    
    return true;
  }

  // Switch to static mode
  bool switchToStaticMode() {
    Serial.println("🔧 SWITCHING TO STATIC MODE...");
    // Payload from capture: frame15353 static (offset8=0x02, then background pattern)
    static const uint8_t modeStatic[64] = {
      0x56, 0x81, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00,
      0x02, 0x00, 0x00, 0x00,
      0x55, 0x55, 0x55, 0x55
    };
    Serial.println("📤 Sending STATIC MODE via interrupt...");
    sendControlData((uint8_t*)modeStatic, 64);
    delay(50);
    return true;
  }

  // Switch to custom mode
  bool switchToCustomMode() {
    Serial.println("🔧 SWITCHING TO CUSTOM MODE...");
    // Exact payload from working capture: 568100000100000002000000bbbbbbbb...
    static const uint8_t modeCustom[64] = {
      0x56, 0x81, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00,
      0x02, 0x00, 0x00, 0x00,
      0xbb, 0xbb, 0xbb, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Serial.println("📤 Sending CUSTOM MODE via interrupt...");
    sendControlData((uint8_t*)modeCustom, 64);
    delay(50);
    return true;
  }

  // Add demo helper to set all LEDs at once
  void setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
    Serial.printf("☑️ Setting ALL LEDs to RGB(%d,%d,%d)\n", r, g, b);
    if (!initialized) initializeDevice();
    
    // Ensure we're in custom mode
    switchToCustomMode();
    delay(20);
    
    // Update full state buffers for all 25 buttons using corrected column-based mapping
    for (uint8_t button = 1; button <= 25; button++) {
      uint8_t idx = button - 1;       // 0-based index
      uint8_t row = idx % 5;          // Row within column (0-4)
      uint8_t col = idx / 5;          // Column number (0-4) 
      uint8_t pos = row * 5 + col;    // Column-major position
      
      if (pos < 13) {
        size_t off = 12 + pos * 3;
        statePacket1[off]     = r;
        statePacket1[off + 1] = g;
        statePacket1[off + 2] = b;
      } else {
        size_t off = 4 + (pos - 13) * 3;
        statePacket2[off]     = r;
        statePacket2[off + 1] = g;
        statePacket2[off + 2] = b;
      }
    }
    
    // Send updated full-state packets
    Serial.println("📤 Sending ALL LED state packets...");
    Serial.print("📦 Packet1 header: ");
    for (int i = 0; i < 12; i++) {
      Serial.printf("%02X ", statePacket1[i]);
    }
    Serial.println();
    
    sendControlData(statePacket1, 64);
    delay(20);
    sendControlData(statePacket2, 64);
    delay(20);
    
    // CRITICAL: Send commit command to apply LED changes
    sendCommitCommand();
  }

  // Test function to set specific button to red (matching working capture)
  bool testButton1Red() {
    Serial.println("🧪 TESTING: Setting Button 1 to RED (matching working capture)");
    
    if (!initialized) initializeDevice();
    
    // Clear all LEDs first
    memset(statePacket1 + 12, 0, 52);  // Clear RGB data in packet1 
    memset(statePacket2 + 4, 0, 60);   // Clear RGB data in packet2
    
    // Set button 1 to red - should be position 0 (bytes 12-14 in packet1)
    statePacket1[12] = 0xFF;  // Red
    statePacket1[13] = 0x00;  // Green
    statePacket1[14] = 0x00;  // Blue
    
    Serial.println("📤 Sending test LED command...");
    Serial.print("📦 Packet1 first 20 bytes: ");
    for (int i = 0; i < 20; i++) {
      Serial.printf("%02X ", statePacket1[i]);
    }
    Serial.println();
    
    sendControlData(statePacket1, 64);
    delay(20);
    sendControlData(statePacket2, 64);
    delay(20);
    
    // CRITICAL: Send commit command to apply changes
    sendCommitCommand();
    
    return true;
  }

  // Send commit command to apply LED changes (0x41 0x80 from working capture)
  bool sendCommitCommand() {
    uint8_t commitCmd[64] = {0};
    commitCmd[0] = 0x41;
    commitCmd[1] = 0x80;
    // Rest stays zero
    
    Serial.println("💾 Sending COMMIT command to apply LED changes...");
    sendControlData(commitCmd, 64);
    delay(10);
    return true;
  }

  // Test function to replicate exact working pattern from capture
  bool testExactWorkingPattern() {
    Serial.println("🔥 TESTING: Exact working pattern from USB capture");
    
    if (!initialized) initializeDevice();
    
    // Clear packets first
    memset(statePacket1 + 12, 0, 52);
    memset(statePacket2 + 4, 0, 60);
    
    // Packet 1 RGB data from working capture: ff0000000000ffff0000000000ffff00ffff00ffff00ffff00ffff00ffffffff00ffff00ffff000000ff00ffffffff00ffff00ff
    uint8_t workingData1[] = {
      0xff, 0x00, 0x00,  // Red
      0x00, 0x00, 0x00,  // Black  
      0xff, 0xff, 0x00,  // Yellow
      0x00, 0x00, 0x00,  // Black
      0x00, 0x00, 0xff,  // Blue
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0xff, 0xff,  // White
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0xff,  // Magenta
      0x00, 0x00, 0x00,  // Black
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0xff, 0xff,  // White
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0xff   // Magenta
    };
    
    // Packet 2 RGB data from working capture: ff00d752ff00ffffffff00ffff00ffff00ff000000ffffffff0000ff00ffff00000000...
    uint8_t workingData2[] = {
      0xff, 0x00, 0xd7,  // Magenta-ish
      0x52, 0xff, 0x00,  // Green-ish
      0xff, 0xff, 0xff,  // White
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0xff,  // Magenta
      0x00, 0x00, 0x00,  // Black
      0xff, 0xff, 0xff,  // White
      0xff, 0x00, 0x00,  // Red
      0xff, 0x00, 0xff,  // Magenta
      0xff, 0x00, 0x00,  // Red
      0x00, 0x00, 0x00   // Black
    };
    
    // Copy exact working patterns  
    memcpy(statePacket1 + 12, workingData1, sizeof(workingData1));
    memcpy(statePacket2 + 4, workingData2, sizeof(workingData2));
    
    Serial.println("📤 Sending EXACT working LED pattern...");
    Serial.print("📦 Packet1 data: ");
    for (int i = 12; i < 30; i++) {
      Serial.printf("%02X ", statePacket1[i]);
    }
    Serial.println();
    
    sendControlData(statePacket1, 64);
    delay(20);
    sendControlData(statePacket2, 64);
    delay(20);
    sendCommitCommand();
    
    return true;
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
  static bool firstRun = true;
  static unsigned long lastTime = 0;
  static bool toggle = false;
  
  // Process any pending controlpad events from the queue
  controlpad_event event;
  if (atomQueueGet(&controlpad_queue, 0, &event) == ATOM_OK) {
    // Distinguish between keyboard events (8 bytes) and control events (64 bytes)
    if (event.len == 8) {
      // Standard HID keyboard event from Interface 0 - already processed in kbd_poll
      if (event.data[2] != 0 && event.data[2] < 0x80) {
        uint8_t key = event.data[2];
        Serial.printf("⌨️ KEYBOARD Key Press: 0x%02X (%d) - Already processed in kbd_poll\n", key, key);
        // Note: LED processing is handled immediately in kbd_poll for better response time
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
}
  