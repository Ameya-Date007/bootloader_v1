#include "common-defines.h"
#include <libopencm3/stm32/f4/memorymap.h>
#define BOOTLOADER_SIZE (0x8000U) // 32KB ----> 1000 0000 0000 0000
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE) // BASE ADDRESS OF FLASH MEM: 0x0800_0000

static void jump_to_main(void){
  typedef void (*void_fn)(void);
  uint32_t * reset_vector_entry = (uint32_t *) (MAIN_APP_START_ADDRESS + 4U);
  uint32_t * reset_vector = (uint32_t *) (*reset_vector_entry);
  void_fn jump_fn = (void_fn) reset_vector;

  jump_fn();
}

int main(void) {
  jump_to_main();
  return 0;
}
