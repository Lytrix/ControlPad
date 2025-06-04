#include <Arduino.h>
#include "ControlPad.h"


ControlPad pad;

void setup() {
    Serial.begin(115200);
    pad.begin();
    pad.setLed(0, 255, 0, 0); // Set button 0 to red
    pad.setLed(1, 0, 255, 0); // Set button 1 to green
    pad.updateLeds();         // Send all LED states to hardware

}

void loop() {
    pad.poll(); // Poll for button/hall/LED events

    // Example: Set LED 0 to red if button 0 is pressed
    if (pad.getButtonState(0)) {
        pad.setLed(0, 255, 0, 0);
    } else {
        pad.setLed(0, 0, 0, 0);
    }
    pad.updateLeds();
}
