#include "radio.h"
#include <hardware/spi.h>
#include <pico/stdlib.h>
#include <string.h>

namespace {
constexpr uint PIN_CE = 11, PIN_MISO = 12, PIN_CSN = 13, PIN_SCK = 14, PIN_MOSI = 15;
constexpr uint8_t CONFIG = 0x00, EN_AA = 0x01, EN_RXADDR = 0x02, SETUP_AW = 0x03;
constexpr uint8_t SETUP_RETR = 0x04, RF_CH = 0x05, RF_SETUP = 0x06, STATUS = 0x07;
constexpr uint8_t OBSERVE_TX = 0x08, RX_ADDR_P0 = 0x0a, RX_ADDR_P1 = 0x0b;
constexpr uint8_t TX_ADDR = 0x10, RX_PW_P0 = 0x11, RX_PW_P1 = 0x12;
constexpr uint8_t FIFO_STATUS = 0x17, DYNPD = 0x1c, FEATURE = 0x1d;
constexpr uint8_t RX_DR = 0x40, TX_DS = 0x20, MAX_RT = 0x10;
constexpr uint8_t RX_CONFIG = 0x0f, TX_CONFIG = 0x0e;
constexpr uint64_t TX_TIMEOUT_US = 20000;
bool initialized = false, sending = false, spi_initialized = false;
bool auto_ack = true;
uint32_t spi_baud = 0;
uint64_t tx_started = 0;
RadioInitError init_error = {};

void command(uint8_t cmd, const uint8_t* out = nullptr, uint8_t* in = nullptr, size_t size = 0) {
    gpio_put(PIN_CSN, 0);
    spi_write_blocking(spi1, &cmd, 1);
    if (out) spi_write_blocking(spi1, out, size);
    if (in) spi_read_blocking(spi1, 0xff, in, size);
    gpio_put(PIN_CSN, 1);
    // CSN must stay high for at least 50 ns between commands, even after inlining.
    sleep_us(1);
}

void write_register(uint8_t reg, uint8_t value) { command(0x20 | reg, &value, nullptr, 1); }
uint8_t read_register(uint8_t reg) {
    uint8_t value;
    command(reg, nullptr, &value, 1);
    return value;
}

bool check_register(uint8_t reg, uint8_t expected, uint8_t mask = 0xff) {
    uint8_t actual = read_register(reg);
    if ((actual & mask) == expected) return true;
    init_error = {reg, 0, expected, actual};
    return false;
}

bool check_address(uint8_t reg, const uint8_t* expected) {
    uint8_t actual[5];
    command(reg, nullptr, actual, 5);
    for (uint8_t i = 0; i < 5; ++i) {
        if (actual[i] != expected[i]) {
            init_error = {reg, i, expected[i], actual[i]};
            return false;
        }
    }
    return true;
}

void listen() {
    write_register(CONFIG, RX_CONFIG);
    gpio_put(PIN_CE, 1);
    sleep_us(130);
}
}

