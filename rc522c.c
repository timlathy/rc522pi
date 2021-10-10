#include <assert.h>
#include <pigpio.h>
#include <stdio.h>
#include <string.h>

#include "rc522c.h"

// Must run as root
// Based on https://github.com/ondryaso/pi-rc522
// Total/free memory: vcgencmd get_mem reloc_total/reloc

// clang-format off
#define CHECK_PIGPIO(s, expr) {int ret = expr; if (ret < 0) { s->error_line = __LINE__; s->error_code = ret; return RC522C_STATUS_ERROR_PIGPIO; }}
#define CHECK_RC522C_STATUS(s, expr) {enum rc522c_status ret = expr; if (ret != RC522C_STATUS_SUCCESS) { return ret; }}
#define RETURN_RC522C_ERROR(s, status, _error_code) {s->error_line = __LINE__; s->error_code = _error_code; return status;}
// clang-format on

int spi_write_byte(struct rc522c_state* s, char addr, char val)
{
    // MFRC522 8.1.2, address byte consists of msb=0 to indicate reg write and lsb=0
    char tx[] = {addr << 1, val};
    int status = spiWrite(s->spi, tx, 2);
    return status;
}

int spi_read_byte(struct rc522c_state* s, char addr, char* val)
{
    // MFRC522 8.1.2, address byte msb=1 (read), lsb=0, next byte is 0 because we only intend to read one byte
    char tx[] = {(addr << 1) | 0x80, 0};
    char rx[] = {0, 0}; // first received byte is undefined, next byte is the value
    int status = spiXfer(s->spi, tx, rx, 2);
    if (status > 0)
        *val = rx[1];
    return status;
}

enum rc522c_status init_dev(struct rc522c_state* s, int antenna_gain)
{
    // Do a simple sanity check: version must be non-zero. If it is 0, the chip is not responding.
    // This happens e.g. when the post-hard reset delay is too short
    s->dev_version = 0;
    CHECK_PIGPIO(s, spi_read_byte(s, RC522_REG_VERSION, &s->dev_version));
    if (s->dev_version == 0)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_DEV_NOT_RESPONDING, 0);

    // Timer pscl = 3390, reload = 30, delay = (3390*2+1)*(30+1) / 13560000Hz = 0.015s = 15ms
    // 0x80 = timer automatically starts at the end of transmission, prescaler_hi = 0xD
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_TIMER_MODE, 0x8D));
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_TIMER_PRESCALER_LO, 0x3E));
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_TIMER_RELOAD_HI, 0));
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_TIMER_RELOAD_LO, 30));

    // ??? shouldn't work, perhaps there's an error in pirc522 and 0x20 is intended?
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_TX_ASK, 0x40));
    // CRC preset = A671, MFIN is active high?, TxWaitRF
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_MODE, 0x3D));

    // Set receiver gain (higher gain => more power along narrower direction)
    // Valid values are 0...7; see MFRC522 9.3.3.6 for more information
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_RECV_GAIN, (antenna_gain << 4)));

    // Enable antennas
    char tx_state;
    CHECK_PIGPIO(s, spi_read_byte(s, RC522_REG_TX_CTRL, &tx_state));
    if ((tx_state & 0x03) == 0)
        CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_TX_CTRL, tx_state | 0x03));

    return RC522C_STATUS_SUCCESS;
}

