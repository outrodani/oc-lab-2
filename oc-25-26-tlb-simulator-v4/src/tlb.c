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

void writeback_l1_to_l2(va_t vpn, pa_dram_t ppn)
{
    // 1) Procura a VPN já na L2
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
        // (mantém ppn; se quiseres, poderias validar que coincide)
        return;
    }

    // 2) Não existe: escolher índice na L2 (inválido -> LRU)
    int idx = -1;
    for (int i = 0; i < (int)TLB_L2_SIZE; i++) {
        if (!tlb_l2[i].valid) { idx = i; break; }
    }
    if (idx < 0) {
        // LRU na L2
        time_ns_t oldest = (time_ns_t)(~(time_ns_t)0);
        for (int i = 0; i < (int)TLB_L2_SIZE; i++) {
            if (tlb_l2[i].last_access < oldest) {
                oldest = tlb_l2[i].last_access;
                idx = i;
            }
        }
        // Se a vítima estiver dirty, write-back para DRAM
        if (tlb_l2[idx].valid && tlb_l2[idx].dirty) {
            pa_dram_t base = (tlb_l2[idx].physical_page_number << PAGE_SIZE_BITS);
            write_back_tlb_entry(base);
        }
    }

    // 3) Escrever/atualizar a linha na L2 (fica dirty porque vem de write-back)
    tlb_l2[idx].valid = true;
    tlb_l2[idx].dirty = true;                    
    tlb_l2[idx].last_access = get_time();
    tlb_l2[idx].virtual_page_number = vpn;
    tlb_l2[idx].physical_page_number = ppn;
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
  // =========================
  // 1) Custos fixos da L1 + higienização do VA
  // =========================
  increment_time(TLB_L1_LATENCY_NS);      // toda tradução "toca" na L1 (hit ou miss)
  virtual_address &= VIRTUAL_ADDRESS_MASK; // garante VA dentro do espaço válido

  // =========================
  // 2) Decompor a VA: (VPN, offset)
  // =========================
  va_t vpn = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;
  va_t off = virtual_address & PAGE_OFFSET_MASK;

  // =========================
  // 3) PROCURA EM L1 (fully associativa)
  // =========================
  int l1_hit = -1;
  for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
    if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == vpn) {
      l1_hit = i; break;
    }
  }

  // --- 3a) HIT L1: atualizar métricas/estado e devolver PA ---
  if (l1_hit >= 0) {
    tlb_l1_hits++;                          // estatística
    tlb_l1[l1_hit].last_access = get_time(); // LRU: "usei agora"
    if (op == OP_WRITE) tlb_l1[l1_hit].dirty = true; // write-hit torna a linha suja
    pa_dram_t ppn = tlb_l1[l1_hit].physical_page_number;
    return (ppn << PAGE_SIZE_BITS) | off;   // PA = base da página + offset
  }

  // --- 3b) MISS L1: vamos consultar a L2 ---
  tlb_l1_misses++;

  // =========================
  // 4) PROCURA EM L2 (só após miss em L1)
  // =========================
  increment_time(TLB_L2_LATENCY_NS);        // custo de consultar a L2

  int l2_hit = -1;
  for (int i = 0; i < (int)TLB_L2_SIZE; i++) {
    if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == vpn) {
      l2_hit = i; break;
    }
  }

  // --- 4a) HIT L2: promover para L1 e devolver ---
  if (l2_hit >= 0) {
    tlb_l2_hits++;                           // estatística
    tlb_l2[l2_hit].last_access = get_time(); // LRU na L2
    tlb_l2[l2_hit].dirty = false;            // mantemos a L2 SEMPRE limpa (evita WBs a mais)

    // Dados que vamos promover
    pa_dram_t ppn = tlb_l2[l2_hit].physical_page_number;
    pa_dram_t pa  = (ppn << PAGE_SIZE_BITS) | off;

    // --- escolher um slot na L1 (inválido ou LRU) ---
    int idx = -1;
    for (int i = 0; i < (int)TLB_L1_SIZE; i++)
      if (!tlb_l1[i].valid) { idx = i; break; }
    if (idx < 0) {
      // não há vaga: escolhe vítima LRU
      time_ns_t oldest = (time_ns_t)(~(time_ns_t)0);
      for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
        if (tlb_l1[i].last_access < oldest) { oldest = tlb_l1[i].last_access; idx = i; }
      }
      // write-back só se a vítima estiver suja
      if (tlb_l1[idx].valid && tlb_l1[idx].dirty) {
        writeback_l1_to_l2(tlb_l1[idx].virtual_page_number , tlb_l1[idx].physical_page_number);
      }
    }

    // --- preencher a linha na L1 com a tradução vinda da L2 ---
    tlb_l1[idx].valid = true;
    tlb_l1[idx].dirty = (op == OP_WRITE);   // só fica dirty se esta operação for WRITE
    tlb_l1[idx].last_access = get_time();
    tlb_l1[idx].virtual_page_number = vpn;
    tlb_l1[idx].physical_page_number = ppn;

    return pa;                               // não há page table aqui
  }

  // --- 4b) MISS L2: caminho lento (page table) ---
  tlb_l2_misses++;

  // =========================
  // 5) PAGE TABLE: traduzir VA→PA (pode gerar page fault/DRAM/disk)
  // =========================
  pa_dram_t pa_res  = page_table_translate(virtual_address, op);
  pa_dram_t ppn_res = (pa_res >> PAGE_SIZE_BITS);

  // =========================
  // 6) INSERIR NA L2 (para reuso futuro) — manter L2 limpa
  // =========================
  int l2_idx = -1;
  for (int i = 0; i < (int)TLB_L2_SIZE; i++)
    if (!tlb_l2[i].valid) { l2_idx = i; break; }
  if (l2_idx < 0) {
    // escolher vítima LRU na L2
    time_ns_t oldest2 = (time_ns_t)(~(time_ns_t)0);
    for (int i = 0; i < (int)TLB_L2_SIZE; i++) {
      if (tlb_l2[i].last_access < oldest2) { oldest2 = tlb_l2[i].last_access; l2_idx = i; }
    }
    // write-back na L2 só se, por algum motivo, a vítima estiver suja
    if (tlb_l2[l2_idx].valid && tlb_l2[l2_idx].dirty) {
      pa_dram_t base2 = tlb_l2[l2_idx].physical_page_number << PAGE_SIZE_BITS;
      write_back_tlb_entry(base2);
    }
  }
  tlb_l2[l2_idx].valid = true;
  tlb_l2[l2_idx].dirty = false;             // regra de ouro: L2 fica sempre limpa
  tlb_l2[l2_idx].last_access = get_time();
  tlb_l2[l2_idx].virtual_page_number = vpn;
  tlb_l2[l2_idx].physical_page_number = ppn_res;

  // =========================
  // 7) INSERIR/PROMOVER TAMBÉM NA L1 (é o nível “quente”)
  // =========================
  int l1_idx = -1;
  for (int i = 0; i < (int)TLB_L1_SIZE; i++)
    if (!tlb_l1[i].valid) { l1_idx = i; break; }
  if (l1_idx < 0) {
    // vítima LRU na L1
    time_ns_t oldest = (time_ns_t)(~(time_ns_t)0);
    for (int i = 0; i < (int)TLB_L1_SIZE; i++) {
      if (tlb_l1[i].last_access < oldest) { oldest = tlb_l1[i].last_access; l1_idx = i; }
    }
    // write-back se a vítima da L1 estiver suja
    if (tlb_l1[l1_idx].valid && tlb_l1[l1_idx].dirty) {
      writeback_l1_to_l2(tlb_l1[l1_idx].virtual_page_number , tlb_l1[l1_idx].physical_page_number);
    }
  }
  tlb_l1[l1_idx].valid = true;
  tlb_l1[l1_idx].dirty = (op == OP_WRITE);  // READ → false; WRITE → true
  tlb_l1[l1_idx].last_access = get_time();
  tlb_l1[l1_idx].virtual_page_number = vpn;
  tlb_l1[l1_idx].physical_page_number = ppn_res;

  // =========================
  // 8) Devolver o PA calculado pela page table
  // =========================
  return pa_res;
}
