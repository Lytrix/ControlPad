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

// Structure to track Windows command verification
struct CommandResponse {
  bool received = false;
  bool verified = false;
  uint8_t expectedCmd1 = 0;
  uint8_t expectedCmd2 = 0;
  uint8_t actualCmd1 = 0;
  uint8_t actualCmd2 = 0;
};

// ===== GLOBAL VARIABLES =====
static DMAMEM TeensyUSBHost2 usbHost;
ATOM_QUEUE controlpad_queue;
controlpad_event controlpad_queue_data[8];

// Forward declaration for the global driver instance
class USBControlPad;
USBControlPad* controlPadDriver = nullptr;

// ControlPad Device Identification
#define CONTROLPAD_VID 0x2516
#define CONTROLPAD_PID 0x012D

// Global device tracking for raw USB mode
static uint16_t current_device_vid = 0;
static uint16_t current_device_pid = 0;

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
  
  // Interface 2 (Dual Action) endpoints and data
  uint8_t dual_ep_in = 0x82;   // Dual action input endpoint
  uint8_t dual_report[8] __attribute__((aligned(32)));
  
  // Interface 3 (Real Buttons) endpoints and data
  uint8_t hall_sensor_ep_in = 0x86;    // Hall sensor data endpoint (Interface 3)
  uint8_t btn_ep_out = 0x07;   // Alternative button events endpoint 
  uint8_t hall_sensor_report[32] __attribute__((aligned(32)));
  uint8_t sensor_out_report[32] __attribute__((aligned(32)));  // For endpoint 0x07
  
  // State buffers to hold full 25-button RGBW values for custom mode
  uint8_t statePacket1[64];  // Buttons 1-13
  uint8_t statePacket2[64];  // Buttons 14-25
  
  uint8_t report_len = 64;
  ATOM_QUEUE* queue = nullptr;
  
  // Separate callbacks for different interfaces
  USBCallback kbd_poll_cb;
  USBCallback ctrl_poll_cb;
  USBCallback dual_poll_cb;
  USBCallback hall_sensor_poll_cb;
  USBCallback sensor_out_poll_cb;  // New callback for endpoint 0x07
  USBCallback send_cb;
  
  uint8_t lastHallGroup = 0;
  
  // Command verification tracking
  CommandResponse lastCommandResponse;

