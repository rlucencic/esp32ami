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
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES musashi
                       PRIV_REQUIRES emudbg)

# Runs together with the CPU core in the emulation loop
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
# The 68EC020's translated blocks (jit.c, TODO 19): off on the board until
# they run faster than the interpreter (stage 1, every instruction a handler
# call through mem.c's jit_exec_block(): Virtual Karting 2 81% -> 67%); the
# host build has them on (AMI_NOJIT turns them off there)
# target_compile_definitions(${COMPONENT_LIB} PRIVATE AMI_JIT)