// rx _must_ be able to fit at least 64 bytes (size of FIFO buffer)
// on success, returns number of _bits_ read to rx
enum rc522c_status rc522c_transceive(struct rc522c_state* s, const char* tx, int tx_bits, char* rx, int* rx_bits)
{
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_COM_IRQ, 0x7F));        // clear interrupt request
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_COM_IEN, 0x80 | 0x77)); // enable all interrupts, invert irq pin signal
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_FIFO_LEVEL, 0x80));     // clear FIFO buffer
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_CMD, RC522_CMD_IDLE));  // don't execute any commands yet

    int tx_bytes = (tx_bits + 7) / 8; // ceil

    for (int i = 0; i < tx_bytes; ++i)
        CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_FIFO_DATA, tx[i]));

    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_CMD, RC522_CMD_TRANSCEIVE));
    // 0x80 starts the transition, lowest 3 bits = number of bits in the last byte
    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_BIT_FRAMING, 0x80 | (tx_bits % 8)));

    char irq;
    int i;
    for (i = 0; i < 2000; ++i)
    {
        CHECK_PIGPIO(s, spi_read_byte(s, RC522_REG_COM_IRQ, &irq));
        if (irq & 0x31) // 0x20 = received data, 0x10 = command terminated, 0x1 = timer counter reached 0
            break;
    }

    CHECK_PIGPIO(s, spi_write_byte(s, RC522_REG_BIT_FRAMING, 0)); // clear transmission bits

    char error;
    CHECK_PIGPIO(s, spi_read_byte(s, RC522_REG_ERROR, &error));
    error &= 0xDB; // ignore crc errors and reserved
    if (error)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_DEV_CMD_FAILED, error);

    // Check for timer interrupt and interpret it as timeout, i.e. the tag did not answer
    if (irq & 0x1)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_MISSING, error);

    char rx_bytes;
    CHECK_PIGPIO(s, spi_read_byte(s, RC522_REG_FIFO_LEVEL, &rx_bytes));

    // I think this shouldn't happen, but sometimes it does. Possibly some unrelated interrupt going off?
    if (rx_bytes == 0)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_MISSING, error);

    char ctrl;
    CHECK_PIGPIO(s, spi_read_byte(s, RC522_REG_CTRL, &ctrl));

    *rx_bits = rx_bytes * 8;
    char valid_bits_in_last_rx_byte = ctrl & 0x7;
    if (valid_bits_in_last_rx_byte != 0)
        *rx_bits -= 8 - valid_bits_in_last_rx_byte;

    if (rx_bytes == 0 && *rx_bits > 0)
        rx_bytes = 1;

    for (int i = 0; i < rx_bytes; ++i)
        CHECK_PIGPIO(s, spi_read_byte(s, RC522_REG_FIFO_DATA, &rx[i]));

    return RC522C_STATUS_SUCCESS;
}

enum rc522c_status rc522c_ntag_select(struct rc522c_state* s)
{
    char rx[64];
    int rx_bits;

    s->tag_selected = 0;

    char tx_reqa[] = {NTAG_CMD_REQA};
    CHECK_RC522C_STATUS(s, rc522c_transceive(s, tx_reqa, 7 /* REQA is a 7 bit command */, rx, &rx_bits));
    if (rx_bits != 16)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);

    // NTAG21x has a 7-bit NFCID and needs to go through two cascade levels (CL1, CL2) before we can work with it
    const char cl_selectors[2] = {NTAG_CMD_CL1_SEL, NTAG_CMD_CL2_SEL};
    for (int cl = 0; cl < 2; cl++)
    {
        // Per NFC Digital Protocol:
        // Section 4.5: EoD _is not_ present for SDD_REQ. We only need to send two bytes, as described in section 4.7
        // (SDD_REQ)
        char tx_sdd[] = {cl_selectors[cl], NTAG_CMD_SDD_REQ};
        CHECK_RC522C_STATUS(s, rc522c_transceive(s, tx_sdd, sizeof(tx_sdd) * 8, rx, &rx_bits));
        // We expect 5 bytes in response:
        // CL1: cascade tag (0x88), NFCID_0, NFCID_1, NFCID_2, BCC (xor of first four bytes)
        // CL2: NFCID_3, NFCID_4, NFCID_5, NFCID_6, BCC
        if (rx_bits != 5 * 8)
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
        char bcc_check = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];
        if (bcc_check != rx[4])
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);

        if (cl == 0)
        {
            // If we haven't received the cascade tag in CL1 SDD_RES, it means the tag is not an NTAG21x --
            // probably a MIFARE Classic (4-bit NFCID)
            if (rx[0] != NFC_CASCADE_TAG)
                RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
            s->tag_nfcid[0] = rx[1];
            s->tag_nfcid[1] = rx[2];
            s->tag_nfcid[2] = rx[3];
        }
        else
        {
            s->tag_nfcid[3] = rx[0];
            s->tag_nfcid[4] = rx[1];
            s->tag_nfcid[5] = rx[2];
            s->tag_nfcid[6] = rx[3];
        }

        // Per NFC Digital Protocol:
        // The payload is the NFCID part we've received in SDD_RES.
        // Since BCC is calculated the same as in SDD_RES, we can resend it too.
        char tx_sel[9] = {cl_selectors[cl], NTAG_CMD_SEL_REQ, rx[0], rx[1], rx[2], rx[3], rx[4], 0};
        // Section 4.5: EoD _is_ present for SEL_REQ
        // Section 4.4: EoD is appended to payload and consists of a two-byte checksum (CRC_A) computed from the payload
        compute_crc(&s->crc, tx_sel, 7, &tx_sel[7]);

        CHECK_RC522C_STATUS(s, rc522c_transceive(s, tx_sel, sizeof(tx_sel) * 8, rx, &rx_bits));
        // We expect 3 bytes in response: SEL_RES and CRC_A[1,2]
        if (rx_bits != 24)
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
        char sel_crc[2] = {0};
        compute_crc(&s->crc, rx, 1, sel_crc);
        if (sel_crc[0] != rx[1] || sel_crc[1] != rx[2])
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);

        if (cl == 0)
        {
            // This shouldn't really happen... Bit 3 (cascade bit) is set to 1 if we need to proceed to CL2, which we do
            if ((rx[0] & 0x04) == 0)
                RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
        }
        else
        {
            // This can happen with tags that have 3 cascade levels (not supported).
            // NTAG21x is expected to have cascade bit = 0 in SEL_RES for CL2.
            if ((rx[0] & 0x04) != 0)
                RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
        }
    }

    // Find the tag type by issuing the GET_VERSION command (NTAG21x section 10.1)
    {
        char tx_get_version[3] = {NTAG_CMD_GET_VERSION, 0};
        compute_crc(&s->crc, tx_get_version, 1, &tx_get_version[1]);
        CHECK_RC522C_STATUS(s, rc522c_transceive(s, tx_get_version, sizeof(tx_get_version) * 8, rx, &rx_bits));
        // First, check for a NAK response (4 bits)
        char acknak = rx[0] & NTAG_ACKNAK_MASK;
        if (rx_bits == NTAG_ACKNAK_RX_BITS && acknak != NTAG_ACK)
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_NAK, acknak);
        // If the response is not a NAK, we expect 10 bytes (8 bytes of product info + CRC)
        if (rx_bits != 10 * 8)
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
        char crc[2];
        compute_crc(&s->crc, rx, 8, crc);
        if (crc[0] != rx[8] || crc[1] != rx[9])
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);

        switch (rx[NTAG_VERSION_STORAGE_SIZE_BYTE])
        {
        case NTAG_VERSION_STORAGE_SIZE_213:
            s->tag_kind = RC522C_TAG_KIND_213;
            break;
        case NTAG_VERSION_STORAGE_SIZE_215:
            s->tag_kind = RC522C_TAG_KIND_215;
            break;
        case NTAG_VERSION_STORAGE_SIZE_216:
            s->tag_kind = RC522C_TAG_KIND_216;
            break;
        default:
            RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
        }
    }

    s->tag_selected = 1;

    return RC522C_STATUS_SUCCESS;
}

