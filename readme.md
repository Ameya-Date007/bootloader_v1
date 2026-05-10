From this point on, I'll be maintaining this readme file to note down the objectives that I want to complete till the end of the week.

Week 1 (April 13 - 17). Establish communication between Raspberry Pi and STM32 Nucleo board.
    --> UART driver for STM32 Nucleo board to be written, with ring buffer implemented.
    --> Writing a python script for Raspberry Pi to communicate with STM32 Nucleo, with all wiring done correctly, transfer single byte at a time, intially.
    --> Design a packet format which will be essential while transmitting the firmware file from Raspberry pi to STM32.

    Day 1, April 13 (Happened on April 17th)
        --> Completion of UART driver and python script. [DONE]
        --> Testing single character transfer, one at a time. [DONE]

    Day 2, April 14 (pending) (Done on April 18th)
        --> Design a packet format for UART protocol, to transfer data in chunks. [DONE]

    Day 3, April 21    
        --> Testing the packet state machine, using typescript. (See how to setup the typescript environment and copy the code written for testing).
        --> Begin with programmatic flash control.

    May 9--------
    Programmatic Flash Control complete, it requires unlocking flash memory and locking it after performing any read/write op. It uses Flash control register to start programming the flash memory, done after reading the busy status register for flash memory. The steps required for it is mentioned in section 3 of RM0368 documention (Reference Manual).

    Also completed a simple timer API abstraction, which will be required further. For this, the systick handler is used, which is already defined in system.c. Setup, reset functions were written for the timer.

    May 10
    Completed the firmware update mechanism!! The fw-updater is written in typescript.
    Learnings: How is handshake signal given? Through means of single packet.
               --> STM32 : acknowledges sync packet from client, sends requests and responses to the requests made by client.
               --> Raspberry Pi: Does the exact opposite of the target node.
                 * Timeout at every step is necessary, so whenever we are in a new state, we have to reset the timer after we have received any packet from Raspberry Pi.
                 * If the transmission fails, the timer will be elapsed past the max limit, and will jump to the main function.
                 * I by mistake, after erasing the main application data, didn't sent the READY_FOR_DATA packet, which caused timeout on my client side fw-updater. That is now resolved.

    
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

    ------------------------------------------ FOR KNOWLEDGE PURPOSE ------------------------------------------

    --> April 17th: Learn about the USART register, its bit fields.
    *