#include "common-defines.h"
#include <libopencm3/stm32/f4/memorymap.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/vector.h>
#include <libopencm3/cm3/scb.h>

#include <string.h>

#include "core/system.h"
#include "bl-flash.h"
#include "core/simple-timer.h"
#include "core/uart.h"
#include "comm.h"
#include "core/firmware-info.h"
#include "aes.h"
#include "core/crc.h"

#define BOOTLOADER_SIZE (0x8000U) // 32KB ----> 1000 0000 0000 0000
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE) // BASE ADDRESS OF FLASH MEM: 0x0800_0000
#define MAX_FW_LENGTH ((1024U * 512U) - BOOTLOADER_SIZE)
#define UART_PORT (GPIOA)
#define UART_RX_PIN (GPIO10)
#define UART_TX_PIN (GPIO9)

//BOOTLOADER STATE MACHINE STATES
#define DEVICE_ID (0x42)

#define SYNC_SEQ_0 (0xc4)
#define SYNC_SEQ_1 (0x55)
#define SYNC_SEQ_2 (0x7e)
#define SYNC_SEQ_3 (0x10)

#define DEFAULT_TIMEOUT (5000)
typedef enum bt_state_t{
  BL_STATE_Sync,
  BL_STATE_WaitForUpdateReq,
  BL_STATE_DeviceIDReq,
  BL_STATE_DeviceIDRes,
  BL_STATE_FWLengthReq,
  BL_STATE_FWLengthRes,
  BL_STATE_EraseApplication,
  BL_STATE_ReceiveFirmware,
  BL_STATE_Done
}bt_state_t;

static bt_state_t state = BL_STATE_Sync;
static uint32_t fw_length = 0;
static uint32_t bytes_written = 0;
static uint8_t sync_seq[4] = {0};
static simple_timer_t timer;
static comm_packet_t temp_packet;

static const uint8_t secret_key [AES_BLOCK_SIZE] = {
  0x00, 0x01, 0x02, 0x03,
  0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b,
  0x0c, 0x0d, 0x0e, 0x0f
};

static void gpio_setup(void){
  rcc_periph_clock_enable(RCC_GPIOA);
  gpio_mode_setup(UART_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, UART_TX_PIN | UART_RX_PIN);
  gpio_set_af(UART_PORT, GPIO_AF7, UART_TX_PIN | UART_RX_PIN);
}

static void gpio_teardown(void){
  rcc_periph_clock_disable(RCC_GPIOA);
  gpio_mode_setup(UART_PORT, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, UART_TX_PIN | UART_RX_PIN);
}

static void jump_to_main(void){
  /* Method 1 */
  /*
  typedef void (*void_fn)(void);
  uint32_t * main_vector_table = (uint32_t *) MAIN_APP_START_ADDRESS;
  void_fn jump_fn = (void_fn) main_vector_table[1];

  jump_fn();
  */
  /* Method 2 :Using the vector_table_t struct containing all interrupt vectors*/
  vector_table_t * main_vector_table = (vector_table_t *) MAIN_APP_START_ADDRESS;
  main_vector_table -> reset(); // Calls the reset handler of the main application.

}

static void aes_cbc_mac_step(AES_Block_t aes_state, AES_Block_t prev_state, const AES_Block_t* key_schedule){
  //CBC Chaining Operation
  for (uint8_t i = 0; i < AES_BLOCK_SIZE; i++){
    ((uint8_t*) aes_state)[i] ^= ((uint8_t*) prev_state)[i];
  }

  AES_EncryptBlock(aes_state, key_schedule);
  memcpy(prev_state, aes_state, AES_BLOCK_SIZE);
}

