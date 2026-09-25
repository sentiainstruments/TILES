#include "din_midi.h"

#include "board_pins.h"
#include "din_midi_queue.h"
#include "din_midi_tx.pio.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/uart.h"

#define DIN_MIDI_BAUD 31250u
#define DIN_UART uart0
#define DIN_UART_IRQ UART0_IRQ
/* PIO cycles per bit in din_midi_tx.pio. */
#define DIN_TX_CYCLES_PER_BIT 8u

static bool s_ready;
static PIO s_pio;
static uint s_sm;
static uint s_offset;
static tiles_din_midi_out_line_t s_line;

static uint line_gpio(tiles_din_midi_out_line_t line) {
    return (line == TILES_DIN_MIDI_OUT_LINE_B) ? TILES_GPIO_DIN_MIDI_OUT_B : TILES_GPIO_DIN_MIDI_OUT_A;
}

/* ---- TX: interrupt-fed PIO transmitter ---- */

static void tx_irq_source(bool enabled) {
    pio_set_irqn_source_enabled(s_pio, 0, pio_get_tx_fifo_not_full_interrupt_source(s_sm), enabled);
}

/* Fires while the PIO TX FIFO has room. Moves bytes from the queue into it;
 * when the queue runs dry, turns its own source off (level-triggered -- left
 * on with nothing to send it would fire forever). The has_data re-check
 * closes the race where main queued a byte between "queue empty" and the
 * disable, which would otherwise strand that byte until the next kick. */
static void din_tx_irq_handler(void) {
    while (!pio_sm_is_tx_fifo_full(s_pio, s_sm)) {
        uint8_t byte;
        if (!tiles_din_queue_pop_tx_byte(&byte)) {
            tx_irq_source(false);
            if (tiles_din_queue_tx_has_data()) {
                tx_irq_source(true);
            }
            return;
        }
        pio_sm_put(s_pio, s_sm, (uint32_t)byte);
    }
}

/* (Re)points the transmitter at `line`'s GPIO and parks the other line high.
 * Order matters for the current loop: MIDI is "no current" while both lines
 * are equal, so the pin being taken over is first driven high through SIO,
 * then handed to PIO already high (pio_sm_set_pins_with_mask), and the
 * released pin ends up SIO-high -- neither transition ever pulls one line
 * low, so the receiver never sees a spurious start bit. */
static void tx_configure_line(tiles_din_midi_out_line_t line) {
    uint signal = line_gpio(line);
    uint idle = line_gpio(line == TILES_DIN_MIDI_OUT_LINE_A ? TILES_DIN_MIDI_OUT_LINE_B : TILES_DIN_MIDI_OUT_LINE_A);

    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);

    gpio_put(idle, 1);
    gpio_set_dir(idle, GPIO_OUT);
    gpio_set_function(idle, GPIO_FUNC_SIO);
    gpio_put(signal, 1);
    gpio_set_dir(signal, GPIO_OUT);

    /* Same order as Raspberry Pi's uart_tx example: level and direction first
     * (pin goes to PIO already high), then the state machine config. */
    pio_sm_set_pins_with_mask(s_pio, s_sm, 1u << signal, 1u << signal);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 1u << signal, 1u << signal);
    pio_gpio_init(s_pio, signal);

    pio_sm_config c = din_uart_tx_program_get_default_config(s_offset);
    sm_config_set_out_pins(&c, signal, 1);
    sm_config_set_sideset_pins(&c, signal);
    /* LSB first, no autopull -- the program's own `pull` fetches each byte. */
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (float)(DIN_TX_CYCLES_PER_BIT * DIN_MIDI_BAUD));
    pio_sm_init(s_pio, s_sm, s_offset, &c);
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_line = line;
}

/* ---- RX: UART0 RX interrupt into a ring ---- */

