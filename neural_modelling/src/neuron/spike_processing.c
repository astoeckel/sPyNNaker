#include "spike_processing.h"
#include "population_table.h"
#include "synapse_row.h"
#include "synapses.h"
#include "../common/in_ap.h"
#include "../common/in_gp.h"
#include <spin1_api.h>
#include <debug.h>

// The number of DMA Buffers to use
#define N_DMA_BUFFERS 2

// The number of spaces in the incoming spike buffer
#define N_INCOMING_APS 256
#define N_INCOMING_GPS 128

// DMA tags
#define DMA_TAG_READ_SYNAPTIC_ROW 0
#define DMA_TAG_WRITE_PLASTIC_REGION 1

// DMA buffer structure combines the row read from SDRAM with 
typedef struct dma_buffer {
  // Does this DMA originate from a gradient potential or an action potential
  bool originating_gp_not_ap;
  
  // Address in SDRAM to write back plastic region to
  address_t sdram_writeback_address;
  
  // Key of originating action/gradient potential 
  // (used to allow row data to be re-used for multiple spikes)
  key_t originating_key;
  
  // Payload of originating gradient potential payload
  payload_t gp_payload;
  
  // Row data
  uint32_t *row;
} dma_buffer;

extern uint32_t time;

// True if the DMA "loop" is currently running
static bool dma_busy;

// The DTCM buffers for the synapse rows
static dma_buffer dma_buffers[N_DMA_BUFFERS];

// The index of the next buffer to be filled by a DMA
static uint32_t next_buffer_to_fill;

// The index of the buffer currently being filled by a DMA read
static uint32_t buffer_being_read;

/* PRIVATE FUNCTIONS - static for inlining */
static inline bool _attempt_synaptic_dma_read(key_t key, payload_t *payload) {
    // Decode spike to get address of destination synaptic row
    address_t row_address;
    size_t n_bytes_to_transfer;
    if (population_table_get_address(key, &row_address,
            &n_bytes_to_transfer)) {

        // Write the SDRAM address of the plastic region and the
        // Key of the originating spike to the beginning of dma buffer
        dma_buffer *next_buffer = &dma_buffers[next_buffer_to_fill];
        next_buffer->sdram_writeback_address = row_address + 1;
        next_buffer->originating_key = key;
    
        // If a payload is specified, copy it into next  
        // Buffer and set gradient potential flag
        if(payload != NULL) {
            next_buffer->originating_gp_not_ap = true;
            next_buffer->gp_payload = *payload;
        }
        // Otherwise, clear gradient potential flag
        else {
            next_buffer->originating_gp_not_ap = false;
        }
        
        // Start a DMA transfer to fetch this synaptic row into current buffer
        buffer_being_read = next_buffer_to_fill;
        spin1_dma_transfer(DMA_TAG_READ_SYNAPTIC_ROW, row_address,
                            next_buffer->row,
                            DMA_READ, n_bytes_to_transfer);
        next_buffer_to_fill = (next_buffer_to_fill + 1) % N_DMA_BUFFERS;

        return true;
    }
    else
    {
        return false;
    }
}
static inline void _setup_synaptic_dma_read() {
    
    while(true)
    {
        // If there's an action potential in the queue
        ap_t ap;
        gp_t gp;
        if(in_ap_get_next(&ap))
        {
          log_debug("Checking for row for action potential 0x%.8x\n", ap);
          if(_attempt_synaptic_dma_read(ap, NULL))
          {
            return;
          }
        }
        // Otherwise, if there's a gradient potential in the queue
        else if(in_gp_get_next(&gp))
        {
          log_debug("Checking for row for gradient potential key 0x%.8x\n", gp_key(gp));
          payload_t payload = gp_payload(gp);
          if(_attempt_synaptic_dma_read(gp_key(gp), &payload))
          {
            return;
          }
        }
        // Otherwise,
        else
        {
          break;
        }
    }
    
    // No more spikes - stop trying to set up synaptic dmas 
    log_debug("DMA not busy");
    dma_busy = false;
}

static inline void _setup_synaptic_dma_write(uint32_t dma_buffer_index) {

    // Get pointer to current buffer
    dma_buffer *buffer = &dma_buffers[dma_buffer_index];
    
    // Get the number of plastic bytes and the writeback address from the
    // synaptic row
    size_t n_plastic_region_bytes = synapse_row_plastic_size(buffer->row) * sizeof(uint32_t);
    
    log_debug("Writing back %u bytes of plastic region to %08x",
              n_plastic_region_bytes, buffer->sdram_writeback_address);

    // Start transfer
    spin1_dma_transfer(
        DMA_TAG_WRITE_PLASTIC_REGION, buffer->sdram_writeback_address,
        synapse_row_plastic_region(buffer->row),
        DMA_WRITE, n_plastic_region_bytes);
}


