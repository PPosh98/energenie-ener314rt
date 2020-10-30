/* radio.c  12/04/2016  D.J.Whale
 *          13/04/2019  Achronite - Additional functions and fixes
 *          01/02/2020  Achronite - Made init return -ve on failure
 *          27/10/2020  Achronite - Made some trace statements FULLTRACE
 *
 * An interface to the Energenie Raspberry Pi Radio board ENER314-RT-VER01
 *
 * https://energenie4u.co.uk/index.phpcatalogue/product/ENER314-RT
 */

/* TODO (rx)
DONE: decide interface for: HRF_readfifo_burst(uint8_t* buf, uint8_t len)
DONE: Knit in the payload check
DONE: Knit in the get_payload

TODO: test with monitor.py (receive only mode, FSK)
*/

/* TODO (OOK rx)
TODO: Write a tester legacy_rx.py to exercise 16 byte OOK receive
TODO: See if we can receive from RF hand transmiter
TODO: See how much 'noise bytes' we get
TODO: Might need to set the sync bytes on OOK receive to prevent false trigger
  This will mean loading a different configuration for OOK receive to transmit
  But we don't want to be manually looking for sync bits, and we don't want
  to use the preamble generator on tx as it doesn't work too well.
*/

/*
TODO: Harden up the payload receiver
  1. If there is a corrupted packet in the fifo, the first byte
  might not be the length byte, and the read might fail or leave
  bytes in the buffer. So, the receiver needs to be hardended up
  to cope with these cases. HRF_readfifo_burst returns an error
  if the length byte is longer than buflen, but need to check this
  error code.

  2. if we are in fixed length, or count byte, and more data
  is in the fifo than we expect, this will be read next time.
  Perhaps we need to clear the fifo at the end of each read operation,
  and also report if there is junk in the buffer (i.e. would always
  expect a read to leave an empty fifo).

  3. We should check the fifo underrun bit at the end of a read,
  this would mean that some of the bytes we read are not real bytes
  and should not be interpreted, that packet should be junked.
*/

/* TODO: DUTY CYCLE PROTECTION REQUIREMENT
 *
 * See page 3 of this app note: http://www.ti.com/lit/an/swra090/swra090.pdf
 *
 * At OOK 4800bps, 1 bit is 20uS, 1 byte is 1.6ms, 16 bytes is 26.6ms
 * 15 times (old design limit) is 400ms
 * 255 times (new design limit) is 6.8s
 *
 * Transmitter duty cycle
 * The transmitter duty cycle is defined as the ratio of the maximum ”on” time,
 * relative to a onehour period. If message acknowledgement is required, the
 * additional ”on” time shall be included. Advisory limits are:
 *
 * Duty cycle  Maximum “on” time [sec]   Minimum “off” time [sec]
 * 0.1 %       0.72                      0.72
 * 1 %         3.6                       1.8
 * 10 %        36                        3.6
 */

/***** INCLUDES *****/

#include "system.h"
#include "radio.h"
#include "delay.h"
#include "gpio.h"
#include "spi.h"
#include "hrfm69.h"
#include "trace.h"

/***** CONFIGURATION *****/

#define EXPECTED_RADIOVER 36
#define MAX_FIFO_BUFFER 66

// Energenie specific radio config values
#define RADIO_VAL_SYNCVALUE1FSK 0x2D // 1st byte of Sync word
#define RADIO_VAL_SYNCVALUE2FSK 0xD4 // 2nd byte of Sync word
#define RADIO_VAL_SYNCVALUE1OOK 0x80 // 1nd byte of Sync word
//#define RADIO_VAL_PACKETCONFIG1FSK       0xA2	// Variable length, Manchester coding, Addr must match NodeAddress
#define RADIO_VAL_PACKETCONFIG1FSKNO 0xA0 // Variable length, Manchester coding

