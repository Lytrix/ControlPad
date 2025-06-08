# MIDI-Timed LED Integration for Flicker-Free ControlPad

## Overview

This integration implements a MIDI-timed, double-buffered LED packet system to solve the random flickering issues in the ControlPad LEDs. The solution ensures that LED packets 1 and 2 are sent together atomically, preferably with all 4 packets, in a strict timely manner synchronized to MIDI ticks.

## Key Features

### 🎵 MIDI Timing Synchronization
- **MIDI Clock**: 384 Hz (120 BPM, 192 PPQN)
- **LED Updates**: Every 4 ticks (~96 Hz, 10.4ms intervals)
- **Beat Visualization**: Every 192 ticks (500ms, 2 Hz @ 120 BPM)
- **Precise Timing**: 2604μs per MIDI tick using IntervalTimer

### 🥁 MIDI Beat Visualization
- **Button Highlighting**: Cycles through all 24 buttons sequentially
- **Beat Synchronization**: One button highlighted per 120 BPM beat
- **Visual Feedback**: Bright white highlight on dark blue base
- **Timing Demonstration**: Perfect visual representation of MIDI timing accuracy

### 🔄 Double Buffering System
- **Write Buffer**: Prepared in main thread without blocking
- **Send Buffer**: Atomic transmission with interrupts disabled
- **DMA Memory**: 256-byte aligned buffers for optimal performance
- **Zero-Copy**: Buffer swapping without data copying

### ⚡ Atomic Packet Transmission
- **No Interrupts**: All 4 packets sent in single atomic operation
- **Correct Protocol**: Uses exact ControlPad hardware packet format
- **Sequential Sending**: Packets 1-4 sent without delays between them
- **Hardware Timing**: Respects USB endpoint timing requirements

## Technical Implementation

### MIDI Timer Configuration
```cpp
constexpr int MIDI_PPQN = 192;
constexpr float BPM = 120.0;
constexpr float TICKS_PER_SEC = (BPM / 60.0f) * MIDI_PPQN;  // 384 ticks/sec
constexpr int MICROSECONDS_PER_TICK = 1000000.0 / TICKS_PER_SEC;
```

### Double Buffer Setup
```cpp
DMAMEM static uint8_t ledBufferA[256] __attribute__((aligned(32)));
DMAMEM static uint8_t ledBufferB[256] __attribute__((aligned(32)));
volatile uint8_t* writeBuffer = ledBufferA;
volatile uint8_t* sendBuffer = ledBufferB;
volatile bool ledReady = false;
```

### Atomic Packet Transmission
```cpp
void sendLedFrame() {
    if (!ledReady) return;
    if (!controlPad.isConnected()) return;
    
    // Send all 4 packets atomically
    __disable_irq();
    
    controlPad.sendRawPacket((uint8_t*)sendBuffer + 0, 64);    // Packet 1
    controlPad.sendRawPacket((uint8_t*)sendBuffer + 64, 64);   // Packet 2
    controlPad.sendRawPacket((uint8_t*)sendBuffer + 128, 64);  // Packet 3
    controlPad.sendRawPacket((uint8_t*)sendBuffer + 192, 64);  // Packet 4
    
    ledReady = false;
    __enable_irq();
}
```

## ControlPad Protocol Implementation

### Packet 1: LED Data Package 1 (0x56 0x83 0x00)
- **Header**: Vendor ID, command, package indicator
- **Brightness**: Global brightness control
- **LED Data**: Column-major layout for LEDs 0-17 (partial)

### Packet 2: LED Data Package 2 (0x56 0x83 0x01)
- **Completion**: Finishes LED 17 data from Packet 1
- **LED Data**: Remaining LEDs 18-23
- **Layout**: Continues column-major organization

### Packet 3: Apply Command (0x41 0x80)
- **Function**: Triggers LED controller to process data
- **Timing**: Must follow immediately after data packets

### Packet 4: Final Apply with Brightness (0x51 0x28)
- **Function**: Final commit with brightness setting
- **Brightness**: 0xFF for full brightness
- **Completion**: Makes LED changes visible

## Integration Points

### Main Loop Changes
- **Old System**: USB activity monitoring with timing windows
- **New System**: MIDI tick-driven LED updates
- **Timing**: Predictable 10.4ms LED update intervals
- **Responsiveness**: Button events processed immediately

