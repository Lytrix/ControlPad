#ifndef USB_SYNCHRONIZED_PACKET_CONTROLLER_H
#define USB_SYNCHRONIZED_PACKET_CONTROLLER_H

#include <Arduino.h>
#include <USBHost_t36.h>

// ===== SIMPLE BUTTON/LED COORDINATION SYSTEM =====
class USBSynchronizedPacketController {
private:
    // *** Simple Coordination Variables ***
    bool buttonActivityDetected = false;        // Track when buttons are being processed
    uint32_t lastButtonActivityTime = 0;    // When button activity was last detected
    static constexpr uint32_t BUTTON_QUIET_PERIOD_MS = 30;   // Wait only 30ms after button activity
    
    // *** Basic Statistics ***
    uint32_t totalPacketsSent = 0;
    uint32_t packetsInCurrentBurst = 0;
    static constexpr uint32_t MAX_PACKETS_PER_BURST = 5;
    uint32_t lastStatusLogTime = 0;
    
    // *** Unused Variables (to keep API compatible) ***
    uint32_t currentPacket = 0;
    
    #ifdef __MK66FX1M0__
    uint32_t getFreeRAM();  // Memory monitoring for Teensy
    #endif
    
public:
    // *** Singleton Pattern ***
    static USBSynchronizedPacketController staticInstance;
    
    // *** Main Interface ***
    void initialize();
    void monitorUSBActivity();
    bool isSafeToSendPacket();
    void recordPacketSent();
    
    // *** NEW: Button/LED Coordination ***
    void notifyButtonActivity();        // Call when button events are processed
    bool isButtonQuietPeriod();         // Check if we're in button quiet period
    
    // *** NEW: Memory cleanup tracking ***
    void trackMemoryCleanup();
    void onFollowupErrorCalled();
    
    // *** NEW: USB cleanup protection for LED coordination ***
    bool isUSBCleanupActive();
    void activateUSBCleanupProtection(const char* reason, uint32_t duration = 100);
    
    // *** NEW: LED Controller Hardware Recovery System ***
    void validateAndRecoverLEDController();
    bool testLEDControllerResponse();
    bool recoverLEDController();

    // *** NEW: followup_Transfer Pattern Integration ***
    void onTransferStarted(uint32_t transferId, const void* buffer, size_t length);
    void onTransferCompleted(uint32_t transferId, bool success, uint32_t actualLength);
    void onTransferError(uint32_t transferId, uint32_t errorCode, const char* errorDescription);
    void performFollowupTransferCleanup(uint32_t transferId);
    void performFollowupErrorRecovery(uint32_t transferId, uint32_t errorCode);
    
    // *** NEW: Transfer state tracking for proper cleanup ***
    struct TransferState {
        uint32_t transferId;
        const void* buffer;
        size_t length;
        uint32_t startTime;
        bool isActive;
        uint32_t retryCount;
    };
    
    static constexpr uint8_t MAX_TRACKED_TRANSFERS = 8;
    TransferState activeTransfers[MAX_TRACKED_TRANSFERS];  // Made public for access
    uint32_t nextTransferId = 1;
    
    // *** NEW: Memory cleanup state tracking ***
    bool isFollowupCleanupActive();
    void activateFollowupCleanupProtection(uint32_t transferId, const char* reason, uint32_t duration = 200);
    
    // *** NEW: Transfer timeout monitoring ***
    void monitorTransferTimeouts();
    static constexpr uint32_t TRANSFER_TIMEOUT_MS = 500; // 500ms timeout for transfers

private:
};

// Global instance
extern USBSynchronizedPacketController usbSyncController;

#endif 