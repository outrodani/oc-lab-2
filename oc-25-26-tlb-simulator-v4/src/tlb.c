#include "tlb.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "constants.h"
#include "log.h"
#include "memory.h"
#include "page_table.h"

typedef struct {
  bool valid;
  bool dirty;
  uint64_t last_access;
  va_t virtual_page_number;
  pa_dram_t physical_page_number;
} tlb_entry_t;

tlb_entry_t tlb_l1[TLB_L1_SIZE];
tlb_entry_t tlb_l2[TLB_L2_SIZE];

uint64_t tlb_l1_hits = 0;
uint64_t tlb_l1_misses = 0;
uint64_t tlb_l1_invalidations = 0;

uint64_t tlb_l2_hits = 0;
uint64_t tlb_l2_misses = 0;
uint64_t tlb_l2_invalidations = 0;

uint64_t get_total_tlb_l1_hits() { return tlb_l1_hits; }
uint64_t get_total_tlb_l1_misses() { return tlb_l1_misses; }
uint64_t get_total_tlb_l1_invalidations() { return tlb_l1_invalidations; }

uint64_t get_total_tlb_l2_hits() { return tlb_l2_hits; }
uint64_t get_total_tlb_l2_misses() { return tlb_l2_misses; }
uint64_t get_total_tlb_l2_invalidations() { return tlb_l2_invalidations; }

void tlb_init() {
  memset(tlb_l1, 0, sizeof(tlb_l1));
  memset(tlb_l2, 0, sizeof(tlb_l2));
  tlb_l1_hits = 0;
  tlb_l1_misses = 0;
  tlb_l1_invalidations = 0;
  tlb_l2_hits = 0;
  tlb_l2_misses = 0;
  tlb_l2_invalidations = 0;
}

void tlb_invalidate(va_t virtual_page_number) {
  (void)(virtual_page_number);  // Suppress unused variable warning. You can
                                // delete this when implementing the actual
                                // function.
  //L1
  increment_time(TLB_L1_LATENCY_NS);
  for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
    if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == virtual_page_number) {
      // Se a linha tem alterações ainda não guardadas, faz write-back
      if (tlb_l1[i].dirty) {
        pa_dram_t base_pa = (tlb_l1[i].physical_page_number << PAGE_SIZE_BITS);
        write_back_tlb_entry(base_pa);  //write-back
      }

      tlb_l1[i].valid = false;
      tlb_l1[i].dirty = false;     
      tlb_l1[i].last_access = 0;   

      tlb_l1_invalidations++;      
    }
  }
  // TODO: implement TLB entry invalidation.
}

pa_dram_t tlb_translate(va_t virtual_address, op_t op) {
  increment_time(TLB_L1_LATENCY_NS);
  virtual_address &= VIRTUAL_ADDRESS_MASK;

  va_t vpn = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;
  va_t off = virtual_address & PAGE_OFFSET_MASK;

  //Procurar hit na L1
  int hit = -1;
  for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
    if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == vpn) { hit = i; break; }
  }

  //Tratar o hit
  if (hit >= 0) {
    tlb_l1_hits++;
    tlb_l1[hit].last_access = get_time();           // LRU
    if (op == OP_WRITE) tlb_l1[hit].dirty = true;   // write hit
    pa_dram_t ppn = tlb_l1[hit].physical_page_number;
    return (ppn << PAGE_SIZE_BITS) | off;           // PA = base + offset
  }

  tlb_l1_misses++;
  pa_dram_t pa = page_table_translate(virtual_address, op);

  int idx = -1;
  for (int i = 0; i < (int)TLB_L1_SIZE; i++) if (!tlb_l1[i].valid) { idx = i; break; }
  if (idx < 0) {
    time_ns_t oldest = (time_ns_t)(~(time_ns_t)0);
    for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
      if (tlb_l1[i].last_access < oldest) { oldest = tlb_l1[i].last_access; idx = i; }
    }
  }

  if (tlb_l1[idx].valid && tlb_l1[idx].dirty) {
    pa_dram_t base = tlb_l1[idx].physical_page_number << PAGE_SIZE_BITS;
    write_back_tlb_entry(base);
  }

  tlb_l1[idx].valid = true;
  tlb_l1[idx].dirty = (op == OP_WRITE);
  tlb_l1[idx].last_access = get_time();
  tlb_l1[idx].virtual_page_number = vpn;
  tlb_l1[idx].physical_page_number = (pa >> PAGE_SIZE_BITS);
  return pa;

  // TODO: implement the TLB logic.
  //return page_table_translate(virtual_address, op);
}
