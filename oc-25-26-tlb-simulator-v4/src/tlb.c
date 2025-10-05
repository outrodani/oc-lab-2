#include "tlb.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "constants.h"
#include "log.h"
#include "memory.h"
#include "page_table.h"

#include <inttypes.h>

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

void writeback_l1_to_l2(va_t vpn, pa_dram_t ppn);

void tlb_fill_entry(tlb_entry_t *entry, bool dirty, va_t vpn, pa_dram_t ppn) {
    entry->valid = true;
    entry->dirty = dirty;
    entry->last_access = get_time();
    entry->virtual_page_number = vpn;
    entry->physical_page_number = ppn;
}

int tlb_select_entry(tlb_entry_t *tlb, int size, bool is_l1) {
    int idx = -1;
    for (int i = 0; i < size; i++)
        if (!tlb[i].valid) return i; // posição livre
    
    // Escolher vítima na LRU
    time_ns_t oldest = (time_ns_t)(~(time_ns_t)0);
    for (int i = 0; i < size; i++)
        if (tlb[i].last_access < oldest) { oldest = tlb[i].last_access; idx = i; }

    // Se a vítima estiver dirty, write-back para DRAM
    if (tlb[idx].valid && tlb[idx].dirty) {
        if (is_l1) 
            writeback_l1_to_l2(tlb[idx].virtual_page_number, tlb[idx].physical_page_number);
        else {
            pa_dram_t base = tlb[idx].physical_page_number << PAGE_SIZE_BITS;
            write_back_tlb_entry(base);
        }
    }
    return idx;
}

void writeback_l1_to_l2(va_t vpn, pa_dram_t ppn)
{
    // Procura a VPN já na L2
    int hit = -1;
    for (int i = 0; i < (int)TLB_L2_SIZE; i++) {
        if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == vpn) {
            hit = i; break;
        }
    }

    if (hit >= 0) {
        // Já existe na L2: marca dirty e atualiza LRU
        tlb_l2[hit].dirty = true;
        tlb_l2[hit].last_access = get_time();
        return;
    }

    // Não existe: escolher índice na L2 (inválido -> LRU)
    int idx = tlb_select_entry(tlb_l2, TLB_L2_SIZE, false);

    // Escrever/atualizar a linha na L2 (fica dirty porque vem de write-back)
    tlb_fill_entry(&tlb_l2[idx], true, vpn, ppn);
}

void tlb_invalidate(va_t virtual_page_number) {
  //L1
  increment_time(TLB_L1_LATENCY_NS);
  for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
    if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == virtual_page_number) {

      tlb_l1[i].valid = false;      

      tlb_l1_invalidations++;    
      return;
    }
  }

  //L2
  increment_time(TLB_L2_LATENCY_NS);
  for (int i = 0; i < (int)TLB_L2_SIZE; i++) {
      if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == virtual_page_number) {

        tlb_l2[i].valid = false;

        tlb_l2_invalidations++;
      }
  }
}

pa_dram_t tlb_translate(va_t virtual_address, op_t op) {
  // Custos fixos da L1 + higienização do virtual address
  increment_time(TLB_L1_LATENCY_NS); // toda tradução "toca" na L1 (hit ou miss)
  virtual_address &= VIRTUAL_ADDRESS_MASK; // garante virtual address dentro do espaço válido

  // Decompor o virtual address: (VPN, offset)
  va_t vpn = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;
  va_t off = virtual_address & PAGE_OFFSET_MASK;

  // PROCURA EM L1 (fully associative)
  int l1_hit = -1;
  for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
    if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == vpn) {
      l1_hit = i; break;
    }
  }

  // HIT L1: atualizar métricas/estado e devolver physical address
  if (l1_hit >= 0) {
    tlb_l1_hits++; // estatística
    tlb_l1[l1_hit].last_access = get_time(); // LRU: "usei agora"
    if (op == OP_WRITE) tlb_l1[l1_hit].dirty = true; // write-hit torna a linha suja
    pa_dram_t ppn = tlb_l1[l1_hit].physical_page_number;
    return (ppn << PAGE_SIZE_BITS) | off;   // PA = base da página + offset
  }

  // MISS L1: vamos consultar a L2
  tlb_l1_misses++;

  // PROCURA EM L2 (só após miss em L1)
  increment_time(TLB_L2_LATENCY_NS); // custo de consultar a L2

  int l2_hit = -1;
  for (int i = 0; i < (int)TLB_L2_SIZE; i++) {
    if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == vpn) {
      l2_hit = i; break;
    }
  }

  // HIT L2: promover para L1 e devolver
  if (l2_hit >= 0) {
    tlb_l2_hits++; // estatística
    tlb_l2[l2_hit].last_access = get_time(); // LRU na L2
    tlb_l2[l2_hit].dirty = false;// mantemos a L2 SEMPRE limpa (evita WBs a mais)

    // Dados que vamos promover
    pa_dram_t ppn = tlb_l2[l2_hit].physical_page_number;
    pa_dram_t pa  = (ppn << PAGE_SIZE_BITS) | off;

    // escolher um slot na L1 (inválido ou LRU)
    int idx = tlb_select_entry(tlb_l1, TLB_L1_SIZE, true);

    // preencher a linha na L1 com a tradução vinda da L2
    tlb_fill_entry(&tlb_l1[idx], (op == OP_WRITE), vpn, ppn);

    return pa; // não há page table aqui
  }

  // MISS L2: caminho lento (page table)
  tlb_l2_misses++;

 // PAGE TABLE: traduzir virtual address para physical address (pode gerar page fault/DRAM/disk)
  pa_dram_t pa_res  = page_table_translate(virtual_address, op);
  pa_dram_t ppn_res = (pa_res >> PAGE_SIZE_BITS);

  // INSERIR NA L2 (para reuso futuro) — manter L2 limpa
  int l2_idx = tlb_select_entry(tlb_l2, TLB_L2_SIZE, false);

  tlb_fill_entry(&tlb_l2[l2_idx], false, vpn, ppn_res); // L2 mantém-se limpa

  // INSERIR/PROMOVER TAMBÉM NA L1 (é o nível “quente”)
  int l1_idx = tlb_select_entry(tlb_l1, TLB_L1_SIZE, true);

  tlb_fill_entry(&tlb_l1[l1_idx], (op == OP_WRITE), vpn, ppn_res);

  // Devolver o PA calculado pela page table  
  return pa_res;
}
