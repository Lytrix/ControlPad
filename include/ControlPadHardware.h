#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>
#include <USBHost_t36.h>
#include <DMAChannel.h>
#include <EventResponder.h>
#include <string.h>  // For memset
#include "ControlPad.h"

// ===== CONTROLPAD CONSTANTS =====
#define CONTROLPAD_VID  0x2516
#define CONTROLPAD_PID  0x012D

// USB endpoints from packet analysis
#define EP_OUT          0x04  // Interrupt OUT endpoint for commands
#define EP_IN           0x83  // Interrupt IN endpoint for responses

// ===== DMA-BASED QUEUE SYSTEM =====
// Custom queue implementation using Teensy's DMA and EventResponder
template<typename T, size_t SIZE>
class DMAQueue {
private:
    T buffer[SIZE];
    volatile size_t head = 0;
    volatile size_t tail = 0;
    volatile size_t count = 0;
    DMAChannel dmaChannel;
    EventResponder eventResponder;
    void (*callback)(void) = nullptr;
    
public:
    bool begin() {
        head = 0;
        tail = 0;
        count = 0;
        return true;
    }
    
    bool put(const T& item, uint32_t timeout_ms = 0) {
        uint32_t start = millis();
        while (count >= SIZE) {
            if (timeout_ms && (millis() - start) > timeout_ms) {
                return false;
            }
            yield();
        }
        
        __disable_irq();
        buffer[head] = item;
        head = (head + 1) % SIZE;
        count++;
        __enable_irq();
        
        if (callback) {
            eventResponder.triggerEvent();
        }
        
        return true;
    }
    
    bool get(T& item, uint32_t timeout_ms = 0) {
        uint32_t start = millis();
        while (count == 0) {
            if (timeout_ms && (millis() - start) > timeout_ms) {
                return false;
            }
            yield();
        }
        
        __disable_irq();
        item = buffer[tail];
        tail = (tail + 1) % SIZE;
        count--;
        __enable_irq();
        
        return true;
    }
    
    bool isEmpty() const { return count == 0; }
    bool isFull() const { return count >= SIZE; }
    size_t available() const { return count; }
    
    void attachCallback(void (*cb)(void)) {
        callback = cb;
        // Use lambda capture to make callback accessible in the EventResponder context
        auto savedCallback = callback;
        eventResponder.attach([savedCallback](EventResponder &resp) {
            if (savedCallback) savedCallback();
        });
    }
};

// ===== CONTROLPAD PROTOCOL STRUCTURES =====
struct controlpad_event {
  uint8_t data[64];  // data[0] = length, data[1-63] = actual USB data
};

// LED Command Queue for MIDI-timing-friendly updates
struct led_command_event {
    uint8_t command_type;  // 0=Package1, 1=Package2, 2=Apply, 3=Finalize
    uint8_t data[64];      // USB command data
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

// ===== DMA LED UPDATE SYSTEM FOR MIDI TIMING =====
// Asynchronous LED updates that don't block CPU for MIDI processing
struct DMALEDUpdate {
    uint8_t package1[64];     // LED Package 1 command
    uint8_t package2[64];     // LED Package 2 command  
    uint8_t apply[64];        // Apply command
    uint8_t finalize[64];     // Finalize command
    volatile bool inProgress; // DMA transfer status
    volatile int currentCommand; // Which command (0-3) is being sent
};

// ===== USB DRIVER CLASS =====
// This is the actual USB driver that inherits from USBDriver
class USBControlPad : public USBDriver {
private:
    USBHost *myusb;
    Device_t *device_ = nullptr;  // USB device
    
    // Required USBHost_t36 member variables
    Pipe_t mypipes[7] __attribute__ ((aligned(32)));
    Transfer_t mytransfers[7] __attribute__ ((aligned(32)));
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
    DMAQueue<controlpad_event, 16>* queue = nullptr;

    // Pipe tracking for different interfaces
    Pipe_t *kbd_pipe = nullptr;
    Pipe_t *ctrl_pipe_in = nullptr;
    Pipe_t *ctrl_pipe_out = nullptr;
    Pipe_t *dual_pipe = nullptr;
    Pipe_t *hall_sensor_pipe = nullptr;
    Pipe_t *sensor_out_pipe = nullptr;
    
    // Make polling state accessible for monitoring
    bool kbd_polling = false;
    bool ctrl_polling = false;
    bool dual_polling = false;
    bool hall_sensor_polling = false;
    bool sensor_out_polling = false;
    bool initialized = false;
    
    // USB ACK synchronization system for flickerless LED updates
    volatile bool commandAckReceived = false;
    volatile int lastCommandResult = 0;
    unsigned long commandTimeout = 100; // 100ms timeout for USB ACK
    
