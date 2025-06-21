#ifndef _CM_CONTROL_PAD_H_
#define _CM_CONTROL_PAD_H_

#include "Usb.h"

#define CM_CONTROL_PAD_VERSION 10000
#define CM_VID  0x2516
#define CM_PID  0x012D
#define CM_MAX_ENDPOINTS 4  // Control, IN, OUT
#define CM_PACKET_SIZE 64

// Control commands
#define CM_CMD_SETUP_1          0x42, 0x00
#define CM_CMD_SETUP_2          0x42, 0x10  
#define CM_CMD_SETUP_3          0x43, 0x00
#define CM_CMD_STATUS           0x41, 0x80
#define CM_CMD_ACTIVATE_EFFECTS 0x52, 0x00
#define CM_CMD_CUSTOM_MODE      0x56, 0x81

/** This class implements support for CM Control Pad USB gamepad device. */
class CMControlPad : public USBDeviceConfig {
protected:
    static const uint8_t epDataInIndex = 1;   // DataIn endpoint index
    static const uint8_t epDataOutIndex = 2;  // DataOut endpoint index

    /* Mandatory members */
    USB      *pUsb;
    uint8_t  bAddress;
    bool     bPollEnable;
    uint16_t pid, vid;    // ProductID, VendorID
    
    /* Endpoint data structure */
    EpInfo  epInfo[CM_MAX_ENDPOINTS];
    
    /* Control pad state */
    bool initialized;
    bool customModeActive;
    
    /* Input data buffer */
    uint8_t recvBuf[CM_PACKET_SIZE];
    uint8_t readPtr;
    
    /* Internal device setup methods */
    uint8_t initializeDevice();
    uint8_t activateInterface0();
    uint8_t activateInterface1();
    uint8_t sendInitializationSequence();
    uint8_t sendCommandWithProperAck(uint8_t* cmd);
    uint8_t sendSimpleLEDTest();
    uint8_t activateCustomMode();
    void setupDeviceSpecific();
    void discoverEndpoints();
    uint8_t sendCommand(uint8_t* data, uint16_t len);
    
    /* USB polling and data management */
    uint8_t pollDevice();
    void processInputData(uint8_t* data, uint16_t len);

public:
    CMControlPad(USB *p);
    
    // Status checks
    operator bool() { return bPollEnable; }
    uint16_t idVendor() { return vid; }
    uint16_t idProduct() { return pid; }
    bool isReady() { return (bPollEnable && initialized); }
    bool isCustomModeActive() { return customModeActive; }
    
    // Control methods
    bool setCustomMode();
    bool setLEDColor(uint8_t button, uint8_t r, uint8_t g, uint8_t b);
    bool sendLEDCommand(uint8_t* data, uint16_t len);
    
    // Manual initialization methods
    uint8_t manualInit(uint8_t address);
    void setAddress(uint8_t address) { bAddress = address; }
    uint8_t getAddress() { return bAddress; }
    
    // Data methods  
    uint8_t RecvData(uint16_t *bytes_rcvd, uint8_t *dataptr);
    uint8_t RecvData(uint8_t *outBuf);
    uint8_t SendData(uint8_t *dataptr, uint16_t len);
    
    // Raw data access
    inline uint8_t SendRawData(uint16_t bytes_send, uint8_t *dataptr) { 
        return pUsb->outTransfer(bAddress, epInfo[epDataOutIndex].epAddr, bytes_send, dataptr); 
    }
    
    // USBDeviceConfig implementation
    virtual uint8_t Init(uint8_t parent, uint8_t port, bool lowspeed);
    virtual uint8_t Release();
    virtual uint8_t Poll();
    virtual uint8_t GetAddress() { return bAddress; }
    virtual bool VIDPIDOK(uint16_t vid, uint16_t pid) { 
        return (vid == CM_VID && pid == CM_PID); 
    }
    virtual bool DEVCLASSOK(uint8_t klass) { return true; } // Accept any class
    virtual bool DEVSUBCLASSOK(uint8_t subklass) { return true; }
    
    // Configuration extraction
    uint8_t ConfigXtract(uint8_t conf, uint8_t iface, uint8_t alt, uint8_t proto, const USB_ENDPOINT_DESCRIPTOR* ep);
    
    // Callback attachments
    void attachOnInit(void (*funcOnInit)(void)) {
        pFuncOnInit = funcOnInit;
    }
    
    void attachOnRelease(void (*funcOnRelease)(void)) {
        pFuncOnRelease = funcOnRelease;
    }
    
    void attachOnData(void (*funcOnData)(uint8_t* data, uint16_t len)) {
        pFuncOnData = funcOnData;
    }

private:
    void (*pFuncOnInit)(void) = nullptr;     // Called on successful init
    void (*pFuncOnRelease)(void) = nullptr;  // Called on release  
    void (*pFuncOnData)(uint8_t* data, uint16_t len) = nullptr; // Called on input data
};

#endif //_CM_CONTROL_PAD_H_