//TODO: Not sure, might pass this in? What about on Arduino?
//What about if we have multiple chip selects on same SPI?
//What about if we have multiple spi's on different pins?

/* GPIO assignments for Raspberry Pi using BCM numbering */
#define RESET 25
// GREEN used for RX, RED used for TX
#define LED_GREEN 27 // (not B rev1)
#define LED_RED 22

#define CS 7 // CE1
#define SCLK 11
#define MOSI 10
#define MISO 9
#define TSETTLE (1) /* us settle */
#define THOLD (1)   /* us hold */
#define TFREQ (1)   /* us half clock */

SPI_CONFIG radioConfig = {CS, SCLK, MOSI, MISO, SPI_SPOL0, SPI_CPOL0, SPI_CPHA0, TSETTLE, THOLD, TFREQ};

/***** LOCAL FUNCTION PROTOTYPES *****/

static void _change_mode(uint8_t mode);
static void _wait_ready(void);
static void _wait_txready(void);
static void _config(HRF_CONFIG_REC *config, uint8_t len);
//static int _payload_waiting(void);

//----- ENERGENIE SPECIFIC CONFIGURATIONS --------------------------------------

static HRF_CONFIG_REC config_FSK[] = {
    {HRF_ADDR_REGDATAMODUL, HRF_VAL_REGDATAMODUL_FSK},      // modulation scheme FSK
    {HRF_ADDR_FDEVMSB, HRF_VAL_FDEVMSB30},                  // frequency deviation 5kHz 0x0052 -> 30kHz 0x01EC
    {HRF_ADDR_FDEVLSB, HRF_VAL_FDEVLSB30},                  // frequency deviation 5kHz 0x0052 -> 30kHz 0x01EC
    {HRF_ADDR_FRMSB, HRF_VAL_FRMSB434},                     // carrier freq -> 434.3MHz 0x6C9333
    {HRF_ADDR_FRMID, HRF_VAL_FRMID434},                     // carrier freq -> 434.3MHz 0x6C9333
    {HRF_ADDR_FRLSB, HRF_VAL_FRLSB434},                     // carrier freq -> 434.3MHz 0x6C9333
    {HRF_ADDR_AFCCTRL, HRF_VAL_AFCCTRLS},                   // standard AFC routine
    {HRF_ADDR_LNA, HRF_VAL_LNA50},                          // 200ohms, gain by AGC loop -> 50ohms
    {HRF_ADDR_RXBW, HRF_VAL_RXBW60},                        // channel filter bandwidth 10kHz -> 60kHz  page:26
    {HRF_ADDR_BITRATEMSB, 0x1A},                            // 4800b/s
    {HRF_ADDR_BITRATELSB, 0x0B},                            // 4800b/s
    {HRF_ADDR_SYNCCONFIG, HRF_VAL_SYNCCONFIG2},             // Size of the Synch word = 2 (SyncSize + 1)
    {HRF_ADDR_SYNCVALUE1, RADIO_VAL_SYNCVALUE1FSK},         // 1st byte of Sync word
    {HRF_ADDR_SYNCVALUE2, RADIO_VAL_SYNCVALUE2FSK},         // 2nd byte of Sync word
    {HRF_ADDR_PACKETCONFIG1, RADIO_VAL_PACKETCONFIG1FSKNO}, // Variable length, Manchester coding
    {HRF_ADDR_PAYLOADLEN, HRF_VAL_PAYLOADLEN66},            // max Length in RX, not used in Tx
    {HRF_ADDR_NODEADDRESS, 0x06},                           // Node address used in address filtering (not used)
};
#define CONFIG_FSK_COUNT (sizeof(config_FSK) / sizeof(HRF_CONFIG_REC))

