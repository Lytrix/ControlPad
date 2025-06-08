# Predictive followup_Error() Detection System

## Overview

This system provides real-time monitoring and prediction of USB Host error conditions that trigger the `followup_Error()` function, along with detailed tracking of memory cleanup events. It helps predict when USB memory will be freed, allowing for better coordination of USB packet queuing and LED updates.

## Features

- **🎯 Real-time USB register monitoring** - 50ms polling of hardware registers
- **💾 Memory cleanup prediction** - Predicts when and what memory will be freed
- **📊 Comprehensive error detection** - 9 different error condition types
- **🔗 Queue management tracking** - Monitors USB transfer queue changes
- **📈 Memory usage monitoring** - Tracks actual RAM allocation changes

## Architecture

### Core Components

1. **USBSynchronizedPacketController::monitorUSBActivity()** - Main monitoring loop
2. **USBSynchronizedPacketController::trackMemoryCleanup()** - Memory tracking
3. **USBSynchronizedPacketController::onFollowupErrorCalled()** - Error event handler

### Hardware Registers Monitored

| Register | Purpose | Address |
|----------|---------|---------|
| `USBHS_USBSTS` | USB Status Register | Status and interrupt flags |
| `USBHS_USBCMD` | USB Command Register | Controller commands and state |
| `USBHS_FRINDEX` | Frame Index Register | Current USB frame number |
| `USBHS_PERIODICLISTBASE` | Periodic List Base | Periodic transfer queue pointer |
| `USBHS_ASYNCLISTADDR` | Async List Address | Async transfer queue pointer |

## Error Conditions Detected

### 1. USB Error Interrupt (UEI) - Critical
```
🚨 USB ERROR INTERRUPT ACTIVE! followup_Error() will be called NOW
   💾 MEMORY CLEANUP: Transfer descriptors being freed
```
- **Trigger**: `USBHS_USBSTS` bit 0x02 set
- **Action**: Immediate `followup_Error()` call
- **Memory Impact**: Transfer descriptors freed

### 2. Port Change Detect (PCI) - Device Events
```
🔌 PORT CHANGE DETECTED! Device enumeration/cleanup in progress
   💾 MEMORY CLEANUP: Device descriptors being reallocated
```
- **Trigger**: `USBHS_USBSTS` bit 0x04 set
- **Action**: Device re-enumeration
- **Memory Impact**: Device descriptor structures reallocated

### 3. System Error (SEI) - Critical
```
🚨 USB SYSTEM ERROR! followup_Error() and memory cleanup required
   💾 MEMORY CLEANUP: Full USB stack reset imminent
```
- **Trigger**: `USBHS_USBSTS` bit 0x10 set
- **Action**: Complete USB stack reset
- **Memory Impact**: All USB memory structures cleared

### 4. Async Schedule Advance (AAI) - Queue Management
```
🔄 ASYNC SCHEDULE ADVANCE! Queue management in progress
   💾 MEMORY CLEANUP: Transfer queues being reorganized
```
- **Trigger**: `USBHS_USBSTS` bit 0x20 set
- **Action**: Async transfer queue reorganization
- **Memory Impact**: Queue elements freed/reallocated

### 5. Host Controller Reset - Major Cleanup
```
🔄 HOST CONTROLLER RESET INITIATED! Major memory cleanup starting
   💾 MEMORY CLEANUP: All USB structures being cleared
```
- **Trigger**: `USBHS_USBCMD` bit 0x02 transition 0→1
- **Action**: Complete controller reset
- **Memory Impact**: All USB memory cleared

### 6. Host Controller Halt - Recovery Mode
```
🚨 USB HOST HALTED! Controller in recovery mode
   💾 MEMORY CLEANUP: Pending transfers being aborted
```
- **Trigger**: `USBHS_USBCMD` bit 0x01 = 0
- **Action**: Controller stopped, transfers aborted
- **Memory Impact**: Pending transfer memory freed

### 7. Transfer Stalls - Early Warning
```
⚠️ USB TRANSFER STALL detected - followup_Error() may be needed soon
   📊 Frames since activity: 150 (stall count: 3)
```
- **Trigger**: No USB interrupt for >100 frames
- **Action**: Potential error condition developing
- **Memory Impact**: May lead to transfer cleanup

### 8. Schedule Status Changes - Queue Cleanup
```
📋 PERIODIC SCHEDULE DISABLED! Cleanup of periodic transfers
   💾 MEMORY CLEANUP: Periodic transfer descriptors freed
```
- **Trigger**: Periodic/Async schedule status changes
- **Action**: Schedule-specific cleanup
- **Memory Impact**: Descriptor pools freed

### 9. Doorbell Operations - Queue Advance
```
🚪 ASYNC QUEUE DOORBELL! Manual queue advance detected
   💾 MEMORY CLEANUP: Queue pointers being updated (0x12345678 → 0x87654321)
```
- **Trigger**: `USBHS_ASYNCLISTADDR` changes
- **Action**: Manual queue pointer update
- **Memory Impact**: Queue element reorganization

## Memory Cleanup Tracking

### Real-time Memory Monitoring
The system continuously tracks free RAM and detects cleanup events:

```
💾 MEMORY CLEANUP DETECTED! +2048 bytes freed (total events: 5)
   📊 Free RAM: 245760 bytes (was 243712)
```

### Enhanced Memory Cleanup Detection (v2.0)
Real-time tracking of actual memory cleanup events:

