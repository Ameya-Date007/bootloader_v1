#include "core/ring_buffer.h"

void ring_buffer_setup(ring_buffer_t * rb, uint8_t * buffer, uint32_t size){
    rb -> buffer = buffer;
    rb -> mask = size -1;
    rb -> read_idx = 0;
    rb -> write_idx = 0;
}

bool ring_buffer_empty(ring_buffer_t * rb){
    return rb -> read_idx == rb -> write_idx;
}
bool ring_buffer_write(ring_buffer_t * rb, uint8_t byte){
    uint32_t local_write_idx = rb -> write_idx;
    uint32_t local_read_idx = rb -> read_idx;

    uint32_t next_write_idx = (local_write_idx + 1) & rb -> mask;

    if(next_write_idx == local_read_idx){
        return false;
    }
    rb-> buffer[local_write_idx] = byte;
    rb -> write_idx = next_write_idx;
    return true;
}
bool ring_buffer_read(ring_buffer_t * rb, uint8_t * byte){
    uint32_t local_read_idx = rb -> read_idx;
    uint32_t local_write_idx = rb -> write_idx;
    if(local_read_idx == local_write_idx){
        return false;
    }
    *byte = rb -> buffer[local_read_idx];
    // Move the read pointer to next location
    local_read_idx = (local_read_idx + 1) & rb -> mask; // Wrapping around to the start.
    rb -> read_idx = local_read_idx;
    return true;
}