static HRF_CONFIG_REC config_OOK[] = {
    {HRF_ADDR_REGDATAMODUL, HRF_VAL_REGDATAMODUL_OOK}, // modulation scheme OOK
    {HRF_ADDR_FDEVMSB, 0},                             // frequency deviation:0kHz
    {HRF_ADDR_FDEVLSB, 0},                             // frequency deviation:0kHz
    {HRF_ADDR_FRMSB, HRF_VAL_FRMSB433},                // carrier freq:433.92MHz 0x6C7AE1
    {HRF_ADDR_FRMID, HRF_VAL_FRMID433},                // carrier freq:433.92MHz 0x6C7AE1
    {HRF_ADDR_FRLSB, HRF_VAL_FRLSB433},                // carrier freq:433.92MHz 0x6C7AE1
    {HRF_ADDR_RXBW, HRF_VAL_RXBW120},                  // channel filter bandwidth:120kHz
    {HRF_ADDR_BITRATEMSB, 0x1A},                       // bitrate:4800b/s
    {HRF_ADDR_BITRATELSB, 0x0B},                       // bitrate:4800b/s
    {HRF_ADDR_PREAMBLELSB, 0},                         // preamble size LSB
    {HRF_ADDR_SYNCCONFIG, HRF_VAL_SYNCCONFIG0},        // Size of sync word (disabled)
    {HRF_ADDR_PACKETCONFIG1, 0x80},                    // Tx Variable length, no Manchester coding
    {HRF_ADDR_PAYLOADLEN, 0}                           // no payload length

};
#define CONFIG_OOK_COUNT (sizeof(config_OOK) / sizeof(HRF_CONFIG_REC))

/***** MODULE STATE *****/

typedef uint8_t RADIO_MODE; // Stores HRF_MODE_xxx

typedef struct
{
    RADIO_MODULATION modu;
    RADIO_MODE mode;
} RADIO_DATA;

RADIO_DATA radio_data;

/***** PRIVATE ***************************************************************/

/*---------------------------------------------------------------------------*/
// Load a table of configuration values into HRF registers

static void _config(HRF_CONFIG_REC *config, uint8_t count)
{
    while (count-- != 0)
    {
        HRF_writereg(config->addr, config->value);
        config++;
    }
}

/*---------------------------------------------------------------------------*/
// Change the operating mode of the HRF radio (includes standby)

static void _change_mode(uint8_t mode)
{
    HRF_writereg(HRF_ADDR_OPMODE, mode);
    _wait_ready();
    gpio_low(LED_GREEN); // TX OFF
    gpio_low(LED_RED);   // RX OFF

    if (mode == HRF_MODE_TRANSMITTER)
    {
        _wait_txready();
        gpio_high(LED_RED); // TX ON
    }
    else if (mode == HRF_MODE_RECEIVER)
    {
        gpio_high(LED_GREEN); // RX ON
    }
    radio_data.mode = mode;
}

/*---------------------------------------------------------------------------*/
// Wait for HRF to be ready after last command

static void _wait_ready(void)
{
    #if defined(FULLTRACE)
        TRACE_OUTS("_wait_ready\n");
    # endif
    HRF_pollreg(HRF_ADDR_IRQFLAGS1, HRF_MASK_MODEREADY, HRF_MASK_MODEREADY);
}

/*---------------------------------------------------------------------------*/
// Wait for the HRF to be ready, and ready for tx, after last command

static void _wait_txready(void)
{
    #if defined(FULLTRACE)
        TRACE_OUTS("_wait_txready\n");
    #endif
    HRF_pollreg(HRF_ADDR_IRQFLAGS1, HRF_MASK_MODEREADY | HRF_MASK_TXREADY, HRF_MASK_MODEREADY | HRF_MASK_TXREADY);
}

/*---------------------------------------------------------------------------*/

/***** PUBLIC ****************************************************************/

/*---------------------------------------------------------------------------*/

void radio_reset(void)
{
    gpio_high(RESET);
    delayms(150);

    gpio_low(RESET);
    delayus(100);
}

/*---------------------------------------------------------------------------*/