static bool validate_firmware_image(void){
  firmware_info_t* firmware_info = (firmware_info_t*) FWINFO_ADDRESS;
  const uint8_t* signature = (uint8_t*) SIGNATURE_ADDRESS;

  if(firmware_info -> sentinel != FWINFO_SENTINEL){
    return false;
  }
  if(firmware_info -> device_id != DEVICE_ID){
    return false;
  }

  AES_Block_t round_keys[NUM_ROUND_KEYS_128];
  AES_KeySchedule128(secret_key, round_keys);

  AES_Block_t aes_state = {0};
  AES_Block_t prev_state = {0};   //IV

  uint8_t bytes_to_pad = 16 - (firmware_info->length % 16);
  if(bytes_to_pad == 0){
    bytes_to_pad = 16;
  }

  memcpy(prev_state, firmware_info, AES_BLOCK_SIZE);
  aes_cbc_mac_step(aes_state, prev_state, round_keys);

  uint32_t offset = 0;
  while (offset < firmware_info->length){
    if(offset == FWINFO_ADDRESS - MAIN_APP_START_ADDRESS){
      offset += AES_BLOCK_SIZE * 2;
      continue;
    }

    if(firmware_info -> length - offset > AES_BLOCK_SIZE){
      //Regular case
      memcpy(aes_state, (void*)(MAIN_APP_START_ADDRESS + offset), AES_BLOCK_SIZE);
      aes_cbc_mac_step(aes_state, prev_state, round_keys);
    } else {
      if(bytes_to_pad == 16){
        memcpy(aes_state, (void*)(MAIN_APP_START_ADDRESS + offset), AES_BLOCK_SIZE);
        aes_cbc_mac_step(aes_state, prev_state, round_keys);

        // Add whole padding
        memset(aes_state, AES_BLOCK_SIZE, AES_BLOCK_SIZE);
        aes_cbc_mac_step(aes_state, prev_state, round_keys);
      } else {
        memcpy(aes_state, (void*)(MAIN_APP_START_ADDRESS + offset), AES_BLOCK_SIZE - bytes_to_pad);
        memset((void*)aes_state + (AES_BLOCK_SIZE - bytes_to_pad), bytes_to_pad, bytes_to_pad);
        aes_cbc_mac_step(aes_state, prev_state, round_keys);
      }
    }
    offset += AES_BLOCK_SIZE;
  }

  return memcmp(signature, aes_state, AES_BLOCK_SIZE) == 0;

}

static bool is_device_id_packet(const comm_packet_t * packet){
    if(packet->length != 2){
        return false;
    }

    if (packet -> data[0] != BL_PACKET_DEVICE_ID_RES_DATA0){
        return false;
    }
    
    for (uint8_t i = 2; i < PACKET_DATA_LENGTH; i++){
        if(packet ->data[i] != 0xff){
            return false;
        }
    }
        return true;
}

static bool is_fw_length_packet(const comm_packet_t * packet){
    if(packet->length != 5){
        return false;
    }

    if (packet -> data[0] != BL_PACKET_FW_LENGTH_RES_DATA0){
        return false;
    }
    
    for (uint8_t i = 5; i < PACKET_DATA_LENGTH; i++){
        if(packet ->data[i] != 0xff){
            return false;
        }
    }
        return true;
}

static void update_fail(void){
  comm_create_single_byte_packet(&temp_packet, BL_PACKET_NACK_DATA0);
  comm_send_packet(&temp_packet);
  state = BL_STATE_Done;
}

static void check_for_timeout(void){
  if(simple_timer_has_elapsed(&timer)){
    update_fail();
  }
}

