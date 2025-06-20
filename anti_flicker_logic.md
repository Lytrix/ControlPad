# CM Control Pad LED Flicker Prevention Logic

## Overview

The CM Control Pad uses a Teensy 4.0 and MAX3421E USB Host Controller to send LED commands in a precise, timed sequence. Flickering was caused by **timing drift** and **NAK (Not Acknowledged) USB retries** accumulating over time, disrupting the regular LED update cycle.

This solution implements **proactive, aggressive timing drift compensation** and **NAK smoothing** for both LED command packages, ensuring stable, flicker-free operation.

---

## System Timing Flow

```
+-------------------+      +-------------------+      +-------------------+
|  Send Package 1   | ---> |  Send Package 2   | ---> |   Activation      |
+-------------------+      +-------------------+      +-------------------+
        |                        |                          |
        |                        |                          |
        v                        v                          v
   [Timing Measured]        [Timing Measured]         [Timing Measured]
```

- **Package 1**: Controls LEDs 0-12
- **Package 2**: Controls LEDs 13-23
- **Activation**: Triggers the LED update

---

## Timing Drift Compensation Logic

### 1. **Continuous Monitoring**

- The system measures the time taken for each package transfer.
- It keeps a running sum and count for both Package 1 and Package 2.

```
For each command:
    pkg1TimingSum += pkg1Duration
    pkg1TimingCount++
    pkg2TimingSum += pkg2Duration
    pkg2TimingCount++
```

### 2. **Frequent Drift Checks**

- Every 25 commands, the system checks the average timing for both packages.

```
if (pkg1TimingCount >= 25) {
    avgPkg1 = pkg1TimingSum / pkg1TimingCount
    avgPkg2 = pkg2TimingSum / pkg2TimingCount
    // ... see if drift correction is needed
}
```

### 3. **Aggressive Early Intervention**

- If average timing exceeds a low threshold (e.g., 100μs for Pkg1, 110μs for Pkg2), drift correction is triggered.

```
if (avgPkg1 > 100 && !driftCorrectionActive) {
    driftCorrectionActive = true
    driftCorrectionSteps = 50
}
if (avgPkg2 > 110 && !pkg2DriftCorrectionActive) {
    pkg2DriftCorrectionActive = true
    pkg2DriftCorrectionSteps = 50
}
```

### 4. **Aggressive Correction**

- For the next 50 commands, a small delay is added to nudge the timing back to target.

```
if (driftCorrectionActive && driftCorrectionSteps > 0) {
    delayMicroseconds(3)
    driftCorrectionSteps--
}
if (pkg2DriftCorrectionActive && pkg2DriftCorrectionSteps > 0) {
    delayMicroseconds(4)
    pkg2DriftCorrectionSteps--
}
```

---

## NAK Smoothing Logic

### 1. **Detect NAK Events**

- If a USB transfer returns a NAK, a retry is performed and a compensation delay is set.

```
if (rcode == 0x4) { // NAK
    lastNAKTime = micros()
    nakCompensationDelay = 100
}
```

### 2. **Apply Compensation**

- On the next cycle, if a NAK was recently seen, a compensation delay is added to smooth out the timing gap.

```
if (lastNAKTime > 0 && (micros() - lastNAKTime) < 10000) {
    if (nakCompensationDelay > 0) {
        delayMicroseconds(nakCompensationDelay)
        nakCompensationDelay = 0
    }
}
```

---

## ASCII Timing Diagram

```
Normal Operation:
|--Pkg1--|--Pkg2--|--Activation--|--Pkg1--|--Pkg2--|--Activation--| ...

With Drift Correction:
|--Pkg1--|--Pkg2--|--Activation--|--Pkg1--|--Pkg2--|--Activation--|
      ^         ^         ^             ^         ^         ^
   [Drift check every 25 commands, correction if needed]

With NAK Smoothing:
|--Pkg1--|--Pkg2--|--Activation--|--Pkg1--|--Pkg2--|--Activation--|
      ^         ^         ^             ^         ^         ^
   [If NAK, add compensation delay here]
```

---

## Key Points

- **Early, aggressive drift correction** prevents timing from ever drifting far enough to cause flicker.
- **Both Package 1 and Package 2 are monitored and corrected independently.**
- **NAK smoothing** ensures that USB retry delays do not cause visible glitches.
- **All compensation is applied in small, distributed steps** to avoid sudden jumps in timing.

---

## Result

- **Stable, flicker-free LED animation for extended periods (6+ minutes and beyond)**
- **Robust to NAKs and timing drift**
- **No visible glitches or periodic flickering**

---

**This approach can be adapted to any system where precise, periodic USB or SPI communication is required and timing drift or retries can cause visible artifacts.** 