// @achronite - Feb 2020 - return -ve when radio or gpio/spi broken
int radio_init(void)
{
    TRACE_OUTS("radio_init()\n");

    //gpio_init(); done by spi_init at moment
    int ret = spi_init(&radioConfig);

    if (ret == 0)
    {
        gpio_setout(RESET);
        gpio_low(RESET);
        gpio_setout(LED_RED);
        gpio_setout(LED_GREEN);

        // flash both LEDs to show initialise working
        gpio_high(LED_GREEN);
        gpio_high(LED_RED);

        radio_reset();

        TRACE_OUTS("radio_ver=");
        uint8_t rv = radio_get_ver();
        TRACE_OUTN(rv);
        TRACE_NL();
        if (rv < EXPECTED_RADIOVER)
        {
            TRACE_OUTS("warning:unexpected radio ver<min\n");
            //TRACE_FAIL("unexpected radio ver<min\n");
            return ERR_RADIO_MIN;
        }
        else if (rv > EXPECTED_RADIOVER)
        {
            TRACE_OUTS("warning:unexpected radio ver>exp\n");
            return ERR_RADIO_MAX;
        }

        radio_standby();
    }
    return ret;
}

/*---------------------------------------------------------------------------*/

uint8_t radio_get_ver(void)
{
    return HRF_readreg(HRF_ADDR_VERSION);
}

/*---------------------------------------------------------------------------*/

void radio_modulation(RADIO_MODULATION mod)
{
    if (mod == RADIO_MODULATION_OOK)
    {
        _config(config_OOK, CONFIG_OOK_COUNT);
        radio_data.modu = mod;
    }
    else if (mod == RADIO_MODULATION_FSK)
    {
        _config(config_FSK, CONFIG_FSK_COUNT);
        radio_data.modu = mod;
    }
    else //TODO: make this ASSERT()
    {
        TRACE_FAIL("Unknown modulation\n");
    }
}

/*---------------------------------------------------------------------------*/
// Put radio into transmit mode for chosen modulation scheme

void radio_transmitter(RADIO_MODULATION mod)
{
    TRACE_OUTS("radio_transmitter\n");

    radio_modulation(mod);
    _change_mode(HRF_MODE_TRANSMITTER);
}

/*---------------------------------------------------------------------------*/
// Put radio into receive mode for chosen modulation scheme
// This will open up the receive window, so that a packet can come in
// and be detected later by radio_isReceiveWaiting

void radio_receiver(RADIO_MODULATION mod)
{
    TRACE_OUTS("radio_receiver\n");

    radio_modulation(mod);
    _change_mode(HRF_MODE_RECEIVER);
}

/*---------------------------------------------------------------------------*/

void radio_standby(void)
{
    TRACE_OUTS("radio_standby\n");
    _change_mode(HRF_MODE_STANDBY);
}

/*---------------------------------------------------------------------------*/

void radio_transmit(uint8_t *payload, uint8_t len, uint8_t times)
{
    TRACE_OUTS("radio_transmit\n");

    uint8_t prevmode = radio_data.mode;
    if (radio_data.mode != HRF_MODE_TRANSMITTER)
    {
        _change_mode(HRF_MODE_TRANSMITTER);
    }

    radio_send_payload(payload, len, times);

    if (radio_data.mode != prevmode)
    {
        // This does not work when switching between OOK & FSK, as it only changes the mode without reloading registers
        // use radio_setmode(RADIO_MODULATION mod, RADIO_MODE mode) instead
        _change_mode(prevmode);
    }
}

/*---------------------------------------------------------------------------*/
// Send a payload of data

