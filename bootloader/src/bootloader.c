#include "common-defines.h"
#include <libopencm3/stm32/f4/memorymap.h>
#include <libopencm3/cm3/vector.h>

#define BOOTLOADER_SIZE (0x8000U) // 32KB ----> 1000 0000 0000 0000
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE) // BASE ADDRESS OF FLASH MEM: 0x0800_0000

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

int main(void) {
  jump_to_main();
  return 0;
}
