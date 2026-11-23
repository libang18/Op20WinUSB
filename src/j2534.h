// SAE J2534-1 (04.04) definitions + the Tactrix Openport 2.0 extensions that
// applications written against op20pt32.dll expect.
#pragma once

#include <windows.h>

// ---- Protocol IDs -------------------------------------------------------
#define J1850VPW        1
#define J1850PWM        2
#define ISO9141         3
#define ISO14230        4
#define CAN             5
#define ISO15765        6

// J2534-2 channel variants supported by the Openport 2.0
#define CAN_CH1         0x00009000
#define J1850VPW_CH1    0x00009080
#define J1850PWM_CH1    0x00009160
#define ISO9141_CH1     0x00009240  // K line
#define ISO9141_CH2     0x00009241  // L line
#define ISO9141_CH3     0x00009242  // receive-only RS-232 on the 2.5 mm jack
#define ISO14230_CH1    0x00009320  // K line
#define ISO14230_CH2    0x00009321  // L line
#define ISO15765_CH1    0x00009400

// ---- IOCTL IDs ----------------------------------------------------------
#define GET_CONFIG                          0x01
#define SET_CONFIG                          0x02
#define READ_VBATT                          0x03
#define FIVE_BAUD_INIT                      0x04
#define FAST_INIT                           0x05
#define CLEAR_TX_BUFFER                     0x07
#define CLEAR_RX_BUFFER                     0x08
#define CLEAR_PERIODIC_MSGS                 0x09
#define CLEAR_MSG_FILTERS                   0x0A
#define CLEAR_FUNCT_MSG_LOOKUP_TABLE        0x0B
#define ADD_TO_FUNCT_MSG_LOOKUP_TABLE       0x0C
#define DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE  0x0D
#define READ_PROG_VOLTAGE                   0x0E

#define TX_IOCTL_BASE                       0x70000
#define TX_IOCTL_APP_SERVICE                (TX_IOCTL_BASE + 0)
#define TX_IOCTL_SET_DLL_DEBUG_FLAGS        (TX_IOCTL_BASE + 1)
#define TX_IOCTL_SET_DEV_DEBUG_FLAGS        (TX_IOCTL_BASE + 2)
#define TX_IOCTL_SET_DLL_STATUS_CALLBACK    (TX_IOCTL_BASE + 3)
#define TX_IOCTL_GET_DEVICE_INSTANCES       (TX_IOCTL_BASE + 4)

// ---- Pins ---------------------------------------------------------------
#define AUX_PIN         0
#define J1962_PIN_16    16
#define PIN_VADJ        17

#define SHORT_TO_GROUND 0xFFFFFFFE
#define VOLTAGE_OFF     0xFFFFFFFF

// ---- Error IDs ----------------------------------------------------------
#define STATUS_NOERROR              0x00
#define ERR_NOT_SUPPORTED           0x01
#define ERR_INVALID_CHANNEL_ID      0x02
#define ERR_INVALID_PROTOCOL_ID     0x03
#define ERR_NULL_PARAMETER          0x04
#define ERR_INVALID_IOCTL_VALUE     0x05
#define ERR_INVALID_FLAGS           0x06
#define ERR_FAILED                  0x07
#define ERR_DEVICE_NOT_CONNECTED    0x08
#define ERR_TIMEOUT                 0x09
#define ERR_INVALID_MSG             0x0A
#define ERR_INVALID_TIME_INTERVAL   0x0B
#define ERR_EXCEEDED_LIMIT          0x0C
#define ERR_INVALID_MSG_ID          0x0D
#define ERR_DEVICE_IN_USE           0x0E
#define ERR_INVALID_IOCTL_ID        0x0F
#define ERR_BUFFER_EMPTY            0x10
#define ERR_BUFFER_FULL             0x11
#define ERR_BUFFER_OVERFLOW         0x12
#define ERR_PIN_INVALID             0x13
#define ERR_CHANNEL_IN_USE          0x14
#define ERR_MSG_PROTOCOL_ID         0x15
#define ERR_INVALID_FILTER_ID       0x16
#define ERR_NO_FLOW_CONTROL         0x17
#define ERR_NOT_UNIQUE              0x18
#define ERR_INVALID_BAUDRATE        0x19
#define ERR_INVALID_DEVICE_ID       0x1A

// ---- RxStatus flags -----------------------------------------------------
#define TX_MSG_TYPE                 0x00000001
#define START_OF_MESSAGE            0x00000002
#define RX_BREAK                    0x00000004
#define TX_DONE                     0x00000008
#define ISO15765_PADDING_ERROR      0x00000010
#define ISO15765_ADDR_TYPE          0x00000080
#define CAN_29BIT_ID                0x00000100

// ---- Filter types -------------------------------------------------------
#define PASS_FILTER                 0x00000001
#define BLOCK_FILTER                0x00000002
#define FLOW_CONTROL_FILTER         0x00000003

// ---- Structures ---------------------------------------------------------
#define PASSTHRU_MSG_DATA_SIZE 4128

typedef struct {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    unsigned char Data[PASSTHRU_MSG_DATA_SIZE];
} PASSTHRU_MSG;

typedef struct {
    unsigned long Parameter;
    unsigned long Value;
} SCONFIG;

typedef struct {
    unsigned long NumOfParams;
    SCONFIG *ConfigPtr;
} SCONFIG_LIST;

typedef struct {
    unsigned long NumOfBytes;
    unsigned char *BytePtr;
} SBYTE_ARRAY;