void radio_send_payload(uint8_t *payload, uint8_t len, uint8_t times)
{
    TRACE_OUTS("radio_send_payload(): ");

    // Note, when PA starts up, radio inserts a 01 at start before any user data
    // we might need to pad away from this by sending a sync of many zero bits
    // to prevent it being misinterpreted as a preamble, and prevent it causing
    // the first bit of the preamble being twice the length it should be in the
    // first packet.
    // Also need to confirm this bit only occurs when transmit actually starts,
    // and not on every FIFO load.

    int i;

    /* VALIDATE: Check input parameters are in range */
    if (times == 0 || len == 0) //TODO: make this an ASSERT()
    {
        TRACE_FAIL("zero times or payloadlen\n");
    }
    if (len > 32) //TODO: make this an ASSERT()
    {
        TRACE_FAIL("payload length>32\n");
    }

    /* CONFIGURE: Setup the radio for transmit of the correct payload length */
    // TRACE_OUTS("config\n");
    // Start transmitting when a full payload is loaded. So for '15':
    // level triggers when it 'strictly exceeds' level (i.e. 16 bytes starts tx,
    // and <=15 bytes triggers fifolevel irqflag to be cleared)
    // We already know from earlier that payloadlen<=32 (which fits into half a FIFO)
    HRF_writereg(HRF_ADDR_FIFOTHRESH, len - 1);

    /* TRANSMIT: Transmit a number of payloads back to back */
    TRACE_OUTN(times);
    TRACE_OUTS(" tx payloads\n");
    /*
#if defined(TRACE)
    for (i = 0; i < len; i++)
    {
        TRACE_OUTN(payload[i]);
        TRACE_OUTC(',');
    }
    TRACE_NL();
#endif
*/

    // send a number of payload repeats for the whole packet burst
    for (i = 0; i < times; i++)
    {
        HRF_writefifo_burst(payload, len);
        // Tx will auto start when fifolevel is exceeded by loading the payload
        // so the level register must be correct for the size of the payload
        // otherwise transmit will never start.
        /* wait for FIFO to not exceed threshold level */
        HRF_pollreg(HRF_ADDR_IRQFLAGS2, HRF_MASK_FIFOLEVEL, 0);
        #if defined(FULLTRACE)
            TRACE_OUTC('|');
        #endif
    }

    // wait for FIFO empty, to indicate transmission completed
    HRF_pollreg(HRF_ADDR_IRQFLAGS2, HRF_MASK_FIFONOTEMPTY, 0);

    /* CONFIRM: Was the transmit ok? */
    // Check final flags in case of overruns etc
#if defined(FULLTRACE)
    uint8_t irqflags1 = HRF_readreg(HRF_ADDR_IRQFLAGS1);
    uint8_t irqflags2 = HRF_readreg(HRF_ADDR_IRQFLAGS2);
    TRACE_OUTS("irqflags1,2=");
    TRACE_OUTN(irqflags1);
    TRACE_OUTC(',');
    TRACE_OUTN(irqflags2);
    TRACE_NL();

    //TODO: make this TRACE_ASSERT()
    if (((irqflags2 & HRF_MASK_FIFONOTEMPTY) != 0) || ((irqflags2 & HRF_MASK_FIFOOVERRUN) != 0))
    {
        TRACE_FAIL("ERROR: FIFO not empty or overrun at end of burst");
    }
#endif
}

/*---------------------------------------------------------------------------*/
// Check to see if a payload is waiting in the receive buffer

bool radio_is_receive_waiting(void)
{
    uint8_t irqflags2 = HRF_readreg(HRF_ADDR_IRQFLAGS2);
    return (irqflags2 & HRF_MASK_PAYLOADRDY) == HRF_MASK_PAYLOADRDY;
    /*
    if (_payload_waiting())
    {
        return RADIO_RESULT_OK_TRUE;
    }
    return RADIO_RESULT_OK_FALSE;
*/
}

/*---------------------------------------------------------------------------*/
// read a single payload from the payload buffer
// this reads a fixed length payload