bool radio_init(const RadioConfig& config) {
    init_error = {};
    if (config.channel > 83 || config.retries > 15 ||
        config.local_address[0] == config.peer_address[0]) return false;
    if (spi_initialized) radio_shutdown();
    gpio_init(PIN_CE);
    gpio_put(PIN_CE, 0);
    gpio_set_dir(PIN_CE, GPIO_OUT);
    gpio_init(PIN_CSN);
    gpio_put(PIN_CSN, 1);
    gpio_set_dir(PIN_CSN, GPIO_OUT);
    spi_baud = spi_init(spi1, 4000000);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    spi_initialized = true;
    sleep_ms(100);

    write_register(CONFIG, 0x0c);
    uint8_t ack_pipes = config.auto_ack ? 0x03 : 0;
    write_register(EN_AA, ack_pipes);
    write_register(EN_RXADDR, 0x03);
    write_register(SETUP_AW, 0x03);
    write_register(SETUP_RETR, 0x10 | config.retries); // 500 us between retries.
    write_register(RF_CH, config.channel);
    write_register(RF_SETUP, 0x06); // 1 Mbit/s, maximum chip RF power 0 dBm; module PA gain is separate.
    write_register(DYNPD, 0);
    write_register(FEATURE, 0);
    command(0x20 | RX_ADDR_P1, config.local_address, nullptr, 5);
    command(0x20 | TX_ADDR, config.peer_address, nullptr, 5);
    // Pipe 0 must match TX_ADDR to receive automatic ACKs while transmitting.
    command(0x20 | RX_ADDR_P0, config.peer_address, nullptr, 5);
    write_register(RX_PW_P0, RADIO_PAYLOAD_SIZE);
    write_register(RX_PW_P1, RADIO_PAYLOAD_SIZE);
    command(0xe1); // FLUSH_TX
    command(0xe2); // FLUSH_RX
    write_register(STATUS, RX_DR | TX_DS | MAX_RT);

    bool valid = check_address(RX_ADDR_P1, config.local_address) &&
                 check_address(TX_ADDR, config.peer_address) &&
                 check_address(RX_ADDR_P0, config.peer_address) &&
                 check_register(SETUP_AW, 3) && check_register(RF_CH, config.channel) &&
                 check_register(RF_SETUP, 0x06, 0x2e) &&
                 check_register(SETUP_RETR, 0x10 | config.retries) &&
                 check_register(EN_AA, ack_pipes) && check_register(EN_RXADDR, 3) &&
                 check_register(RX_PW_P0, 32) && check_register(RX_PW_P1, 32) &&
                 check_register(FEATURE, 0) && check_register(DYNPD, 0);
    if (!valid) { radio_shutdown(); return false; }

    write_register(CONFIG, RX_CONFIG);
    sleep_ms(5); // Allow the radio oscillator to settle after power-up.
    if (!check_register(CONFIG, RX_CONFIG)) { radio_shutdown(); return false; }
    initialized = true;
    auto_ack = config.auto_ack;
    gpio_put(PIN_CE, 1);
    sleep_us(130);
    return true;
}

void radio_shutdown() {
    if (spi_initialized) {
        gpio_put(PIN_CE, 0);
        write_register(CONFIG, 0x0c);
        command(0xe1);
        command(0xe2);
        write_register(STATUS, RX_DR | TX_DS | MAX_RT);
    }
    initialized = sending = false;
}

uint32_t radio_spi_baud() { return spi_baud; }
RadioInitError radio_init_error() { return init_error; }

bool radio_send(const uint8_t* payload) {
    if (!initialized || sending || !payload) return false;
    gpio_put(PIN_CE, 0);
    // RX data can be read before its automatic ACK finishes. Let that ACK end
    // before changing modes; this guard is for our fixed 1 Mbit/s configuration.
    sleep_us(280);
    write_register(CONFIG, TX_CONFIG);
    write_register(STATUS, TX_DS | MAX_RT);
    command(0xa0, payload, nullptr, RADIO_PAYLOAD_SIZE);
    tx_started = time_us_64();
    sending = true;
    gpio_put(PIN_CE, 1);
    sleep_us(15);
    gpio_put(PIN_CE, 0);
    return true;
}

RadioTxResult radio_poll_tx() {
    if (!sending) return {RadioTxStatus::idle, 0};
    uint8_t status = read_register(STATUS);
    RadioTxStatus result = RadioTxStatus::pending;
    if (status & MAX_RT) result = RadioTxStatus::failed;
    else if (status & TX_DS) result = auto_ack ? RadioTxStatus::acked : RadioTxStatus::sent;
    else if (time_us_64() - tx_started >= TX_TIMEOUT_US) result = RadioTxStatus::timed_out;
    if (result == RadioTxStatus::pending) return {result, 0};
    uint8_t retries = auto_ack ? read_register(OBSERVE_TX) & 0x0f : 0;
    write_register(STATUS, TX_DS | MAX_RT);
    if (result != RadioTxStatus::acked && result != RadioTxStatus::sent) command(0xe1);
    sending = false;
    listen();
    return {result, retries};
}

bool radio_receive(uint8_t* payload) {
    if (!initialized || sending || !payload || (read_register(FIFO_STATUS) & 1)) return false;
    command(0x61, nullptr, payload, RADIO_PAYLOAD_SIZE);
    write_register(STATUS, RX_DR);
    return true;
}
