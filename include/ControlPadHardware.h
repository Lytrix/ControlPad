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

// ===== TEENSY EVENTRESPONDER QUEUE SYSTEM =====
// Use Teensy's built-in EventResponder for atomic queue operations

struct QueuedLEDCommand {
    uint8_t data[64];
    size_t length;
    uint8_t commandType;    // 1 = pkg1, 2 = pkg2, etc
    bool priority;
};

// ===== SIMPLE PACKET QUEUE =====
// Clean, simple queue for 64-byte LED command packets
class LEDPacketQueue {
private:
    static const size_t MAX_PACKETS = 32;  // Larger queue to handle bursts of 4 packets
    uint8_t packets[MAX_PACKETS][64];      // Store 64-byte command packets
    volatile uint8_t head = 0;
    volatile uint8_t tail = 0;
    volatile uint8_t count = 0;
    
public:
    LEDPacketQueue() : head(0), tail(0), count(0) {}

    bool enqueue(const uint8_t* packet) {
        __disable_irq();
        if (count >= MAX_PACKETS) {
            __enable_irq();
            return false;  // Queue full
        }
        
        memcpy(packets[tail], packet, 64);
        tail = (tail + 1) % MAX_PACKETS;
        count++;
        __enable_irq();
        return true;
    }

    bool dequeue(uint8_t* packet) {
        __disable_irq();
        if (count == 0) {
            __enable_irq();
            return false;  // Queue empty
        }
        
        memcpy(packet, packets[head], 64);
        head = (head + 1) % MAX_PACKETS;
        count--;
        __enable_irq();
        return true;
    }

    bool isFull() const { 
        __disable_irq();
        bool result = (count == MAX_PACKETS);
        __enable_irq();
        return result;
    }
    
    bool isEmpty() const { 
        __disable_irq();
        bool result = (count == 0);
        __enable_irq();
        return result;
    }
    
    uint8_t size() const { 
        __disable_irq();
        uint8_t result = count;
        __enable_irq();
        return result;
    }
};

// ===== LED TIMING CONTROLLER =====
// Handles precise 1ms timing for LED packet transmission
class LEDTimingController {
private:
    LEDPacketQueue& queue;
    uint32_t lastSendTime;
    uint32_t sendIntervalMicros;  // Make this configurable instead of const
    bool enabled;
    
public:
    LEDTimingController(LEDPacketQueue& q) : queue(q), lastSendTime(0), sendIntervalMicros(12500), enabled(false) {}  // DISABLED - bypassing queue
    
    // Call this regularly from main loop
    void processTimedSending();  // Implementation moved to .cpp file
    
    // Change timing interval if needed
    void setIntervalMicros(uint32_t micros) {
        sendIntervalMicros = micros;
    }
    
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() const { return enabled; }
    
    // Get queue status
    uint8_t getQueueSize() const { return queue.size(); }
    bool isQueueEmpty() const { return queue.isEmpty(); }
    bool isQueueFull() const { return queue.isFull(); }
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

// ===== USB HID INPUT CLASS =====
// This inherits from USBHIDInput to work WITH HID parsers, not compete against them
class USBControlPad : public USBHIDInput {
private:
    USBHost *myusb;
    Device_t *device_ = nullptr;  // USB device (old USBDriver interface)
    
    // USBHIDInput interface variables
    Device_t *mydevice = nullptr;  // Device for USBHIDInput interface
    USBHIDParser *driver_ = nullptr;  // HID parser driver
    uint32_t usage_ = 0;  // HID top-level usage
    
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
    void* queue = nullptr;  // Legacy queue pointer (unused with EventResponder)

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
    // USBHIDInput interface methods  
    virtual hidclaim_t claim_collection(USBHIDParser *driver, Device_t *dev, uint32_t topusage);
    virtual void disconnect_collection(Device_t *dev);
    virtual void hid_input_begin(uint32_t topusage, uint32_t type, int lgmin, int lgmax);
    virtual void hid_input_data(uint32_t usage, int32_t value);
    virtual void hid_input_end();
    virtual bool hid_process_in_data(const Transfer_t *transfer);
    virtual bool hid_process_out_data(const Transfer_t *transfer);
    
private:
    void init();
    
public:
    // USB driver functionality
    bool begin();  // Simplified - no queue parameter needed
    bool sendActivationSequence();
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
    bool updateAllLEDs(const ControlPadColor* colors, size_t count, bool priority = false, uint32_t retryStartTime = 0);
    
    // 🚀 ATOMIC LED UPDATE IMPLEMENTATION - USB Interference Prevention
    void pauseUSBPolling();
    void resumeUSBPolling();
    
    // ===== USB DEVICE STATE MONITORING =====
    bool isDeviceConnected() const { return device_ != nullptr; }
    Device_t* getDevice() const { return device_; }

    // ===== QUEUE-BASED LED SYSTEM FOR MIDI TIMING =====
    // Uses EventResponder-based queue system
    bool queueLEDUpdate(const ControlPadColor* colors, size_t count);
    void processLEDCommandQueue(); // Call this in main loop
    
    // ===== DMA LED UPDATE SYSTEM FOR MIDI TIMING =====
    // Non-blocking LED updates that don't interfere with MIDI timing
    bool startAsyncLEDUpdate(const ControlPadColor* colors, size_t count);
    bool isLEDUpdateInProgress();
    
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
    bool setAllLeds(const ControlPadColor* colors, size_t count);
    
    // *** ANIMATION SYSTEM ***
    void enableAnimation();
    void disableAnimation();
    void updateAnimation();
    void updateButtonHighlights();
    void updateUnifiedLEDs();
    bool isAnimationEnabled() const;

    // Reference to the ControlPad instance for event callbacks (made public for USB callbacks)
    ControlPad* currentPad = nullptr;

    // ===== SIMPLIFIED EVENT HANDLING =====
    // Using EventResponder instead of DMAQueue
    void processControlPadEvent(const uint8_t* data, size_t length);
    void prepareLEDCommands(const ControlPadColor* colors, led_command_event* commands);

private:
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

// ===== LED QUEUE MONITORING FUNCTIONS =====
void getLEDQueueStatus(size_t* queueSize, bool* isProcessing);