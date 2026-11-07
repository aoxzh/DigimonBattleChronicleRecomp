/* HAL selftest-only state shared with runtime glue and IRQ polling. */
#include <stdint.h>

/* The generated blocks own this definition in the full headless link. */
uint32_t gc_spr_misc[1024];