enum rc522c_status rc522c_ntag_read(struct rc522c_state* s, char start_page, char* out)
{
    char rx[64];
    int rx_bits;

    if (!s->tag_selected)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_MISSING, 0);

    char tx_read[4] = {NTAG_CMD_READ, start_page, 0};
    compute_crc(&s->crc, tx_read, 2, &tx_read[2]);
    CHECK_RC522C_STATUS(s, rc522c_transceive(s, tx_read, sizeof(tx_read) * 8, rx, &rx_bits));
    // NTAG21x section 10.2:
    // First, check for a NAK response (4 bits)
    char acknak = rx[0] & NTAG_ACKNAK_MASK;
    if (rx_bits == NTAG_ACKNAK_RX_BITS && acknak != NTAG_ACK)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_NAK, acknak);
    // If the response is not a NAK, we expect 18 bytes (contents of 4 pages + CRC)
    if (rx_bits != (RC522_READ_LEN + 2) * 8)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
    char crc[2];
    compute_crc(&s->crc, rx, RC522_READ_LEN, crc);
    if (crc[0] != rx[16] || crc[1] != rx[17])
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);

    memcpy(out, rx, RC522_READ_LEN);
    return RC522C_STATUS_SUCCESS;
}

enum rc522c_status rc522c_ntag_write(struct rc522c_state* s, char page, const char* in)
{
    char rx[64];
    int rx_bits;

    if (!s->tag_selected)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_MISSING, 0);

    char tx_write[8] = {NTAG_CMD_WRITE, page, in[0], in[1], in[2], in[3], 0};
    compute_crc(&s->crc, tx_write, 6, &tx_write[6]);
    CHECK_RC522C_STATUS(s, rc522c_transceive(s, tx_write, sizeof(tx_write) * 8, rx, &rx_bits));
    // NTAG21x section 10.4: we expect 4 bits (ACK/NAK) in response. ACK is 0xA
    if (rx_bits != NTAG_ACKNAK_RX_BITS)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
    char acknak = rx[0] & NTAG_ACKNAK_MASK;
    if (acknak != NTAG_ACK)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_NAK, acknak);

    return RC522C_STATUS_SUCCESS;
}

