# The Amiga (platform independent; host build in host/)
idf_component_register(SRCS "amiga.c"
                            "mem.c"
                            "cia.c"
                            "custom.c"
                            "copper.c"
                            "blitter.c"
                            "display.c"
                            "floppy.c"
                            "rtc.c"
                            "input.c"
                            "audio.c"
                            "rp9.c"
                            "autoconf.c"
                            "gayle.c"
                            "hdf.c"
                            "hdboard.c"
                            "space.c"
                            "wait.c"
                            "jit.c"
                            "jit_rv.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES musashi
                       PRIV_REQUIRES emudbg esp_mm heap)

# Runs together with the CPU core in the emulation loop
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
# The 68EC020's translated blocks (jit.c, jit_rv.c, TODO 19): RISC-V code in
# PSRAM (executable with CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION off);
# the host build runs the same code through host/rvsim.c (AMI_NOJIT there:
# the interpreter alone)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AMI_JIT)
