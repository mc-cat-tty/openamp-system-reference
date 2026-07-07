/*
 * Sample application to benchmark latency, jitter and WCET of execution on the application domain
 * (Linux on Cortex-A53 cores) vs the real-time domain (baremetal Cortex-R5 cores).
 * This header contains the common bench function, shared between the two domains,
 * and the common START and SHUTDOWN message representations.
 */

#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdint.h>
#include <stdlib.h>

#define RPMSG_SERVICE_NAME "rpmsg-openamp-bench"

#define STARTBENCH_MSG	0xEF56A55A
#define SHUTDOWN_MSG	0xEF56A55B

#define SAMPLES_NUMBER 10000
typedef uint32_t sample_ele_type;

// sizeof(vector_ele_type) * VECTOR_LENGTH must be <= 64Kb, the size of TCM-B
#define VECTOR_LENGTH 256
typedef uint8_t vector_ele_type;

#if defined(__ARM_ARCH_7R__) || defined(ARMR5)
static inline void pmu_enable(void){
  uint32_t v;
  asm volatile("mrc p15,0,%0,c9,c12,0":"=r"(v));
  v |= (1u<<0)|(1u<<2);
  v &= ~(1u<<3);          // E | reset CCNT | D=0 (1:1, not /64)
  asm volatile("mcr p15,0,%0,c9,c12,0"::"r"(v));
  asm volatile("mcr p15,0,%0,c9,c12,1"::"r"(0x80000000u)); // enable CCNT
}

static inline uint32_t ccnt(void){ uint32_t c; asm volatile("mrc p15,0,%0,c9,c13,0":"=r"(c)); return c; }

#define TCM_DATA __attribute__((section(".tcm_bench")))
#define TCM_TEXT __attribute__((noinline, section(".tcm_text")))
#else
#define TCM_DATA
#define TCM_TEXT
#endif  /* defined(__ARM_ARCH_7R__) || defined(ARMR5) */

static TCM_DATA vector_ele_type bench_vector[VECTOR_LENGTH];

static TCM_TEXT uint32_t bench_fun(void) {
  uint32_t acc = 0;
  for (int i=0; i<64; i++) 
    for (int j=0; j<VECTOR_LENGTH; j++)
      acc += bench_vector[j];
  return acc;
}

#endif /* BENCH_COMMON_H */