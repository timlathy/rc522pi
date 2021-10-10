#pragma once

#include "crc.h"

// Data sheets/references:
// MFRC522: https://www.nxp.com/docs/en/data-sheet/MFRC522.pdf
// NTAG21x: https://www.nxp.com/docs/en/data-sheet/NTAG213_215_216.pdf
// NFC Digital Protocol Technical Specification: https://its-wiki.no/images/3/3b/NFC_forum_digital_protocol.pdf

// MFRC522 data sheet, section 9
#define RC522_REG_CMD 0x01
#define RC522_REG_COM_IEN 0x02
#define RC522_REG_COM_IRQ 0x04
#define RC522_REG_ERROR 0x06
#define RC522_REG_FIFO_DATA 0x09
#define RC522_REG_FIFO_LEVEL 0x0A
#define RC522_REG_CTRL 0x0C
#define RC522_REG_BIT_FRAMING 0x0D
#define RC522_REG_MODE 0x11
#define RC522_REG_TX_CTRL 0x14
#define RC522_REG_TX_ASK 0x15
#define RC522_REG_RECV_GAIN 0x26
#define RC522_REG_TIMER_MODE 0x2A
#define RC522_REG_TIMER_PRESCALER_LO 0x2B
#define RC522_REG_TIMER_RELOAD_HI 0x2C
#define RC522_REG_TIMER_RELOAD_LO 0x2D
#define RC522_REG_VERSION 0x37

// MFRC522 data sheet, section 10
#define RC522_CMD_IDLE 0x0
#define RC522_CMD_TRANSCEIVE 0xC

// NTAG21x data sheet, section 9
#define NTAG_CMD_REQA 0x26
#define NTAG_CMD_CL1_SEL 0x93
#define NTAG_CMD_CL2_SEL 0x95
#define NTAG_CMD_SDD_REQ 0x20
#define NTAG_CMD_SEL_REQ 0x70
#define NTAG_CMD_READ 0x30
#define NTAG_CMD_WRITE 0xA2
#define NTAG_CMD_GET_VERSION 0x60
#define NTAG_CMD_PWD_AUTH 0x1B

#define NTAG_VERSION_STORAGE_SIZE_BYTE 6
#define NTAG_VERSION_STORAGE_SIZE_213 0x0F
#define NTAG_VERSION_STORAGE_SIZE_215 0x11
#define NTAG_VERSION_STORAGE_SIZE_216 0x13

// NTAG21x data sheet, section 9.3
#define NTAG_ACKNAK_RX_BITS 4
// Select first four bits
#define NTAG_ACKNAK_MASK 0xF
// If the response does not equal NTAG_ACK, it's a NAK
#define NTAG_ACK 0xA
#define NTAG_NAK_INVALID_ARG 0x0
#define NTAG_NAK_CRC_ERROR 0x1
#define NTAG_NAK_AUTH_CTR_OVERLOW 0x2
#define NTAG_NAK_WRITE_ERROR 0x3

// NTAG21x has a 7-bit NFCID
#define NTAG_NFCID_LEN 7

#define NFC_CASCADE_TAG 0x88

enum rc522c_status
{
  RC522C_STATUS_SUCCESS = 0,
  RC522C_STATUS_ERROR_PIGPIO = -1,
  RC522C_STATUS_ERROR_DEV_CMD_FAILED = -2,
  RC522C_STATUS_ERROR_DEV_NOT_RESPONDING = -3,
  RC522C_STATUS_ERROR_TAG_MISSING = -4,
  RC522C_STATUS_ERROR_TAG_UNSUPPORTED = -5,
  RC522C_STATUS_ERROR_TAG_NAK = -6
};

enum rc522c_tag_kind
{
  RC522C_TAG_KIND_UNKNOWN,
  RC522C_TAG_KIND_213,
  RC522C_TAG_KIND_215,
  RC522C_TAG_KIND_216
};

struct rc522c_state
{
    // pigpio handle for SPI device access
    int spi;
    // GPIO pin number for RST
    int rst_pin;
    int irq_pin;

    // Chip version
    // MFRC522 data sheet, section 9.3.4.8 lists two versions: 0x91 and 0x92.
    // There's also a Chinese chip with version 0x12
    char dev_version;

    // Is there an active (selected) tag?
    int tag_selected;

    // NTAG21x has a 7-bit NFCID. Valid when tag_selected is 1.
    // Only one tag should be selected and manipulated at a time, so
    // the NFCID of the current tag can be treated as global state.
    char tag_nfcid[NTAG_NFCID_LEN];

    // NTAG21x type. Valid when tag_selected is 1.
    enum rc522c_tag_kind tag_kind;

    // In case rc522c_status is _not_ RC522C_STATUS_SUCCESS:
    // Line in rc522.c where the error originated
    int error_line;
    // Context-specific error code (e.g. pigpio error code)
    int error_code;

    // Internal
    struct crc16_ccitt crc;
};

enum rc522c_status rc522c_ntag_select(struct rc522c_state* s);

// A single NFC read command returns 16 bytes (4 pages) of data
#define RC522_READ_LEN 16
enum rc522c_status rc522c_ntag_read(struct rc522c_state* s, char start_page, char* out);

#define RC522_WRITE_LEN 4
enum rc522c_status rc522c_ntag_write(struct rc522c_state* s, char page, const char* in);

#define RC522_PWD_LEN 4
#define RC522_PACK_LEN 2
enum rc522c_status rc522c_ntag_authenticate(struct rc522c_state* s, const char* pwd, char* out_pack);

enum rc522c_status rc522c_ntag_protect(struct rc522c_state* s, const char* pwd, const char* pack, int start_page, int rw);

// antenna_gain _must_ be in 0..7 range
enum rc522c_status rc522c_init(struct rc522c_state* s, int spi_baud_rate, int antenna_gain, int rst_pin);

void rc522c_deinit(struct rc522c_state* s);
