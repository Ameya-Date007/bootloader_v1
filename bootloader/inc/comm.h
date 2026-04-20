#ifndef INCL_COMM_H
#define INCL_COMM_H
#include "common-defines.h"

#define PACKET_DATA_LENGTH  (16)
#define PACKET_LENGTH_BYTES (1)
#define PACKET_CRC_BYTES    (1)
#define PACKET_SIZE         (PACKET_LENGTH_BYTES + PACKET_DATA_LENGTH + PACKET_CRC_BYTES)

#define RETX_DATA0_PACKET    (0x19)
#define ACK_DATA0_PACKET     (0x15)


typedef struct comms_packet_t{
    uint8_t length;
    uint8_t data[PACKET_DATA_LENGTH];
    uint8_t crc;
}comm_packet_t;

void comm_setup(void);
void comm_update(void);
void packets_available(void);
void comm_send_packet(const comm_packet_t* packet);
void comm_receive_packet(comm_packet_t * packet);
uint8_t comm_compute_crc(comm_packet_t * packet);

#endif