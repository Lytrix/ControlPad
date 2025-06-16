#ifndef CMCONTROLPAD_H
#define CMCONTROLPAD_H

#include <Usb.h>
#include <usbhub.h>

#define CM_VID  0x2516
#define CM_PID  0x012D

class CMControlPad : public USBDeviceConfig {
private:
    USB *pUsb;  // Must be first since it's initialized first
    uint8_t bAddress;
    bool initialized;
    uint8_t bConfNum;
    EpInfo epInfo[3];

    void sendControlData(uint8_t *data, uint16_t len);
    void sendCommitCommand();
    bool initializeProfiles();
    bool switchToCustomMode();

public:
    CMControlPad(USB *p) : pUsb(p), bAddress(0), initialized(false), bConfNum(0) {
        Serial.println("🎮 CMControlPad constructor called");
        for (uint8_t i = 0; i < 3; i++) {
            epInfo[i].epAddr = 0;
            epInfo[i].maxPktSize = 0;
            epInfo[i].epAttribs = 0;
            epInfo[i].bmNakPower = USB_NAK_MAX_POWER;
        }
    }

    // Required USBDeviceConfig methods
    uint8_t Init(uint8_t parent, uint8_t port, bool lowspeed) override;
    uint8_t ConfigureDevice(uint8_t parent, uint8_t port, bool lowspeed) override {
        Serial.println("🎮 CMControlPad::ConfigureDevice called");
        
        // Get configuration descriptor
        uint8_t buf[sizeof(USB_CONFIGURATION_DESCRIPTOR)];
        USB_CONFIGURATION_DESCRIPTOR* confDesc = reinterpret_cast<USB_CONFIGURATION_DESCRIPTOR*>(buf);
        
        uint8_t rcode = pUsb->getConfDescr(bAddress, 0, sizeof(USB_CONFIGURATION_DESCRIPTOR), 0, buf);
        if (rcode) {
            Serial.print("❌ Error getting configuration descriptor: 0x");
            Serial.println(rcode, HEX);
            return rcode;
        }
        
        // Print configuration info
        Serial.print("📋 Configuration descriptor: bLength=");
        Serial.print(confDesc->bLength);
        Serial.print(" bDescriptorType=");
        Serial.print(confDesc->bDescriptorType);
        Serial.print(" wTotalLength=");
        Serial.print(confDesc->wTotalLength);
        Serial.print(" bNumInterfaces=");
        Serial.print(confDesc->bNumInterfaces);
        Serial.print(" bConfigurationValue=");
        Serial.print(confDesc->bConfigurationValue);
        Serial.print(" bmAttributes=");
        Serial.print(confDesc->bmAttributes, HEX);
        Serial.print(" bMaxPower=");
        Serial.println(confDesc->bMaxPower);
        
        // Initialize the device
        Serial.println("🎮 Starting device initialization...");
        rcode = initializeDevice();
        if (rcode) {
            Serial.print("❌ Error initializing device: 0x");
            Serial.println(rcode, HEX);
            return rcode;
        }
        
        Serial.println("✅ Device configuration complete");
        return 0;
    }
    uint8_t Release() override;
    uint8_t Poll() override { return 0; }
    uint8_t GetAddress() override { return bAddress; }
    void ResetHubPort(uint8_t port) override {} // Not used for non-hub devices
    bool VIDPIDOK(uint16_t vid, uint16_t pid) override {
        Serial.print("🎮 CMControlPad::VIDPIDOK called with VID: 0x");
        Serial.print(vid, HEX);
        Serial.print(" PID: 0x");
        Serial.println(pid, HEX);
        return (vid == CM_VID && pid == CM_PID);
    }
    bool DEVCLASSOK(uint8_t klass) override {
        Serial.print("🎮 CMControlPad::DEVCLASSOK called with class: 0x");
        Serial.println(klass, HEX);
        return true; // Accept any device class
    }
    bool DEVSUBCLASSOK(uint8_t subklass) override {
        Serial.print("🎮 CMControlPad::DEVSUBCLASSOK called with subclass: 0x");
        Serial.println(subklass, HEX);
        return true; // Accept any subclass
    }
    uint8_t ConfigXtract(uint8_t conf, uint8_t iface, uint8_t alt, uint8_t proto, const USB_ENDPOINT_DESCRIPTOR* ep);

    uint8_t initializeDevice();
    
    // Public accessors for private members
    uint8_t getAddress() const { return bAddress; }
    const EpInfo* getEpInfo() const { return epInfo; }
    bool isInitialized() const { return initialized; }

    // Control pad specific functions
    bool setCustomMode();  // Set device to custom LED mode
};

#endif