int main(void) {
  system_setup();
  gpio_setup();
  uart_setup();
  comm_setup();

  simple_timer_setup(&timer, DEFAULT_TIMEOUT, false);

  while (state != BL_STATE_Done){
    if(state == BL_STATE_Sync){
      if(uart_data_available()){
        sync_seq[0] = sync_seq[1];
        sync_seq[1] = sync_seq[2];
        sync_seq[2] = sync_seq[3];
        sync_seq[3] = uart_read_byte();

        bool is_match = sync_seq[0] == SYNC_SEQ_0;
        is_match = is_match && (sync_seq[1] == SYNC_SEQ_1);
        is_match = is_match && (sync_seq[2] == SYNC_SEQ_2);
        is_match = is_match && (sync_seq[3] == SYNC_SEQ_3);

        if(is_match){
          comm_create_single_byte_packet(&temp_packet, BL_PACKET_SYNC_OBSERVED_DATA0);
          comm_send_packet(&temp_packet);
          simple_timer_reset(&timer);
          state = BL_STATE_WaitForUpdateReq;
        }
        else{
          check_for_timeout();
        }
      } else {
        check_for_timeout();
      }
      continue;
    }
    comm_update();
    
    switch (state){
      case BL_STATE_WaitForUpdateReq:{
        if(packets_available()){
          comm_receive_packet(&temp_packet);
          if (is_single_byte_packet(&temp_packet, BL_PACKET_FW_UPDATE_REQ_DATA0)){
            simple_timer_reset(&timer);
            comm_create_single_byte_packet(&temp_packet, BL_PACKET_FW_UPDATE_RES_DATA0);
            comm_send_packet(&temp_packet);
            state = BL_STATE_DeviceIDReq;
          }
          else{
            update_fail();
          }
        }
        else{
          check_for_timeout();
        }
      }
      break;

      case BL_STATE_DeviceIDReq:{
        simple_timer_reset(&timer);
        comm_create_single_byte_packet(&temp_packet, BL_PACKET_DEVICE_ID_REQ_DATA0);
        comm_send_packet(&temp_packet);
        state = BL_STATE_DeviceIDRes;
      }
      break;

      case BL_STATE_DeviceIDRes:{
        if(packets_available()){
          comm_receive_packet(&temp_packet);

          if(is_device_id_packet(&temp_packet) && temp_packet.data[1] == DEVICE_ID){
            simple_timer_reset(&timer);
            state = BL_STATE_FWLengthReq;
          } else {
            update_fail();
          }
        } else {
          check_for_timeout();
        }
      }
      break;
      case BL_STATE_FWLengthReq:{
        simple_timer_reset(&timer);
        comm_create_single_byte_packet(&temp_packet, BL_PACKET_FW_LENGTH_REQ_DATA0);
        comm_send_packet(&temp_packet);
        state = BL_STATE_FWLengthRes;
      }
      break;
      case BL_STATE_FWLengthRes:{
        if(packets_available()){
          comm_receive_packet(&temp_packet);
          fw_length = (
            (temp_packet.data[1])       |
            (temp_packet.data[2] << 8)  |
            (temp_packet.data[3] << 16) |
            (temp_packet.data[4] << 24)
          );
          if(is_fw_length_packet(&temp_packet) && fw_length <= MAX_FW_LENGTH){
            state = BL_STATE_EraseApplication;
          } else {
            update_fail();
          }
        } else {
          check_for_timeout();
        }
      }
      break;
      case BL_STATE_EraseApplication:{
        bl_flash_erase_main_application();
        comm_create_single_byte_packet(&temp_packet, BL_PACKET_READY_FOR_DATA_DATA0);
        comm_send_packet(&temp_packet);
        simple_timer_reset(&timer);
        state = BL_STATE_ReceiveFirmware;
      }
      break;
      case BL_STATE_ReceiveFirmware:{
        if(packets_available()){
          comm_receive_packet(&temp_packet);
          const uint8_t packet_length = (temp_packet.length & 0x0f) + 1;
          bl_flash_write(MAIN_APP_START_ADDRESS + bytes_written, temp_packet.data, packet_length);
          bytes_written += packet_length;

          simple_timer_reset(&timer);
          
          if (bytes_written >= fw_length){
            comm_create_single_byte_packet(&temp_packet, BL_PACKET_FW_UPDATE_SUCCESSFUL_DATA0);
            comm_send_packet(&temp_packet);
            state = BL_STATE_Done;
          }
          else{
            comm_create_single_byte_packet(&temp_packet, BL_PACKET_READY_FOR_DATA_DATA0);
            comm_send_packet(&temp_packet);
          }
        }
        else{
          check_for_timeout();
        }
      }
      break;

      default:
      state = BL_STATE_Sync;
    }
  }
  
  /*PROGRAMMATIC FLASH CONTROL TESTING
  uint8_t data[1024] = {0};
  for (uint16_t i = 0; i < 1024; i++)
  {
    data[i] = i & 0xff;
  }

  bl_flash_erase_main_application();
  
  bl_flash_write(0x08008000, data, 1024);
  bl_flash_write(0x0800C000, data, 1024);
  bl_flash_write(0x08010000, data, 1024);
  bl_flash_write(0x08020000, data, 1024);
  bl_flash_write(0x08040000, data, 1024);
  bl_flash_write(0x08060000, data, 1024);
  
  */
  /*
  simple_timer_t timer2;
  simple_timer_setup(&timer, 1000,false);
  simple_timer_setup(&timer2, 1500 ,true);
  while (true){
    if(simple_timer_has_elapsed(&timer)){
      volatile int x = 0;
      x++;
    }
    if(simple_timer_has_elapsed(&timer2)){
      simple_timer_reset(&timer);
    }
  }
  */

  /*comm_packet_t packet = {
    .length = 9,
    .data = {1,2,3,4,5,6,7,8,9,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    .crc = 0
  };
  packet.crc = comm_compute_crc(&packet);

  comm_packet_t rx_packet;

  while (1)
  {
    comm_update();

    if(packets_available()){
      comm_receive_packet(&rx_packet);
    }
    comm_send_packet(&packet);
    system_delay(500);
  }
  */
  // TODO Teardown!
  system_delay(150);

  uart_teardown();
  gpio_teardown();
  system_teardown();

  if(validate_firmware_image()){
    jump_to_main();
  }
  else{
    scb_reset_core();
  }
  return 0;
}
