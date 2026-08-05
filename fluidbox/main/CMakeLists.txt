idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "render.c"
        "sim.c"
        "imu.c"
        "button.c"
    INCLUDE_DIRS "."
)

# The inner loops of the fluid solver are pure float math with no NaN/Inf
# semantics to preserve, so relaxed FP rules are safe and measurably faster.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)
