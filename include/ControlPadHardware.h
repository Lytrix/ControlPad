#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>
#include <teensy4_usbhost.h>
#include <string.h>  // For memset
#include "ControlPad.h"

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

// Forward declarations
class ControlPad;
class ControlPadHardware;

// ===== USB DRIVER CLASS =====
// This is the actual USB driver that inherits from USB_Driver_FactoryGlue
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
    
    uint8_t report_len = 64;
    ATOM_QUEUE* queue = nullptr;

    // Separate callbacks for different interfaces
    USBCallback kbd_poll_cb;
    USBCallback ctrl_poll_cb;
    USBCallback dual_poll_cb;
    USBCallback hall_sensor_poll_cb;
    USBCallback sensor_out_poll_cb;
    USBCallback send_cb;

    // Static tracking to prevent multiple driver instances
    static bool driver_instance_created;
    
    // Make polling state accessible for monitoring
    bool kbd_polling = false;
    bool ctrl_polling = false;
    bool dual_polling = false;
    bool hall_sensor_polling = false;
    bool sensor_out_polling = false;
    bool initialized = false;

    // Callback implementations
    void kbd_poll(int result);
    void ctrl_poll(int result);
    void dual_poll(int result);
    void hall_sensor_poll(int result);
    void sensor_out_poll(int result);
    void sent(int result);

public:
    // Constructor for USB_Driver_FactoryGlue (requires USB_Device*)
    USBControlPad(USB_Device* dev);
    
    // USB_Driver_FactoryGlue REQUIRED STATIC METHODS
    static bool offer_interface(const usb_interface_descriptor* iface, size_t length);
    static USB_Driver* attach_interface(const usb_interface_descriptor* iface, size_t length, USB_Device* dev);
    
    // USB_Driver virtual methods
    void detach() override;
    
    // USB driver functionality
    bool begin(ATOM_QUEUE *q);
    void setupQuadInterface();
    void startQuadPolling();
    bool sendUSBInterfaceReSponseActivation();
    bool sendActivationCommandsForInterface(int step, const char* description);
    
    // Friend class for hardware manager access
    friend class ControlPadHardware;
};

// ===== HARDWARE MANAGER CLASS =====
// This class manages the USB driver and provides hardware abstraction
class ControlPadHardware {
public:
    ControlPadHardware();
    ~ControlPadHardware();

    bool begin(ControlPad& pad); // Pass reference to your main API class
    void poll(); // (Optional) If you want to handle hardware polling here

    // Only called by API layer
    void setAllLeds(const ControlPadColor* colors, size_t count);

private:
    // USB driver instance (managed by this class)
    USBControlPad* usbDriver = nullptr;
    
    // Queue for events
    ATOM_QUEUE* controlpad_queue = nullptr;
    controlpad_event* controlpad_queue_data = nullptr;
};

// Global driver instance pointer (used by USB factory system)
extern USBControlPad* controlPadDriver;