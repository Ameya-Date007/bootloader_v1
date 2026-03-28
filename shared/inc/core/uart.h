#ifndef INCL_UART_H
#define INCL_UART_H
#include "common-defines.h"

//Driver Interface

void uart_setup(void);
void uart_write(uint8_t *data, const uint32_t length);
void uart_write_byte(uint8_t data); //Write an immidiate single byte.
uint32_t uart_read(uint8_t * data_buffer, const uint32_t buffer_length);
uint8_t uart_read_byte (void);
bool uart_data_available (void);

#endif