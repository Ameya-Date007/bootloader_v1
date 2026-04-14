From this point on, I'll be maintaining this readme file to note down the objectives that I want to complete till the end of the week.

Week 1 (April 13 - 17). Establish communication between Raspberry Pi and STM32 Nucleo board.
    --> UART driver for STM32 Nucleo board to be written, with ring buffer implemented.
    --> Writing a python script for Raspberry Pi to communicate with STM32 Nucleo, with all wiring done correctly, transfer single byte at a time, intially.
    --> Design a packet format which will be essential while transmitting the firmware file from Raspberry pi to STM32.

    Day 1, April 13
        --> Completion of UART driver and python script. []
        --> Testing single character transfer, one at a time. []

    Day 2, April 14
        --> Design a packet format for UART protocol, to transfer data in chunks. []





NOTES/CONCEPTS
1)  Why Ring Buffer? (Or Circular Buffer)
--> As compared to current case, where only a single byte is used to store the character, it might be possible that when the interrupt is given, it might be possible that the previous data
    might get lost. Assume, that you enter two characters very quickly and when the isr is called for UART, only 2nd character is read, due to a very small time difference in arrival of the
    data. Hence, the 1st char gets overwritten by the second, so to overcome this, we have to design a data structure which will take more characters, so that no data gets overwritten.
    Simply, it tackles the racing condition.
    *Google: They provide a low-overhead, FIFO (first-in-first-out) method to safely transfer data between high-speed asynchronous hardware interrupts (producer) and slower application code (consumer) without needing complex locks.

    Why Ring Buffers are Recommended for I/O Interrupts:

    Asynchronous Handling: The Interrupt Service Routine (ISR) can quickly deposit data and return, while the main loop processes it at its own pace.
    Lock-Free Safety: In a single-producer (ISR) and single-consumer (main task) scenario, a ring buffer can be implemented safely without disabling interrupts, provided the head/tail pointers are updated correctly.
    Prevent Overflows: They allow handling high-rate characters (e.g., UART at 115.2 KBaud) by buffering data until the processor can catch up.
    Memory Efficiency: They use a fixed-size, pre-allocated memory chunk, which is ideal for systems with limited RAM.
    *