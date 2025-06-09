# ControlPad Button/LED Mapping & Layout - CORRECTED VERSION

## 🚨 CRITICAL: USB Protocol Understanding

**THE KEY INSIGHT:** The USB hardware sends **vertical indices directly** (0-23), not physical button numbers!

- **Physical button 1** → USB sends **buttonId=0**
- **Physical button 2** → USB sends **buttonId=5** 
- **Physical button 5** → USB sends **buttonId=20**

**NO CONVERSION NEEDED** - just use the USB buttonId as the verticalIndex directly.

## Overview

This document describes the **correct** button and LED ordering system used in the ControlPad project. After extensive debugging, we discovered the USB protocol uses column-major (vertical) indexing, not row-major physical indexing.

## Hardware Specifications

### Button Array Configuration
- **Total Buttons:** 24 buttons
- **Physical Labels:** 1-24 (as printed on device)
- **USB Protocol:** Sends vertical indices 0-23 directly (column-major order)
- **Button Type:** Hall effect sensors with LED backlighting  
- **Layout:** 5×5 grid (button 24 is double-wide but still at row 5, col 4)

### Device Identification
- **USB VID:** 0x2516
- **USB PID:** 0x012D
- **Device Class:** Custom HID device with multiple interfaces

## Physical vs USB Mapping

### Physical Layout (What You See)
```
Physical Layout (Top View):
┌─────┬─────┬─────┬─────┬─────┐
│  1  │  2  │  3  │  4  │  5  │  Row 1
├─────┼─────┼─────┼─────┼─────┤
│  6  │  7  │  8  │  9  │ 10  │  Row 2
├─────┼─────┼─────┼─────┼─────┤
│ 11  │ 12  │ 13  │ 14  │ 15  │  Row 3
├─────┼─────┼─────┼─────┼─────┤
│ 16  │ 17  │ 18  │ 19  │ 20  │  Row 4
├─────┼─────┼─────┼─────┴─────┤
│ 21  │ 22  │ 23  │     24    │  Row 5
└─────┴─────┴─────┴───────────┘
```

### USB Index Layout (What Hardware Sends)
```
USB Index Layout (Column-Major Order):
┌─────┬─────┬─────┬─────┬─────┐
│  0  │  5  │ 10  │ 15  │ 20  │  Row 1
├─────┼─────┼─────┼─────┼─────┤
│  1  │  6  │ 11  │ 16  │ 21  │  Row 2
├─────┼─────┼─────┼─────┼─────┤
│  2  │  7  │ 12  │ 17  │ 22  │  Row 3
├─────┼─────┼─────┼─────┼─────┤
│  3  │  8  │ 13  │ 18  │ 23  │  Row 4
├─────┼─────┼─────┼─────┴─────┤
│  4  │  9  │ 14  │     19    │  Row 5
└─────┴─────┴─────┴───────────┘
```

### Exact Physical→USB Mapping Table
| Physical Button | USB buttonId | Row | Col | Notes |
|----------------|--------------|-----|-----|-------|
| 1              | 0            | 1   | 1   | |
| 2              | 5            | 1   | 2   | |
| 3              | 10           | 1   | 3   | |
| 4              | 15           | 1   | 4   | |
| 5              | 20           | 1   | 5   | |
| 6              | 1            | 2   | 1   | |
| 7              | 6            | 2   | 2   | |
| 8              | 11           | 2   | 3   | |
| 9              | 16           | 2   | 4   | |
| 10             | 21           | 2   | 5   | |
| 11             | 2            | 3   | 1   | |
| 12             | 7            | 3   | 2   | |
| 13             | 12           | 3   | 3   | |
| 14             | 17           | 3   | 4   | |
| 15             | 22           | 3   | 5   | |
| 16             | 3            | 4   | 1   | |
| 17             | 8            | 4   | 2   | |
| 18             | 13           | 4   | 3   | |
| 19             | 18           | 4   | 4   | |
| 20             | 23           | 4   | 5   | |
| 21             | 4            | 5   | 1   | |
| 22             | 9            | 5   | 2   | |
| 23             | 14           | 5   | 3   | |
| 24             | 19           | 5   | 4   | Double-wide |

## Software Implementation