    // Command verification system (LED, activation, effect modes)
    volatile bool ledCommandVerified = false;
    uint8_t expectedLEDEcho[2] = {0};  // Expected echo bytes for commands
    bool fastModeEnabled = false;      // Skip verification during rapid updates
    
    // USB Command Serialization using EventResponder
    EventResponder usbCommandEvent;    // Single serialization point for all USB commands
    
    // Atomic LED Update Control - Prevents USB interference during LED updates
    volatile bool atomicLEDUpdateInProgress = false;

    // Callback implementations
    void kbd_callback(const Transfer_t *transfer);
    void ctrl_in_callback(const Transfer_t *transfer);
    void ctrl_out_callback(const Transfer_t *transfer);
    void dual_callback(const Transfer_t *transfer);
    void hall_sensor_callback(const Transfer_t *transfer);
    void sensor_out_callback(const Transfer_t *transfer);

public:
    // Constructor for USBDriver
    USBControlPad(USBHost &host);
    virtual ~USBControlPad() = default;
    
    // USBDriver virtual methods
    bool claim(Device_t *device, int type, const uint8_t *descriptors, uint32_t len) override;
    void disconnect() override;
    void control(const Transfer_t *transfer) override;
    
    // USB driver functionality
    bool begin(DMAQueue<controlpad_event, 16> *q);
    void setupQuadInterface();
    void startQuadPolling();
    bool sendUSBInterfaceReSponseActivation();
    bool sendActivationCommandsForInterface(int step, const char* description);
    
    // LED control commands
    bool sendCommand(const uint8_t* data, size_t length);
    bool sendLEDCommandWithVerification(const uint8_t* data, size_t length, uint8_t expectedEcho1, uint8_t expectedEcho2);
    bool sendCommandWithVerification(const uint8_t* data, size_t length, uint8_t expectedEcho1, uint8_t expectedEcho2);
    void setFastMode(bool enabled);    // Enable/disable fast mode for rapid updates
    bool setCustomMode();
    bool setStaticMode();
    bool sendLEDPackages(const ControlPadColor* colors);  // Combined Package1+Package2 for flickerless operation
    void verifyPackageStructure();                       // Debug function to verify package coverage
    bool sendLEDPackage1(const ControlPadColor* colors); // Deprecated - use sendLEDPackages instead
    bool sendLEDPackage2(const ControlPadColor* colors); // Deprecated - use sendLEDPackages instead
    bool sendApplyCommand();
    bool sendFinalizeCommand();
    bool updateAllLEDs(const ControlPadColor* colors, size_t count);
    
    // 🚀 ATOMIC LED UPDATE IMPLEMENTATION - USB Interference Prevention
    void pauseUSBPolling();
    void resumeUSBPolling();

    // ===== QUEUE-BASED LED SYSTEM FOR MIDI TIMING =====
    // Uses DMA-based queue system with EventResponder underneath
    bool queueLEDUpdate(const ControlPadColor* colors, size_t count);
    void processLEDCommandQueue(); // Call this in main loop
    
    // ===== DMA LED UPDATE SYSTEM FOR MIDI TIMING =====
    // Non-blocking LED updates that don't interfere with MIDI timing
    bool startAsyncLEDUpdate(const ControlPadColor* colors, size_t count);
    bool isLEDUpdateInProgress();
    void processAsyncLEDUpdate(); // Call this in main loop
    
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

    // Data processing methods
    void processKeyboardData(const uint8_t* data, size_t length);
    void processDualActionData(const uint8_t* data, size_t length);  
    void processHallSensorData(const uint8_t* data, size_t length);

    // Only called by API layer
    void setAllLeds(const ControlPadColor* colors, size_t count);
    //void setFastMode(bool enabled);  // Enable/disable fast mode for rapid updates

    // Reference to the ControlPad instance for event callbacks (made public for USB callbacks)
    ControlPad* currentPad = nullptr;

    // DMA-based Queue for events
    DMAQueue<controlpad_event, 16> controlpad_queue;

    // LED Command Queue for MIDI-timing-friendly updates
    DMAQueue<led_command_event, 32> led_command_queue;
    void prepareLEDCommands(const ControlPadColor* colors, led_command_event* commands);

private:
    // No longer need local USB Host/Driver - using global objects for USBHost_t36 compatibility
    
    // DMA for LED updates
    DMAChannel ledDMAChannel;
    DMALEDUpdate dmaLEDBuffer __attribute__((aligned(32)));
    
    // Event responders for async operations
    EventResponder ledUpdateResponder;
    
    // DMA callback for LED updates
    static void dmaLEDCallback(int result);
    void prepareAsyncLEDCommands(const ControlPadColor* colors);
    void sendNextDMACommand();
};

// Global USB Host and Driver (USBHost_t36 standard pattern)
extern USBHost globalUSBHost;
extern USBControlPad globalControlPadDriver;

extern USBControlPad* controlPadDriver;
extern ControlPadHardware* globalHardwareInstance;