```
🧹 MEMORY CLEANUP DETECTED! +2048 bytes freed (cleanup #3)
   📊 RAM: 243712 → 245760 bytes free
```

Memory leak detection:
```
🚨 MEMORY LEAK DETECTED: -4096 bytes trend over 1 second
```

USB frame jump detection (indicates USB reset):
```
🔄 USB FRAME JUMP DETECTED: 5000 frames (12000 → 17000)
   🧹 This usually indicates USB cleanup/reset occurred
```

### Device State Change Detection
Critical for catching actual disconnection events:

```
🔌 USB DEVICE STATE CHANGE: 0x00001005 → 0x00000005
   📤 DEVICE DISCONNECTED - Major cleanup expected!
   🎨 FLICKER RISK: LED updates should be paused briefly
```

### Memory Cleanup Prediction
When multiple conditions indicate imminent cleanup:

```
🔥 MEMORY CLEANUP IMMINENT! followup_Error() will free descriptors
   📊 USB State: Status=0x0000C088, Command=0x00018B35, Frame=12160
   🔗 Queues: Periodic=0x12345678, Async=0x87654321
   💾 Will free: Error transfer descriptors
   💾 Will free: Async queue elements
```

## Usage Examples

### Basic Monitoring
The system runs automatically when `usbSyncController.monitorUSBActivity()` is called in the main loop:

```cpp
void loop() {
    // Monitor USB activity and predict followup_Error() calls
    usbSyncController.monitorUSBActivity();
    
    // Your USB operations here
    globalUSBHost.Task();
    
    // LED updates coordinated with USB activity
    controlPad.updateUnifiedLEDs();
}
```

### Responding to Predictions
Use the detection to coordinate USB queue management:

```cpp
// In your LED update code
if (memoryCleanupImminent) {
    // Reduce queue depth before cleanup
    ledQueue.reserveSpace(10);
    
    // Wait for cleanup to complete
    delay(5);
}
```

### Manual Error Tracking
Call when you detect `followup_Error()` manually:

```cpp
// If you have access to followup_Error() calls
void yourFollowupErrorWrapper() {
    usbSyncController.onFollowupErrorCalled();
    // ... original followup_Error() code
}
```

## Serial Output Reference

### Status Indicators

| Symbol | Meaning |
|--------|---------|
| 🚨 | Critical error requiring immediate action |
| ⚠️ | Warning condition that may lead to errors |
| 🔄 | Queue/schedule management activity |
| 🔌 | Device connection/enumeration events |
| 💾 | Memory cleanup activity |
| 📊 | Status information and metrics |
| 🔗 | Queue pointer and structure changes |
| 🚪 | Manual queue operations (doorbell) |

### Timing Information
- **Frame numbers**: USB frames at 8KHz (125µs each)
- **Status checks**: Every 50ms for responsive detection
- **Memory checks**: Every 100ms for efficient monitoring
- **Reports**: Every 30 seconds for status summary

## Integration with LED Queue Management

### Coordination Strategy
1. **Prediction Phase**: Monitor for error condition indicators
2. **Preparation Phase**: Reserve queue space when cleanup imminent
3. **Cleanup Phase**: Pause LED updates during memory cleanup
4. **Recovery Phase**: Resume normal operations after cleanup

### Example Coordination Code
```cpp
// In your LED update logic
bool isSafeForLEDUpdate() {
    // Check if USB error conditions are active
    if (errorConditionsDetected) {
        return false;  // Wait for cleanup
    }
    
    // Check if memory cleanup is happening
    if (memoryCleanupImminent) {
        return false;  // Pause during cleanup
    }
    
    return true;  // Safe to proceed
}
```

## Configuration Options

### Polling Intervals
```cpp
// In monitorUSBActivity()
static uint32_t lastErrorCheck = 0;
if (currentTime - lastErrorCheck >= 50) {  // 50ms for fast response
```

### Memory Cleanup Thresholds
```cpp
// In trackMemoryCleanup()
if (freeRAM > lastFreeRAM + 1024) {  // 1KB threshold
```

### Stall Detection Sensitivity
```cpp
// In transfer stall detection
if (framesSinceActivity > 100) {  // ~12.5ms at 8KHz
```

## Troubleshooting

### No Detection Output
1. Verify `monitorUSBActivity()` is called in main loop
2. Check USB Host is properly initialized
3. Ensure device is connected and active

### False Positives
1. Increase stall detection threshold
2. Adjust memory cleanup threshold
3. Add filtering for noise conditions

### Memory Tracking Issues
1. Verify Teensy platform defines (`__MK66FX1M0__`)
2. Check heap/stack configuration
3. Monitor for memory fragmentation

### Performance Impact
- **CPU Usage**: ~0.1% with 50ms polling
- **Memory Usage**: ~100 bytes for tracking variables
- **Serial Output**: Can be disabled by removing `Serial.printf()` calls

## Future Enhancements

1. **Adaptive Thresholds**: Adjust detection sensitivity based on USB activity patterns
2. **Queue Integration**: Direct integration with LED command queues
3. **Statistics Collection**: Long-term error pattern analysis
4. **Recovery Strategies**: Automatic recovery procedures for different error types
5. **Performance Profiling**: Detailed timing analysis of cleanup operations

## Technical Notes

- Compatible with USBHost_t36 library on Teensy 4.x
- Requires access to USBHS hardware registers
- Designed for real-time embedded applications
- Thread-safe for interrupt-driven USB callbacks
- Low overhead suitable for high-frequency LED updates 