static void din_uart_irq_handler(void) {
    uart_hw_t *hw = uart_get_hw(DIN_UART);
    while (uart_is_readable(DIN_UART)) {
        uint32_t dr = hw->dr;
        if ((dr & (UART_UARTDR_OE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_FE_BITS)) != 0u) {
            /* Framing error / break / overrun: this byte can't be trusted and
             * the parser must not stitch the neighbours across the gap. */
            tiles_din_queue_rx_flag_loss();
            continue;
        }
        (void)tiles_din_queue_rx_push((uint8_t)(dr & 0xFFu));
    }
}

bool tiles_din_midi_init(void) {
    s_ready = false;
    tiles_din_queue_init();

    /* Any PIO block with a free state machine and instruction space will do
     * (lighting.c's SK6805 chains already use some of pio0). The program
     * must be able to reach BOTH output lines (GP0..GP2). */
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&din_uart_tx_program, &s_pio, &s_sm, &s_offset,
                                                          TILES_GPIO_DIN_MIDI_OUT_A, 3u, true)) {
        return false;
    }

    tx_configure_line(TILES_DIN_MIDI_OUT_DEFAULT_LINE);
    int irq = pio_get_irq_num(s_pio, 0);
    irq_set_exclusive_handler((uint)irq, din_tx_irq_handler);
    irq_set_enabled((uint)irq, true);

    /* RX. UART0's TX half is initialized too (uart_init enables both) but its
     * pin function is never selected, so it drives nothing. */
    uart_init(DIN_UART, DIN_MIDI_BAUD);
    gpio_set_function(TILES_GPIO_DIN_MIDI_IN_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(DIN_UART, false, false);
    uart_set_format(DIN_UART, 8u, 1u, UART_PARITY_NONE);
    uart_set_fifo_enabled(DIN_UART, true);
    /* Interrupt as soon as 1/8 of the 32-byte RX FIFO (4 bytes) is filled,
     * not at the 1/2 reset default: anything shorter than the threshold only
     * gets serviced by the ~1 ms receive-timeout interrupt, and a 3-byte
     * Note-On is shorter than any threshold. The FIFO itself stays enabled
     * for what it's really for -- ~10 ms of slack if the main loop or a flash
     * write holds interrupts off, instead of losing every byte in that time. */
    uart_get_hw(DIN_UART)->ifls = (0u << UART_UARTIFLS_TXIFLSEL_LSB) | (0u << UART_UARTIFLS_RXIFLSEL_LSB);
    while (uart_is_readable(DIN_UART)) {
        (void)uart_get_hw(DIN_UART)->dr; /* discard whatever the line did while it was unconfigured */
    }
    irq_set_exclusive_handler(DIN_UART_IRQ, din_uart_irq_handler);
    irq_set_enabled(DIN_UART_IRQ, true);
    uart_set_irq_enables(DIN_UART, true, false); /* RX data (+ receive timeout), no TX interrupt */

    s_ready = true;
    return true;
}

bool tiles_din_midi_is_ready(void) {
    return s_ready;
}

void tiles_din_midi_set_out_line(tiles_din_midi_out_line_t line) {
    if (!s_ready || line == s_line) {
        return;
    }
    tx_irq_source(false);
    tx_configure_line(line);
    if (tiles_din_queue_tx_has_data()) {
        tx_irq_source(true);
    }
}

static void tx_kick(void) {
    if (tiles_din_queue_tx_has_data()) {
        tx_irq_source(true);
    }
}

void tiles_din_midi_send(const uint8_t *msg, uint8_t len) {
    if (!s_ready) {
        return;
    }
    (void)tiles_din_queue_push_message(msg, len);
    tx_kick();
}

void tiles_din_midi_service(void) {
    if (!s_ready) {
        return;
    }
    (void)tiles_din_queue_service();
    tx_kick();
}

bool tiles_din_midi_rx_read_byte(uint8_t *byte) {
    return s_ready && tiles_din_queue_rx_pop(byte);
}

bool tiles_din_midi_rx_take_loss(void) {
    return s_ready && tiles_din_queue_rx_take_overflow();
}