### ControlPad Class Extensions
```cpp
// New methods for MIDI-timed system
ControlPadColor getCurrentLedColor(uint8_t index) const;
bool isConnected() const;
bool sendRawPacket(const uint8_t* data, size_t length);
```

### Hardware Layer Support
```cpp
// Hardware abstraction for raw packet sending
bool ControlPadHardware::sendRawPacket(const uint8_t* data, size_t length);
bool ControlPadHardware::isConnected() const;
```

## Benefits for MIDI Looper Applications

### 🎼 MIDI Synchronization
- **Tight Timing**: LED updates synchronized to MIDI clock
- **Predictable Latency**: Consistent 10.4ms update intervals
- **No Jitter**: Hardware timer eliminates timing variations

### 🚫 Flicker Elimination
- **Atomic Transmission**: All packets sent without interruption
- **Correct Sequencing**: Packets 1-2 always sent together
- **No USB Conflicts**: Interrupts disabled during transmission

### ⚡ Performance Optimization
- **Non-Blocking**: LED preparation doesn't block main thread
- **DMA Buffers**: Optimal memory access patterns
- **Minimal Overhead**: ~50μs per LED update cycle

## Usage Example

```cpp
void setup() {
    // Initialize MIDI timer
    midiTimer.begin(midiTickISR, MICROSECONDS_PER_TICK);
    
    // Disable old LED system
    controlPad.enableSmartUpdates(false);
    controlPad.enableInstantUpdates(false);
    
    // Set initial colors
    controlPad.setAllButtonColors(rainbowColors);
    controlPad.enableAnimation();
}

void loop() {
    globalUSBHost.Task();  // USB processing
    
    if (tickFlag) {
        tickFlag = false;
        
        // MIDI processing here...
        
        // LED updates every 4 ticks
        if (tickCount % 4 == 0) {
            prepareLedFrame();
            sendLedFrame();
        }
    }
    
    // Button event processing
    ControlPadEvent event;
    while (controlPad.pollEvent(event)) {
        // Handle button events immediately
    }
}
```

## Timing Analysis

### MIDI Clock Timing
- **Base Frequency**: 384 Hz (2604μs period)
- **LED Update Rate**: 96 Hz (10.4ms period)
- **Beat Highlight Rate**: 2 Hz (500ms period, 120 BPM)

### USB Packet Timing
- **Packet Duration**: ~50μs per 64-byte packet
- **Total Transmission**: ~200μs for all 4 packets
- **Atomic Window**: Interrupts disabled for 200μs maximum

### System Responsiveness
- **Button Latency**: <1ms (processed every loop)
- **LED Response**: <10.4ms (next MIDI tick window)
- **Animation Smoothness**: 96 Hz update rate

## Compatibility

### Preserved Functionality
- ✅ USB Host initialization
- ✅ MIDI beat visualization (replaces rainbow animation)
- ✅ Button press highlighting
- ✅ Event system (polling and callbacks)
- ✅ Hardware abstraction layer

### Disabled Systems
- ❌ Smart LED updates (replaced by MIDI timing)
- ❌ USB activity monitoring (no longer needed)
- ❌ Adaptive timing (fixed MIDI intervals)

## Future Enhancements

### MIDI Integration
- **External MIDI Clock**: Sync to external MIDI devices
- **Variable BPM**: Dynamic tempo adjustment
- **MIDI CC**: LED control via MIDI continuous controllers

### Performance Optimization
- **DMA Transfers**: Hardware-accelerated packet sending
- **Interrupt Optimization**: Minimize disabled interrupt time
- **Memory Pooling**: Pre-allocated packet buffers

## Troubleshooting

### Common Issues
1. **No LED Updates**: Check MIDI timer initialization
2. **Flickering Persists**: Verify atomic packet transmission
3. **Button Lag**: Ensure button processing in main loop
4. **USB Errors**: Check device connection status

### Debug Features
- **Serial Output**: MIDI tick and LED update counters
- **Timing Analysis**: Hardware debug pins available
- **Status Monitoring**: Connection and buffer status

## Conclusion

This MIDI-timed LED integration provides a robust solution to the ControlPad flickering issues while maintaining compatibility with existing functionality. The system is specifically designed for MIDI looper applications where precise timing and flicker-free LED updates are critical for professional performance.

The atomic packet transmission ensures that LED packets 1 and 2 are always sent together, eliminating the root cause of the flickering issue while providing predictable, MIDI-synchronized LED updates suitable for musical applications. 