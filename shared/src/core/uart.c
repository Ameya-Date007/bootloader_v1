#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include "core/ring_buffer.h"
#include "core/uart.h"

#define BAUD_RATE (115200)
#define RING_BUFFER_SIZE (64)

static ring_buffer_t rb = {0U};
static uint8_t data_buffer[RING_BUFFER_SIZE] = {0U};

//static volatile uint8_t data_buffer = 0U;
//static volatile bool data_available = false;

void usart1_isr(void){
    const bool overrun_occurred = usart_get_flag(USART1, USART_FLAG_ORE) == 1;
    const bool received_data = usart_get_flag(USART1, USART_FLAG_RXNE) == 1;

    if(received_data || overrun_occurred){
        //data_buffer = (uint8_t) usart_recv(USART1);
        //data_available = true;
        if(ring_buffer_write(&rb, (uint8_t) usart_recv(USART1))){

        }
    }
}

void uart_setup(void){
    ring_buffer_setup(&rb, data_buffer, RING_BUFFER_SIZE);
    rcc_periph_clock_enable(RCC_USART1);

    usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
    usart_set_baudrate(USART1, BAUD_RATE);
    usart_set_databits(USART1, 8);
    usart_set_parity(USART1, 0);
    usart_set_stopbits(USART1, 1);

    usart_set_mode(USART1, USART_MODE_TX_RX);
    usart_enable_rx_interrupt(USART1);
    nvic_enable_irq(NVIC_USART1_IRQ);

    usart_enable(USART1);

}
void uart_write(uint8_t *data, const uint32_t length){
    for (uint32_t i = 0; i < length; i++){
        uart_write_byte(data[i]);
    }
    
}
void uart_write_byte(uint8_t data){
    usart_send_blocking(USART1, (uint16_t) data);
}
uint32_t uart_read(uint8_t * data, const uint32_t buffer_length){
    if(buffer_length == 0){
        return 0;
    } 
    for(uint32_t bytes_read = 0; bytes_read < buffer_length; bytes_read++){
        if(!ring_buffer_read(&rb, &data[bytes_read])){
            return bytes_read;
        }
    }
    return buffer_length;
   /*if(buffer_length > 0 && data_available){
    *data = data_buffer;
    data_available = false;
    return 1;
   }
   return 0;
   */
}
uint8_t uart_read_byte (void){
    // a temp variable to read the uart data from the ring buffer.
    uint8_t byte = 0;
    // The address of the variable is given as the data should be given to it, which will further be used to return it to the host.
    (void)uart_read(&byte,1);

    //The final received value is returned to the host (if required).
    return byte;
}
bool uart_data_available (void){
    return !ring_buffer_empty(&rb);
    //return data_available;
}