RADIO_RESULT radio_get_payload_len(uint8_t *buf, uint8_t buflen)
{
    if (buflen > MAX_FIFO_BUFFER)
    { /* At the moment, the receiver cannot reliably cope with payloads > 1 FIFO buffer.
        * It *might* be able to in the future.
        */
        return RADIO_RESULT_ERR_LONG_PAYLOAD;
    }
    HRF_RESULT r = HRF_readfifo_burst_len(buf, buflen);
    if (r != HRF_RESULT_OK)
    {
        return RADIO_RESULT_ERR_READ_FAILED;
    }
    return RADIO_RESULT_OK;
}

/*---------------------------------------------------------------------------*/
// read a single payload from the payload buffer
// this reads count byte preceeded payloads.
// The CBP payload always has the count byte in the first byte
// and this is returned in the user buffer too.

RADIO_RESULT radio_get_payload_cbp(uint8_t *buf, uint8_t buflen)
{
    ////if (buflen > MAX_FIFO_BUFFER)
    ////{  /* At the moment, the receiver cannot reliably cope with payloads > 1 FIFO buffer.
    ////    * It *might* be able to in the future.
    ////    */
    ////    return RADIO_RESULT_ERR_LONG_PAYLOAD;
    ////}
    HRF_RESULT r = HRF_readfifo_burst_cbp(buf, buflen);
    if (r != HRF_RESULT_OK)
    {
        TRACE_OUTS("radio_get_payload_cbp failed, error=");
        TRACE_OUTN(r);
        TRACE_NL();
        return RADIO_RESULT_ERR_READ_FAILED;
    }
    return RADIO_RESULT_OK;
}

/*---------------------------------------------------------------------------*/

void radio_finished(void)
{
    TRACE_OUTS("radio_finished\n");
    //spi_finished();
    radio_standby();
    gpio_finished();

    // clear globals
    radio_data.modu = 99;
    radio_data.mode = 99;
}

/* @Achronite - March 2019, January 2020
** New function that performs all mode switching of radio modulation and mode
** This version only writes HRF changes when required by using the previous state
** It allows switch between OOK Tx and FSK Rx unlike radio_transmit() above
**
** Please ensure that the radio device is mutex locked in multithreaded environment before calling
*/
void radio_setmode(RADIO_MODULATION mod, RADIO_MODE mode)
{
    // Only switch modulation if required
    if (mod != radio_data.modu)
    {
        // modulation change
        if (mod == RADIO_MODULATION_OOK)
        {
            _config(config_OOK, CONFIG_OOK_COUNT);
            radio_data.modu = RADIO_MODULATION_OOK;
        }
        else
        {
            // assume FSK if not OOK
            _config(config_FSK, CONFIG_FSK_COUNT);
            radio_data.modu = RADIO_MODULATION_FSK;
        }
    }

    // Only switch mode if required
    if (mode != radio_data.mode)
    {
        // mode change
        _change_mode(mode);
        radio_data.mode = mode;
    }
}

/* radio_mod_transmit()
**
** New function that caters for previous modulation switching when transmitting data
** cloned from radio_transmit() to add extra modulation param
**
** Please ensure that the radio device is mutex locked in multithreaded environment before calling
**
** @Achronite - March 2019
**/

void radio_mod_transmit(RADIO_MODULATION mod, uint8_t *payload, uint8_t len, uint8_t times)
{
    #if defined(FULLTRACE)
        TRACE_OUTS("radio_mod_transmit()\n");
    #endif

    // preserve previous mode & modulation
    uint8_t prevmod = radio_data.modu;
    uint8_t prevmode = radio_data.mode;

    if (prevmode != HRF_MODE_TRANSMITTER || prevmod != mod)
    {
        radio_setmode(mod, HRF_MODE_TRANSMITTER);
        radio_send_payload(payload, len, times);
        radio_setmode(prevmod, prevmode);
    }
    else
    {
        // already in correct transmit only mode
        radio_send_payload(payload, len, times);

        // place radio into standby after sending payload
        radio_standby();
    }
}
/***** END OF FILE *****/