### ⚠️ CORRECT Hardware Layer Implementation
```cpp
// In ControlPadHardware.cpp - Button event processing
if (buttonId >= 0 && buttonId <= 23) {  // USB sends vertical indices 0-23 directly
    // USB already sends vertical index (column-major order) - no conversion needed!
    uint8_t verticalIndex = buttonId;
    
    // Convert vertical index to physical position for debugging only
    uint8_t col = verticalIndex / 5;  // Column (0-4)
    uint8_t row = verticalIndex % 5;  // Row (0-4)
    uint8_t physicalIndex = row * 5 + col;  // Physical index (0-23)
    uint8_t physicalButton = physicalIndex + 1;  // Physical button label (1-24)
    
    // Create event using verticalIndex directly
    ControlPadEvent event;
    event.type = ControlPadEventType::Button;
    event.button.button = verticalIndex;  // Pass USB buttonId as-is
    
    // Send to main application
    currentPad->pushEvent(event);
}
```

### ✅ CORRECT Main Application Layer
```cpp
// In main.cpp - Handle button events
void handleButtonEvent(const ControlPadEvent& event) {
    uint8_t verticalIndex = event.button.button;  // Already correct from hardware
    
    // Convert to physical index for button state tracking
    uint8_t col = verticalIndex / 5;
    uint8_t row = verticalIndex % 5;
    uint8_t physicalIndex = row * 5 + col;
    
    // Update button state using physical indexing
    buttonStates[physicalIndex] = event.button.pressed;
    
    // Your application logic here using physicalIndex...
}
```

### LED Control - Physical Order Expected
```cpp
// ControlPad.setAllButtonColors() expects colors in PHYSICAL order (0-23)
ControlPadColor colors[24];
for (int i = 0; i < 24; i++) {
    colors[i] = {255, 0, 0};  // All red, indexed by physical button order
}
controlPad.setAllButtonColors(colors);  // Hardware will convert to vertical order
```

## 🔧 Conversion Functions

### USB → Physical Conversion
```cpp
// Convert USB buttonId (vertical index) to physical button info
struct ButtonInfo {
    uint8_t physicalButton;  // 1-24
    uint8_t physicalIndex;   // 0-23
    uint8_t row;            // 0-4
    uint8_t col;            // 0-4
};

ButtonInfo usbToPhysical(uint8_t usbButtonId) {
    ButtonInfo info;
    info.col = usbButtonId / 5;
    info.row = usbButtonId % 5;
    info.physicalIndex = info.row * 5 + info.col;
    info.physicalButton = info.physicalIndex + 1;
    return info;
}
```

### Physical → USB Conversion
```cpp
// Convert physical button number to USB buttonId (vertical index)
uint8_t physicalToUSB(uint8_t physicalButton) {
    if (physicalButton < 1 || physicalButton > 24) return 0xFF;
    
    uint8_t physicalIndex = physicalButton - 1;  // 0-23
    uint8_t row = physicalIndex / 5;
    uint8_t col = physicalIndex % 5;
    uint8_t verticalIndex = col * 5 + row;
    
    return verticalIndex;
}
```

## 🚨 Common Mistakes to Avoid

1. **Don't convert USB buttonId** - it's already the correct vertical index
2. **Don't subtract 1 from USB buttonId** - button 1 sends buttonId=0, which is correct
3. **Don't filter out buttonId=0** - that's physical button 1!
4. **Use physical indexing for application logic** - convert vertical→physical for button states
5. **Colors array uses physical order** - hardware converts to vertical for USB transmission

## Debug Output Reference

When working correctly, button presses should show:
```
🔧 USB buttonId=0 is already verticalIndex, converts to physical button 1 (index 0)
🔧 USB buttonId=5 is already verticalIndex, converts to physical button 2 (index 1)
🔧 USB buttonId=20 is already verticalIndex, converts to physical button 5 (index 4)
```

## Architecture Summary

**USB Hardware:** Sends vertical indices (0-23) directly - no conversion needed
**Hardware Layer:** Passes USB buttonId as verticalIndex to application
**Main Application:** Converts vertical→physical for button state tracking and intuitive logic
**LED Control:** Uses physical color array order, hardware converts to vertical for USB transmission

This architecture provides the most intuitive behavior while respecting the USB protocol's column-major ordering. 