public:
  // Static initialization test
  static bool factory_registered;
  
  // Static tracking to prevent multiple driver instances
  static bool driver_instance_created;
  
  // Make polling state accessible for monitoring
  bool kbd_polling = false;
  bool ctrl_polling = false;
  bool dual_polling = false;
  bool hall_sensor_polling = false;
  bool sensor_out_polling = false;  // New polling state for endpoint 0x07
  bool initialized = false;  // Moved to public section
  
  // Constructor for USB_Driver_FactoryGlue (requires USB_Device*)
  USBControlPad(USB_Device* dev) : USB_Driver_FactoryGlue<USBControlPad>(dev), 
                                   kbd_poll_cb([this](int r) { kbd_poll(r); }),
                                   ctrl_poll_cb([this](int r) { ctrl_poll(r); }),
                                   dual_poll_cb([this](int r) { dual_poll(r); }),
                                   hall_sensor_poll_cb([this](int r) { hall_sensor_poll(r); }),
                                   sensor_out_poll_cb([this](int r) { sensor_out_poll(r); }),
                                   send_cb([this](int r) { sent(r); }) {
    Serial.println("🔧 USBControlPad DUAL INTERFACE driver instance created");
    factory_registered = true;
  }
  
  // USB_Driver_FactoryGlue REQUIRED STATIC METHODS
  // These are called by the factory system during device enumeration
  
  static bool offer_interface(const usb_interface_descriptor* iface, size_t length) {
    Serial.println("🔍 *** USBControlPad::offer_interface called ***");
    Serial.printf("   Interface: %d\n", iface->bInterfaceNumber);
    Serial.printf("   Class: 0x%02X\n", iface->bInterfaceClass);
    Serial.printf("   SubClass: 0x%02X\n", iface->bInterfaceSubClass);
    Serial.printf("   Protocol: 0x%02X\n", iface->bInterfaceProtocol);
    Serial.printf("   NumEndpoints: %d\n", iface->bNumEndpoints);
    Serial.printf("   AltSetting: %d\n", iface->bAlternateSetting);
    
    // Show basic endpoint info
    const uint8_t* desc = (const uint8_t*)iface;
    size_t pos = iface->bLength;
    Serial.println("   Endpoints:");
    
    while (pos < length && pos + 7 < length) {
      const uint8_t* ep_desc = desc + pos;
      if (ep_desc[1] == 5) { // USB_DT_ENDPOINT
        Serial.printf("     - EP 0x%02X: type=0x%02X, maxPacket=%d\n", 
                      ep_desc[2], ep_desc[3], ep_desc[4] | (ep_desc[5] << 8));
      }
      pos += ep_desc[0];
      if (ep_desc[0] == 0) break;
    }
    
    // Accept ALL interfaces for ControlPad device (VID:0x2516 PID:0x012D)
    // We'll handle raw USB data from all endpoints
    Serial.printf("✅ USBControlPad ACCEPTING Interface %d for raw USB access!\n", iface->bInterfaceNumber);
    return true;
  }
  
  static USB_Driver* attach_interface(const usb_interface_descriptor* iface, size_t length, USB_Device* dev) {
    Serial.println("🎯 *** USBControlPad::attach_interface called ***");
    Serial.printf("   Attaching to Interface: %d\n", iface->bInterfaceNumber);
    Serial.printf("   Class: 0x%02X, SubClass: 0x%02X, Protocol: 0x%02X\n", 
                  iface->bInterfaceClass, iface->bInterfaceSubClass, iface->bInterfaceProtocol);
    
    // RAW USB MODE: Accept the first interface and create driver with full initialization
    if (!driver_instance_created) {
      driver_instance_created = true;
      
      // Create the driver instance
      USBControlPad* driver = new USBControlPad(dev);
      driver->interface = iface->bInterfaceNumber;
      
      // Configure for raw USB operation on all interfaces
      driver->setupDualInterface();
      
      // Store the instance globally so our main loop can access it
      controlPadDriver = driver;
      
      // Start the driver with the global queue - this triggers initialization and polling
      driver->begin(&controlpad_queue);
      
      Serial.printf("✅ USBControlPad RAW USB driver created with full initialization (primary: Interface %d)!\n", iface->bInterfaceNumber);
      return driver;
    } else {
      // Additional interfaces - just acknowledge
      Serial.printf("✅ Interface %d acknowledged (using existing RAW USB driver)!\n", iface->bInterfaceNumber);
      return nullptr;
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
    Serial.println("🔧 Setting up QUAD interface operation...");
    // Set fixed endpoints based on USB capture analysis
    kbd_ep_in = 0x81;    // Interface 0 keyboard input
    ctrl_ep_in = 0x83;   // Interface 1 control input  
    ctrl_ep_out = 0x04;  // Interface 1 control output
    dual_ep_in = 0x82;   // Interface 2 dual-action input
    hall_sensor_ep_in = 0x86;    // Interface 3 hall sensor data
    btn_ep_out = 0x07;   // Interface 3 alternative button events
    Serial.printf("✅ Endpoints: Kbd=0x%02X, Ctrl=0x%02X/0x%02X, Dual=0x%02X, Btn=0x%02X/0x%02X\n", 
                  kbd_ep_in, ctrl_ep_in, ctrl_ep_out, dual_ep_in, hall_sensor_ep_in, btn_ep_out);
  }
  
  bool begin(ATOM_QUEUE *q) {
    queue = q;
    if (queue != nullptr) {
      Serial.println("🎯 DRIVER BEGIN - Starting polling only, delaying activation...");
      startDualPolling();
      initialized = true;
      
      Serial.println("✅ Driver initialization complete - activation will happen after USB stabilizes");
    }
    return true;
  }
  
  bool sendUSBInterfaceReSponseActivation() {
    if (!initialized) {
      return false;
    }
    
    // Send Windows startup sequence silently
    sendWindowsCommandWithVerification(1, "42 00 activation");
    sendWindowsCommandWithVerification(2, "42 10 variant");
    sendWindowsCommandWithVerification(3, "43 00 status");
    sendWindowsCommandWithVerification(4, "41 80 status");
    sendWindowsCommandWithVerification(5, "52 00 query");
    sendWindowsCommandWithVerification(6, "41 80 repeat");
    sendWindowsCommandWithVerification(7, "52 00 final");
    
    return true;
  }
  
  bool sendWindowsCommandWithVerification(int step, const char* description) {
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
        if (attempt > 0 && attempt < 3) {
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
    Serial.printf("   - Control polling: %s\n", controlPadDriver->ctrl_polling ? "ACTIVE" : "INACTIVE");
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
    Serial.println("🔧 INITIALIZING CONTROLPAD DEVICE...");
    delay(50);
    
    Serial.println("🟡 INITIALIZING PROFILES");
    bool result2 = initializeProfiles();
    if (result2) {
      Serial.println("✅ Profile initialization successful");
    } else {
      Serial.println("⚠️ Profile initialization failed");
    }
    
    Serial.println("🎯 DEVICE INITIALIZATION COMPLETE - Starting polling...");
    startDualPolling();
    
    return true;
  }


  // NEW: Device activation command to unlock EP 0x83 data flow
  bool sendDeviceActivationCommand() {
    Serial.println("🚀 Sending DEVICE ACTIVATION command to EP 0x04 to unlock EP 0x83 data flow...");
     
    // Approach 1: Standard ControlPad LED command (known to work)
    ControlPadPacket activationPkt1;
    activationPkt1.vendor_id = 0x56;
    activationPkt1.cmd1 = 0x83;  // LED control command
    activationPkt1.cmd2 = 0x00;  // LED index 0
    memset(activationPkt1.data, 0x01, sizeof(activationPkt1.data));  // Fill with 0x01
    
    Serial.println("📤 Activation Approach 1: Standard LED command...");
    int result1 = sendControlData((uint8_t*)&activationPkt1, sizeof(activationPkt1));
    delay(50);
    
    // Approach 2: Raw button activation command
    ControlPadPacket activationPkt2;
    activationPkt2.vendor_id = 0x56;
    activationPkt2.cmd1 = 0x43;  // Button command
    activationPkt2.cmd2 = 0x01;  // Enable flag
    memset(activationPkt2.data, 0x00, sizeof(activationPkt2.data));
    
    Serial.println("📤 Activation Approach 2: Button activation command...");
    int result2 = sendControlData((uint8_t*)&activationPkt2, sizeof(activationPkt2));
    delay(50);
    
    // Approach 3: Device mode switch command
    ControlPadPacket activationPkt3;
    activationPkt3.vendor_id = 0x56;
    activationPkt3.cmd1 = 0x81;  // Mode command
    activationPkt3.cmd2 = 0x08;  // Raw mode flag
    activationPkt3.data[0] = 0x01;  // Enable
    
    Serial.println("📤 Activation Approach 3: Mode switch command...");
    int result3 = sendControlData((uint8_t*)&activationPkt3, sizeof(activationPkt3));
    delay(50);
    
    // Approach 4: Feature report activation
    uint8_t featureData[64] = {0x56, 0x00, 0x43, 0x01, 0x01, 0x00};
    Serial.println("📤 Activation Approach 4: Feature report...");
    int result4 = sendControlData(featureData, sizeof(featureData));
    delay(50);
    
    // Approach 5: Initialization sequence
    uint8_t initData[64] = {0x56, 0x82, 0x00, 0x00, 0x43, 0x01, 0xFF, 0x00};
    Serial.println("📤 Activation Approach 5: Init sequence...");
    int result5 = sendControlData(initData, sizeof(initData));
    
    Serial.printf("📊 Activation Results: %d %d %d %d %d\n", result1, result2, result3, result4, result5);
    
    if (result1 == 0 || result2 == 0 || result3 == 0 || result4 == 0 || result5 == 0) {
      Serial.println("✅ At least one activation command succeeded!");
      return true;
    } else {
      Serial.println("❌ All activation commands failed");
      return false;
    }
  }

 
  // NEW: Send HID SET_REPORT control transfer
  bool sendHIDSetReport(uint8_t interface, uint8_t reportType, uint8_t reportId, uint8_t* data, uint16_t length) {
    Serial.printf("🔧 HID SET_REPORT: Interface=%d, Type=0x%02X, ID=0x%02X, Length=%d\n", 
                  interface, reportType, reportId, length);
    
    // Print first 16 bytes of data for debugging
    Serial.print("   Data: ");
    for (int i = 0; i < min(16, (int)length); i++) {
      Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    
    // HID SET_REPORT control transfer parameters:
    // bmRequestType: 0x21 = Host-to-device, Class request, Interface recipient
    // bRequest: 0x09 = SET_REPORT
    // wValue: (reportType << 8) | reportId
    // wIndex: interface number
    // wLength: data length
    
    uint16_t wValue = (reportType << 8) | reportId;
    
    Serial.printf("   🔄 Sending USB Control Transfer: bmRequestType=0x21, bRequest=0x09, wValue=0x%04X, wIndex=%d\n", 
                  wValue, interface);
    
    // Use teensy4_usbhost ControlMessage for HID SET_REPORT
    int result = ControlMessage(0x21, 0x09, wValue, interface, length, data);
    
    if (result == 0) {
      Serial.println("   ✅ HID SET_REPORT control transfer succeeded!");
      return true;
    } else {
      Serial.printf("   ❌ HID SET_REPORT control transfer failed: result=%d\n", result);
      return false;
    }
  }

  // NEW: Test if device activation worked by doing a quick read
  void testActivationSuccess() {
    Serial.println("🔍 Testing activation success by reading EP 0x83...");
    
    auto testCallback = [](int result) {
      static bool testCompleted = false;
      if (testCompleted) return;  // Only run once
      testCompleted = true;
      
      if (result > 0) {
        Serial.printf("✅ Activation test SUCCESS: Got %d bytes from EP 0x83\n", result);
        // Don't print all data, just first few bytes for verification
        Serial.print("📦 First 8 bytes: ");
        extern uint8_t ctrl_report[64];  // Access global ctrl_report
        for (int i = 0; i < min(8, result); i++) {
          Serial.printf("%02X ", ctrl_report[i]);
        }
        Serial.println();
        
        // Check for activation success patterns
        if (ctrl_report[0] == 0x43 && ctrl_report[1] == 0x01) {
          Serial.println("🎉 PERFECT: Device is sending 43 01 button data!");
        } else if (ctrl_report[0] != 0xFF || ctrl_report[1] != 0xAA) {
          Serial.println("🎉 DEVICE ACTIVATION VERIFIED: Got non-FF-AA data on EP 0x83!");
        } else {
          Serial.println("⚠️ Still getting FF AA pattern - device may need more activation");
        }
      } else {
        Serial.printf("❌ Activation test failed: result=%d\n", result);
      }
    };
    
    // Try to read from control endpoint once
    if (queue && ctrl_ep_in != 0) {
      InterruptMessage(ctrl_ep_in, 64, ctrl_report, testCallback);
    } else {
      Serial.println("❌ Cannot test activation: queue or ctrl_ep_in not ready");
    }
  }
  
  void kbd_poll(int result) {
    static int kbd_counter = 0;
    static int no_data_counter = 0;
    
    if (result > 0 && queue) {
      kbd_counter++;
      no_data_counter = 0;  // Reset no-data counter
      
      // DEBUG: SHOW ALL KEYBOARD DATA - every single poll
      Serial.printf("⌨️ Keyboard poll #%d (%d bytes): ", kbd_counter, result);
      for (int i = 0; i < result; i++) {
          Serial.printf("0x%02X ", kbd_report[i]);
        }
        Serial.println();
      
      // CHECK FOR 43 01 PATTERN IN KEYBOARD DATA
      for (int i = 0; i < min(6, result-1); i++) {
        if (kbd_report[i] == 0x43 && kbd_report[i+1] == 0x01) {
          Serial.printf("🔥 FOUND 43 01 at position %d in KEYBOARD data!\n", i);
          
          // Check if this looks like button press format: 43 01 00 00 XX
          if (i <= result-5 && kbd_report[i+2] == 0x00 && kbd_report[i+3] == 0x00) {
            uint8_t buttonId = kbd_report[i+4];
            Serial.printf("🎯 BUTTON PRESS: ID=0x%02X from KEYBOARD interface!\n", buttonId);
            
            // Process button press here
            if (buttonId <= 0x17) {  // Valid button range 0x00-0x17
              uint8_t buttonNumber = buttonId + 1;
              Serial.printf("🎯 Triggering LED for button %d (ID=0x%02X)\n", buttonNumber, buttonId);
              controlPadDriver->sendSimpleLEDTest(buttonNumber, 255, 0, 0);  // Red
            }
          }
        }
      }
      
      // Queue keyboard event
      if (kbd_report[2] != 0) {
        Serial.printf("📤 QUEUE PUT: Added keyboard event (len=%d) to queue\n", result);
        controlpad_event event;
        memcpy(event.data, kbd_report, result);
        event.len = result;
        atomQueuePut(queue, 0, &event);

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
    } else if (result == 0) {
      // No data - could indicate HID mode is being disabled
      no_data_counter++;
      if (no_data_counter > 10) {
        Serial.println("🔍 Keyboard interface quiet - HID mode may be disabled!");
        Serial.println("🎯 This could mean raw mode is now active - check control endpoint for 43 01!");
        no_data_counter = 0;  // Reset to avoid spam
      }
      
      // Still restart polling to keep listening
      int restart = InterruptMessage(kbd_ep_in, 8, kbd_report, &kbd_poll_cb);
      if (restart != 0) {
        kbd_polling = false;
      }
    }
  }
  
  void ctrl_poll(int result) {
    static int ctrl_counter = 0;
    
    if (result > 0 && queue) {
      ctrl_counter++;
      
      // Show control packets for LED verification
      Serial.printf("🎮 Control poll #%d (%d bytes): ", ctrl_counter, result);
      for (int i = 0; i < min(16, result); i++) {
        Serial.printf("0x%02X ", ctrl_report[i]);
      }
      Serial.println();
      
      // Check for button press pattern: 43 01 00 00 XX (pressed/released)
      if (result >= 6 && ctrl_report[0] == 0x43 && ctrl_report[1] == 0x01 && 
          ctrl_report[2] == 0x00 && ctrl_report[3] == 0x00) {
        uint8_t buttonId = ctrl_report[4];
        uint8_t state = ctrl_report[5];
        
        if (state == 0xC0) {
          Serial.printf("🔴 Button pressed: 43 01 00 00 %02X C0 (ID=0x%02X)\n", buttonId, buttonId);
        } else if (state == 0x40) {
          Serial.printf("🔘 Button released: 43 01 00 00 %02X 40 (ID=0x%02X)\n", buttonId, buttonId);
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
    
    // Start polling Interface 3 Button Output (32 byte packets) - NEW!
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
    
    if (ctrl_polling) {
      Serial.println("🎯 Control endpoint is polling! Device should now send 43 01 patterns on button press!");
      Serial.println("🔥 Try pressing buttons now - should see real button events!");
    } else {
      Serial.println("⚠️ Control polling failed - button events may not work");
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

  // Implementation: Hall sensor & trigger helper functions
  bool setHallSensorGroup(uint8_t groupId, bool enable) {
    // Debug print removed to reduce serial spam
    lastHallGroup = groupId;
    uint8_t data[62] = {0};
    data[4] = groupId;
    data[5] = enable ? 0xC0 : 0x40;
    if (!sendRawCommand(0x43, 0x01, data, 62)) {
        // Failure logged internally
        return false;
    }
    return true;
  }

  uint16_t getLastHallReading() {
     uint16_t value = uint16_t(ctrl_report[1]) | (uint16_t(ctrl_report[2]) << 8);
     return value;
  }

  void getLastTriggers(uint8_t &left, uint8_t &right) {
     left = ctrl_report[5];
     right = ctrl_report[6];
     // Debug print removed to reduce serial spam
  }

  void requestAllHallSensorReadings() {
    static const uint8_t groups[4] = {0x04, 0x09, 0x02, 0x08};
    for (int i = 0; i < 4; i++) {
        setHallSensorGroup(groups[i], true);
        delay(15);
    }
  }

  // Convenience: reset button LED then enable hall on that button
  bool enableHallOnButton(uint8_t buttonIndex, uint8_t hallGroup);

  void dual_poll(int result) {
    static int dual_counter = 0;
    
    if (result > 0 && queue) {
      dual_counter++;
      
      // Debug: Show dual packets occasionally
      if (dual_counter % 50 == 1) {
        Serial.printf("🎯 Dual poll #%d: ", dual_counter);
        for (int i = 0; i < min(16, result); i++) {
          Serial.printf("0x%02X ", dual_report[i]);
        }
        Serial.println();
      }
      
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
  
  void hall_sensor_poll(int result) {
    static int hall_sensor_counter = 0;
    
    if (result > 0 && queue) {
      hall_sensor_counter++;
      
      // Debug: Show hall sensor data occasionally (this is Interface 3 - hall sensors, not button presses)
      if (hall_sensor_counter % 10 == 1) {
        Serial.printf("🔘 Hall sensor #%d: ", hall_sensor_counter);
        for (int i = 0; i < min(16, result); i++) {
          Serial.printf("0x%02X ", hall_sensor_report[i]);
        }
        Serial.println();
      }
      
      // This interface provides hall sensor data (0x00 0x14 pattern), not button press events
      // The real button press events (43 01 00 00 XX) must come from a different interface
      
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

  void sensor_out_poll(int result) {
    static int sensor_out_counter = 0;
    
    if (result > 0 && queue) {
      //sensor_out_counter++;
      
      // Show endpoint 0x07 data occasionally
      // if (sensor_out_counter % 20 == 1) {
      //   Serial.printf("🔄 EP 0x07 poll #%d: ", sensor_out_counter);
      //   for (int i = 0; i < min(16, result); i++) {
      //     Serial.printf("0x%02X ", sensor_out_report[i]);
      //   }
      //   Serial.println();
      // }
      
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

  // Note: Static declarations moved to avoid duplicates
};

// Static variable definition
bool USBControlPad::factory_registered = false;
bool USBControlPad::driver_instance_created = false;

// ===== New: Combined hall setup helper =====
bool USBControlPad::enableHallOnButton(uint8_t buttonIndex, uint8_t hallGroup) {
  Serial.printf("🔧 Enabling hall sensor group 0x%02X on button %d\n", hallGroup, buttonIndex);
  // 1) Reset that button to default via static mode
  switchToStaticMode();
  delay(10);
  // 2) Temporarily light button to default brightness (16-bit full): use raw finalize packet
  uint8_t ledData[62] = {0};
  ledData[0] = 0xFF;  // LSB brightness
  ledData[1] = 0x3F;  // MSB brightness (0x3FFF full scale)
  // send finalize command targeting this button
  sendRawCommand(0x51, buttonIndex, ledData, 2);
  delay(10);
  // 3) Reset to static mode again to latch LED state
  switchToStaticMode();
  delay(10);
  // 4) Finally select the hall sensor group for reading
  return setHallSensorGroup(hallGroup, true);
}

// ===== MAIN SETUP AND LOOP =====

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  
  Serial.println("\n🚀 Teensy4 USB Host ControlPad Button & LED Controller 🚀");
  Serial.println("==========================================================");
  Serial.println("🎯 MULTI-INTERFACE POLLING - Complete Device Control");
  Serial.println("✅ Interface 0: Keyboard input (EP 0x81 - 8-byte HID packets)");
  Serial.println("✅ Interface 1: Control/LED commands (EP 0x83/0x04 - 64-byte packets)");
  Serial.println("✅ Interface 2: Dual action input (EP 0x82 - additional input)");
  Serial.println("✅ Interface 3: Hall sensors (EP 0x86/0x07 - 32-byte packets)");
  Serial.println("✅ Button detection with LED responses via Windows activation sequence");
  Serial.println("Press buttons to see detection and hall sensor readings!");
  
  // Initialize queue for ControlPad events
  atomQueueCreate(&controlpad_queue, (uint8_t*)malloc(10 * sizeof(controlpad_event)), 10, sizeof(controlpad_event));
  
  Serial.println("📋 Queue created successfully");
  
  // Initialize USB Host
  Serial.println("🔌 Starting USB Host...");
  usbHost.begin();
  delay(2000);  // Give devices time to enumerate
  controlPadDriver->sendUSBInterfaceReSponseActivation();
  Serial.println("✅ USB Host initialized and device activated successfully");
}

void loop() {
  static unsigned long lastTime = 0;
  static int loopCounter = 0;
  
  loopCounter++;
   
  // Status update every 5 seconds
  unsigned long currentTime = millis();
  if (currentTime - lastTime > 5000) {
    lastTime = currentTime;
    
    if (controlPadDriver) {
      Serial.printf("📊 Status: kbd=%s, ctrl=%s, dual=%s, btn=%s, sensor_out=%s\n",
                    controlPadDriver->kbd_polling ? "✅" : "❌",
                    controlPadDriver->ctrl_polling ? "✅" : "❌",
                    controlPadDriver->dual_polling ? "✅" : "❌", 
                    controlPadDriver->hall_sensor_polling ? "✅" : "❌",
                    controlPadDriver->sensor_out_polling ? "✅" : "❌");
      
      if (!controlPadDriver->ctrl_polling) {
        Serial.println("🚨 CRITICAL: Control endpoint (0x83) not polling - attempting restart...");
        controlPadDriver->restartControlPolling();
      } else {
          Serial.println("✅ Control endpoint is polling");
      }
    }
  }   
  delay(10);
}
  