/* CALLBACK FUNCTIONS - cannot be static */

// Called when a multicast packet is received
void _multicast_packet_received_callback(uint key, uint payload) {
    use(payload);

    log_debug("Received action potential %x at %d, DMA Busy = %d", key, time, dma_busy);

    // If there was space to add action potential to incoming queue
    if (in_ap_add(key)) {

        // If we're not already processing synaptic dmas,
        // flag pipeline as busy and trigger a feed event
        if (!dma_busy) {

            log_debug("Sending user event for new action potential");
            if (spin1_trigger_user_event(0, 0)) {
                dma_busy = true;
            } else {
                log_debug("Could not trigger user event\n");
            }
        }
    } else {
        log_debug("Could not add action potential");
    }
}

void _multicast_payload_packet_received_callback(uint key, uint payload) {
  log_debug("Received gradient potential %x:%x at %d, DMA Busy = %d", 
            key, payload, time, dma_busy);

    // If there was space to add gradient potential to incoming queue
    if (in_gp_add(gp_create(key, payload))) {

        // If we're not already processing synaptic dmas,
        // flag pipeline as busy and trigger a feed event
        if (!dma_busy) {

            log_debug("Sending user event for new action potential");
            if (spin1_trigger_user_event(0, 0)) {
                dma_busy = true;
            } else {
                log_debug("Could not trigger user event\n");
            }
        }
    } else {
        log_debug("Could not add action potential");
    }
}

// Called when a user event is received
void _user_event_callback(uint unused0, uint unused1) {
    use(unused0);
    use(unused1);
    _setup_synaptic_dma_read();
}

// Called when a DMA completes
void _dma_complete_callback(uint unused, uint tag) {
    use(unused);

    log_debug("DMA transfer complete with tag %u", tag);

    // If this DMA is the result of a read
    if (tag == DMA_TAG_READ_SYNAPTIC_ROW) {
        // Get pointer to current buffer
        uint32_t current_buffer_index = buffer_being_read;
        dma_buffer *current_buffer = &dma_buffers[current_buffer_index];
        
        // Start the next DMA transfer, so it is complete when we are finished
        _setup_synaptic_dma_read();
        
        // Process synaptic row repeatedly
        bool subsequent_spikes;
        do {
            // If row fetch was started by a gradient potential, check if next 
            // Gradient potential originates from the same source neuron
            if(current_buffer->originating_gp_not_ap)
            {
                subsequent_spikes = in_gp_is_next_key_equal(current_buffer->originating_key);
            }
            // Otherwise, check if next action potential originates from the same source neuron
            else
            {
                subsequent_spikes = in_ap_is_next_key_equal(current_buffer->originating_key);
            }
            
            // Process synaptic row, writing it back if it's the last time
            // it's going to be processed
            synapses_process_synaptic_row(time, current_buffer->row,
                                          !subsequent_spikes,
                                          current_buffer_index);
        } while (subsequent_spikes);

    } else if (tag == DMA_TAG_WRITE_PLASTIC_REGION) {

        // Do Nothing

    } else {

        // Otherwise, if it ISN'T the result of a plastic region write
        log_error("Invalid tag %d received in DMA", tag);
    }
}


/* INTERFACE FUNCTIONS - cannot be static */

bool spike_processing_initialise(size_t row_max_n_words) {

    // Allocate the DMA buffers
    for (uint32_t i = 0; i < N_DMA_BUFFERS; i++) {
        dma_buffers[i].row = (uint32_t*) spin1_malloc(
                row_max_n_words * sizeof(uint32_t));
        if (dma_buffers[i].row == NULL) {
            log_error("Could not initialise DMA buffers");
            return false;
        }
    }
    dma_busy = false;
    next_buffer_to_fill = 0;
    buffer_being_read = N_DMA_BUFFERS;

    // Allocate incoming action and graded potential buffers
    if (!in_ap_initialize_buffer(N_INCOMING_APS)) {
        return false;
    }
    
    if (!in_gp_initialize_buffer(N_INCOMING_GPS)) {
        return false;
    }

    // Set up the callbacks
    spin1_callback_on(MC_PACKET_RECEIVED,
            _multicast_packet_received_callback, -1);
    spin1_callback_on(MCPL_PACKET_RECEIVED,
            _multicast_payload_packet_received_callback, -1);
    spin1_callback_on(DMA_TRANSFER_DONE, _dma_complete_callback, 0);
    spin1_callback_on(USER_EVENT, _user_event_callback, 0);

    return true;
}

void spike_processing_finish_write(uint32_t process_id) {
    _setup_synaptic_dma_write(process_id);
}
