#ifndef INCL_COMM_H
#define INCL_COMM_H
#include "common-defines.h"

#define PACKET_DATA_LENGTH  (16)
#define PACKET_LENGTH_BYTES (1)
#define PACKET_CRC_BYTES    (1)
#define PACKET_SIZE         (PACKET_LENGTH_BYTES + PACKET_DATA_LENGTH + PACKET_CRC_BYTES)

#define RETX_DATA0_PACKET    (0x19)
#define ACK_DATA0_PACKET     (0x15)

//------ FIRMWARE UPDATE STATE_MACHINE RELATED PACKETS ------
#define BL_PACKET_SYNC_OBSERVED_DATA0           (0x20)
#define BL_PACKET_FW_UPDATE_REQ_DATA0           (0x31)
#define BL_PACKET_FW_UPDATE_RES_DATA0           (0x37)
#define BL_PACKET_DEVICE_ID_REQ_DATA0           (0x3C)
#define BL_PACKET_DEVICE_ID_RES_DATA0           (0x3F)
#define BL_PACKET_FW_LENGTH_REQ_DATA0           (0x42)
#define BL_PACKET_FW_LENGTH_RES_DATA0           (0x45)
#define BL_PACKET_READY_FOR_DATA_DATA0          (0x48)
#define BL_PACKET_FW_UPDATE_SUCCESSFUL_DATA0    (0x54)
#define BL_PACKET_NACK_DATA0                    (0x59)

typedef struct comms_packet_t{
    uint8_t length;
    uint8_t data[PACKET_DATA_LENGTH];
    uint8_t crc;
}comm_packet_t;

void comm_setup(void);
void comm_update(void);
bool packets_available(void);
void comm_send_packet(const comm_packet_t* packet);
void comm_receive_packet(comm_packet_t * packet);
bool is_single_byte_packet(const comm_packet_t * packet, uint8_t byte);
void comm_create_single_byte_packet(comm_packet_t * packet, uint8_t byte);
uint8_t comm_compute_crc(comm_packet_t * packet);

#endif