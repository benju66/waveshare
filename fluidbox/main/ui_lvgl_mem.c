// LVGL memory backend (CONFIG_LV_USE_CUSTOM_MALLOC).
//
// The fluid simulation's particle arrays deliberately fill most of the static
// internal SRAM (see the sizing notes in config.h), so LVGL's default 64 KB
// built-in pool does not fit: the link overflows dram0 by roughly that much.
// LVGL's own allocations — widget objects, style values, draw tasks — are
// small, latency-tolerant and never DMA'd, which makes them the ideal tenant
// for the otherwise idle 8 MB PSRAM. Internal RAM is the fallback, not the
// preference. The DMA'd draw buffers are not affected: LVGL renders into the
// two internal band buffers owned by display.c.

#include "esp_heap_caps.h"
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "Expected CONFIG_LV_USE_CUSTOM_MALLOC; check sdkconfig"
#endif

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    }
    return p;
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
    if (q == NULL) {
        q = heap_caps_realloc(p, new_size, MALLOC_CAP_DEFAULT);
    }
    return q;
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
