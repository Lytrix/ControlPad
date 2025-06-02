# ControlPad LED Integration for MIDI Looper

Complete integration guide for adding professional ControlPad LED control to your [MIDI Looper project](https://github.com/lytrix/midilooper).

## 🎯 Overview

This integration adds:
- **Individual LED control** for all 24 RGB buttons based on track states
- **Measure-synchronized pulsing** for the record button (Button 24)
- **Visual feedback** for recording, playing, overdubbing, and muted states
- **Beautiful baseline lighting** when inactive
- **Future-ready** for multiple clips per track

## 🚀 Quick Start

### 1. Add Files to Your Project

Copy these files to your MIDI looper project:

```
YourMidiLooper/
├── include/
│   └── ControlPadLEDManager.h
├── src/
│   └── ControlPadLEDManager.cpp
└── examples/
    └── MidiLooperIntegration.cpp
```

### 2. Update Your Main Code

```cpp
#include "ControlPadLEDManager.h"

// Add global LED manager
ControlPadLEDManager ledManager;

void setup() {
    // Your existing setup...
    
    // Initialize ControlPad LEDs
    if (controlPadDriver && controlPadDriver->isInitialized()) {
        ledManager.initialize(controlPadDriver);
        ledManager.setBPM(120.0f);  // Set initial BPM
        ledManager.showStartupAnimation();
    }
}

void loop() {
    // Your existing loop...
    
    // Update LED animations (call every loop)
    ledManager.update();
}
```

### 3. Connect to Your State Machine

In your track state change handlers:

```cpp
void onTrackStateChange(uint8_t trackIndex, TrackState newState) {
    // Your existing logic...
    
    // Update LEDs
    ledManager.setTrackState(trackIndex, newState);
}

void onStartRecording(uint8_t trackIndex) {
    // Your existing logic...
    ledManager.onTrackStartRecording(trackIndex);
}

void onMeasureBeat(uint8_t beatNumber) {
    // Your existing timing logic...
    ledManager.onMeasureBeat(beatNumber);
}
```

## 🎛️ Button Layout

```
[T1] [T2] [T3] [T4] [St]    Track buttons + Stop
[C1] [C2] [C3] [C4] [Cl]    Future clips + Clear  
[C1] [C2] [C3] [C4] [--]    Future clips
[C1] [C2] [C3] [C4] [--]    Future clips
[C1] [C2] [C3] [Rc] [Pl]    Future clips + Record + Play
```

- **T1-T4**: Track buttons (1-4) - main looper controls
- **Rc**: Record button (24) - pulses with measures
- **Pl**: Play button (25) - center button (no LED)
- **St**: Stop button (5)
- **Cl**: Clear button (10)
- **C1-C4**: Future clip slots per track

## 🎨 LED States & Colors

| Track State | LED Color | Animation | Description |
|-------------|-----------|-----------|-------------|
| `EMPTY` | Dim white (20,20,20) | Slow pulse | Ready to record |
| `RECORDING` | Bright red (255,0,0) | Fast pulse | Recording active |
| `PLAYING` | Bright green (0,255,0) | Solid | Loop playing |
| `OVERDUBBING` | Yellow (255,255,0) | Fast pulse | Adding layers |
| `MUTED` | Gray (128,128,128) | Breathe | Track muted |

### Special Buttons

- **Record Button**: Pulsing red synchronized with BPM
- **Measure Flash**: White flash on beat 1
- **Button Press**: Brief flash for visual feedback

## 🔧 Integration Points

### With Your TrackManager

```cpp
class TrackManager {
    void setTrackState(uint8_t track, TrackState state) {
        // Your existing logic...
        
        // Update LEDs
        ledManager.setTrackState(track, state);
    }
};
```

### With Your ClockManager

```cpp
class ClockManager {
    void onBeat(uint8_t beatNumber) {
        // Your existing timing logic...
        
        // Update LED timing
        ledManager.onMeasureBeat(beatNumber);
        
        if (beatNumber == 0) {
            ledManager.onBarComplete();
        }
    }
};
```

### With Your ButtonManager

```cpp
class ButtonManager {
    void onButtonPress(uint8_t buttonIndex) {
        // Your existing button logic...
        
        // Visual feedback
        ledManager.onButtonPressed(buttonIndex);
        
        // Handle ControlPad-specific actions
        handleControlPadButton(buttonIndex);
    }
};
```

## 🎵 MIDI Looper Workflow Integration

### Recording Workflow

```cpp
void startRecording(uint8_t trackIndex) {
    // 1. Start your MIDI recording
    track[trackIndex].startRecording();
    
    // 2. Update state
    trackStates[trackIndex] = TrackState::RECORDING;
    
    // 3. Update LEDs
    ledManager.onTrackStartRecording(trackIndex);
    ledManager.setRecordButtonState(true, measureLengthMs);
}
```

### Button Press Handling

```cpp
void handleTrackButton(uint8_t trackIndex) {
    switch (trackStates[trackIndex]) {
        case TrackState::EMPTY:
            startRecording(trackIndex);
            break;
        case TrackState::PLAYING:
            startOverdubbing(trackIndex);
            break;
        case TrackState::OVERDUBBING:
            stopOverdubbing(trackIndex);
            break;
        // ... handle other states
    }
}
```

### BPM Synchronization

```cpp
void setBPM(float bpm) {
    currentBPM = bpm;
    
    // Update LED timing
    ledManager.setBPM(bpm);
    
    // Record button now pulses at correct rate
}
```

## 🎪 Advanced Features

### Performance Mode

```cpp
void activatePerformanceMode() {
    ledManager.applyPerformancePattern();
    
    // Add rainbow effects to active tracks
    for (int i = 0; i < 4; i++) {
        if (trackStates[i] == TrackState::PLAYING) {
            ledManager.setButtonAnimation(i, LEDAnimation::RAINBOW, 
                                        LEDColor::GREEN, LEDColor::BLUE);
        }
    }
}
```

### Multi-Clip Support (Future)

```cpp
void setClipState(uint8_t trackIndex, uint8_t clipIndex, TrackState state) {
    // Future: Support multiple clips per track
    uint8_t buttonIndex = getClipButton(trackIndex, clipIndex);
    ledManager.setClipState(trackIndex, clipIndex, state);
}
```

### Custom Color Schemes

```cpp
// In ControlPadLEDManager.h, customize colors:
const LEDColor LEDColor::TRACK_RECORDING(255, 100, 100);  // Softer red
const LEDColor LEDColor::TRACK_PLAYING(100, 255, 100);    // Softer green
```

## 🔄 State Machine Integration

### Adapt TrackState Enum

Make sure your `TrackState` enum matches:

```cpp
enum class TrackState {
    EMPTY = 0,
    RECORDING,
    STOPPED_RECORDING,  // Optional intermediate state
    PLAYING,
    OVERDUBBING,
    MUTED
};
```

### Complete State Updates

```cpp
void updateAllTrackLEDs() {
    TrackState states[4] = {
        trackManager.getTrackState(0),
        trackManager.getTrackState(1), 
        trackManager.getTrackState(2),
        trackManager.getTrackState(3)
    };
    
    ledManager.setAllTrackStates(states);
}
```

## ⚡ Performance Considerations

- **Update Rate**: LEDs update at 20 FPS (50ms intervals)
- **CPU Usage**: Minimal - animations calculated on-demand
- **Memory**: ~2KB for LED state and animation data
- **USB Traffic**: Uses your proven 5-command protocol

## 🐛 Troubleshooting

### LEDs Not Updating
```cpp
// Ensure you're calling update() in main loop
void loop() {
    ledManager.update();  // Essential!
    // ... rest of loop
}
```

### Wrong Button Mapping
```cpp
// Check button layout constants
Serial.printf("Track 1 button: %d\n", ControlPadLayout::TRACK_1);
Serial.printf("Record button: %d\n", ControlPadLayout::RECORD_BUTTON);
```

### Timing Issues
```cpp
// Verify BPM is set correctly
ledManager.setBPM(getCurrentBPM());
```

## 📊 Integration Checklist

- [ ] Add `ControlPadLEDManager.h` and `.cpp` to project
- [ ] Initialize LED manager in `setup()`
- [ ] Call `ledManager.update()` in main loop
- [ ] Connect track state changes to `ledManager.setTrackState()`
- [ ] Connect button presses to `ledManager.onButtonPressed()`
- [ ] Connect timing to `ledManager.onMeasureBeat()`
- [ ] Set BPM with `ledManager.setBPM()`
- [ ] Test with your existing MIDI looper workflow
- [ ] Customize colors and layout as needed
- [ ] Add startup animation for professional feel

## 🎯 Result

Your MIDI looper now has:
- ✅ **Professional LED feedback** for all track states
- ✅ **BPM-synchronized record button** pulsing  
- ✅ **Beautiful baseline lighting** when inactive
- ✅ **Visual confirmation** of all user actions
- ✅ **Future-ready** for clip-based expansion
- ✅ **Integrated timing** with your ClockManager
- ✅ **Modular design** that doesn't interfere with existing code

This transforms your 2-button MIDI looper into a visually stunning, professional-grade performance instrument! 🎛️✨ 