enum rc522c_status rc522c_ntag_authenticate(struct rc522c_state* s, const char* pwd, char* out_pack)
{
    char rx[64];
    int rx_bits;

    if (!s->tag_selected)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_MISSING, 0);

    char tx_auth[7] = {NTAG_CMD_PWD_AUTH, pwd[0], pwd[1], pwd[2], pwd[3], 0};
    compute_crc(&s->crc, tx_auth, 5, &tx_auth[5]);
    CHECK_RC522C_STATUS(s, rc522c_transceive(s, tx_auth, sizeof(tx_auth) * 8, rx, &rx_bits));
    // NTAG21x section 10.7:
    // First, check for a NAK response (4 bits)
    char acknak = rx[0] & NTAG_ACKNAK_MASK;
    if (rx_bits == NTAG_ACKNAK_RX_BITS && acknak != NTAG_ACK)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_NAK, acknak);
    // If the response is not a NAK, we expect 4 bytes (2-byte PACK + CRC)
    if (rx_bits != 4 * 8)
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
    char crc[2];
    compute_crc(&s->crc, rx, 2, crc);
    if (crc[0] != rx[2] || crc[1] != rx[3])
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);

    out_pack[0] = rx[0];
    out_pack[1] = rx[1];

    return RC522C_STATUS_SUCCESS;
}

enum rc522c_status rc522c_ntag_protect(
    struct rc522c_state* s, const char* pwd, const char* pack, int start_page, int rw)
{
    int config_start_page;
    switch (s->tag_kind)
    {
    case RC522C_TAG_KIND_213:
        config_start_page = 0x29;
        break;
    case RC522C_TAG_KIND_215:
        config_start_page = 0x83;
        break;
    case RC522C_TAG_KIND_216:
        config_start_page = 0xE3;
        break;
    default:
        RETURN_RC522C_ERROR(s, RC522C_STATUS_ERROR_TAG_UNSUPPORTED, 0);
    }

    // Rewrite PWD
    CHECK_RC522C_STATUS(s, rc522c_ntag_write(s, config_start_page + 2, pwd));

    // To rewrite AUTH0, ACCESS, and PACK, we must first read the config so unrelated settings are left unchanged
    char config_data[16];
    CHECK_RC522C_STATUS(s, rc522c_ntag_read(s, config_start_page, config_data));

    // Rewrite AUTH0 (first protected page)
    config_data[3] = start_page;
    CHECK_RC522C_STATUS(s, rc522c_ntag_write(s, config_start_page, &config_data[0]));

    // Rewrite PACK
    config_data[12] = pack[0];
    config_data[13] = pack[1];
    CHECK_RC522C_STATUS(s, rc522c_ntag_write(s, config_start_page + 3, &config_data[12]));

    // Rewrite PROT (bit 7 of ACCESS) (0 = write access is protected by password, 1 = read and write access is
    // protected)
    if (rw)
        config_data[4] |= 0x80;
    else
        config_data[4] &= 0x7F;
    CHECK_RC522C_STATUS(s, rc522c_ntag_write(s, config_start_page + 1, &config_data[4]));

    return RC522C_STATUS_SUCCESS;
}

enum rc522c_status rc522c_init(struct rc522c_state* s, int spi_baud_rate, int antenna_gain, int rst_pin)
{
    memset(s, 0, sizeof(struct rc522c_state));
    init_crc16_ccitt(&s->crc);

    CHECK_PIGPIO(s, gpioInitialise());
    CHECK_PIGPIO(s, (s->spi = spiOpen(0, spi_baud_rate, 0)));

    s->rst_pin = rst_pin;
    CHECK_PIGPIO(s, gpioSetMode(rst_pin, PI_OUTPUT));

    // Chinese knock-offs (vresion register 0x37 returning 0x12) do not implement soft reset.
    // Before interfacing with the chip, perform a hard reset, just in case.

    // Set RST to LOW for at least 100ns (MFRC522 8.8.1); we'll wait for 10us
    CHECK_PIGPIO(s, gpioWrite(rst_pin, PI_LOW));
    gpioDelay(10);

    // Set RST to HIGH and wait for the chip to start.
    // Testing shows that the chip doesn't reply until at least 200us have passed; we'll wait for 400us to be sure.
    CHECK_PIGPIO(s, gpioWrite(rst_pin, PI_HIGH));
    gpioDelay(400);

    return init_dev(s, antenna_gain);
}

void rc522c_deinit(struct rc522c_state* s)
{
    spiClose(s->spi);
    gpioTerminate();
}
