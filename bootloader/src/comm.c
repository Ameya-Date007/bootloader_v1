#include "comm.h"
#include "core/uart.h"
#include "core/crc8.h"

#define PACKET_BUFF_LENGTH (8)

typedef enum comm_state_t{
    COMM_STATE_LENGTH,
    COMM_STATE_DATA,
    COMM_STATE_CRC
} comm_state_t;

static comm_packet_t temp_packet = {.length = 0, .data = {0}, .crc = 0}; //.struct_membernames are a feature of C99, where we can initialise certain strcut members' values.
static comm_packet_t retx_packet = {.length = 0, .data = {0}, .crc = 0};
static comm_packet_t ack_packet = {.length = 0, .data = {0}, .crc = 0};
static comm_packet_t last_tx_packet = {.length = 0, .data = {0}, .crc = 0};

// Creating a buffer of comm_packet_t type to store a few packets --> Why some? To avoid UART buffer overflow.
static comm_packet_t packet_buffer[PACKET_BUFF_LENGTH];
static uint32_t read_idx = 0;
static uint32_t write_idx = 0;
static uint32_t packet_buffer_mask = PACKET_BUFF_LENGTH - 1;

static comm_state_t state = COMM_STATE_LENGTH;
static uint8_t data_byte_count = 0;

static bool is_single_byte_packet(const comm_packet_t * packet, uint8_t byte){
    if(packet->length != 1){
        return false;
    }

    if (packet -> data[0] != byte){
        return false;
    }
    
    for (uint8_t i = 0; i < PACKET_DATA_LENGTH; i++){
        if(packet ->data[i] != 0xff){
            return false;
        }
    }
        return true;
}

static void copy_packet(const comm_packet_t * source, comm_packet_t * dest){
    dest -> length = source -> length;
    for (uint8_t i = 1; i < PACKET_DATA_LENGTH; i++){
        dest -> data[i] = source -> data[i];
    }
    dest -> crc = source -> crc;
}

void comm_setup(void){
    retx_packet.length = 1;
    retx_packet.data[0] = RETX_DATA0_PACKET;
    for (uint8_t i = 1; i < PACKET_DATA_LENGTH; i++){
        retx_packet.data[i] = 0xff;
    }
    retx_packet.crc = comm_compute_crc(&retx_packet);


    ack_packet.length = 1;
    ack_packet.data[0] = ACK_DATA0_PACKET;
    for (uint8_t i = 1; i < PACKET_DATA_LENGTH; i++){
        ack_packet.data[i] = 0xff;
    }
    ack_packet.crc = comm_compute_crc(&ack_packet);
}

void comm_update(void){
    while(uart_data_available()){
        switch (state)
        {
        case COMM_STATE_LENGTH:
            temp_packet.length = uart_read_byte();
            state = COMM_STATE_DATA;
            break;

        case COMM_STATE_DATA:
            temp_packet.data[data_byte_count++] = uart_read_byte();
            if(data_byte_count >= PACKET_DATA_LENGTH)
            state = COMM_STATE_CRC;
            break;
        case COMM_STATE_CRC:
            temp_packet.crc = uart_read_byte();

            if(temp_packet.crc != comm_compute_crc(&temp_packet)){
                comm_send_packet(&retx_packet);
                state = COMM_STATE_LENGTH;
                break;
            }

            if(is_single_byte_packet(&temp_packet, RETX_DATA0_PACKET)){
                comm_send_packet(&last_tx_packet);
                state = COMM_STATE_LENGTH;
                break;
            }

            if(is_single_byte_packet(&temp_packet, ACK_DATA0_PACKET)){
                state = COMM_STATE_LENGTH;
                break;
            }
            else{
                uint32_t next_write_idx = (write_idx + 1) &packet_buffer_mask;
                copy_packet(&temp_packet, &packet_buffer[write_idx]);
                write_idx = next_write_idx;
                comm_send_packet(&ack_packet);
            }

            break;
        default:
        state = COMM_STATE_LENGTH;
            break;
        }
    }
}
void packets_available(void){
    return write_idx != read_idx;
}
void comm_send_packet(const comm_packet_t* packet){
    uart_write((uint8_t) packet, PACKET_SIZE);
    copy_packet(packet, &last_tx_packet);
}
void comm_receive_packet(comm_packet_t * packet){
    copy_packet(&packet_buffer[read_idx], packet);
    read_idx = (read_idx + 1) & packet_buffer_mask;
}
uint8_t comm_compute_crc(comm_packet_t * packet){
    return crc8((uint8_t)packet, PACKET_SIZE - PACKET_CRC_BYTES);
}