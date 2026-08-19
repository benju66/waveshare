idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "render.c"
        "sim.c"
        "imu.c"
        "button.c"
        "page_manager.c"
        "touch_cst820.c"
        "ui_lvgl.c"
        "ui_lvgl_mem.c"
        "page_pomodoro.c"
        "page_now.c"
        "net_task.c"
        "battery.c"
        "ota.c"
        "settings.c"
        "page_settings.c"
    INCLUDE_DIRS "."
)

# The inner loops of the fluid solver are pure float math with no NaN/Inf
# semantics to preserve, so relaxed FP rules are safe and measurably faster.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)

# lvgl resolves its allocator (CONFIG_LV_USE_CUSTOM_MALLOC) against
# ui_lvgl_mem.c in this component, a reverse dependency the linker's
# left-to-right archive scan cannot see. Forcing one of the symbols
# undefined pulls the whole object in before liblvgl needs it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
