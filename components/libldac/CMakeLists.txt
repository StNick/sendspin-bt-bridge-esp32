idf_component_register(SRCS "src/ldaclib.c" "src/ldacBT.c"
                       INCLUDE_DIRS "inc" PRIV_INCLUDE_DIRS "src")
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
# Sony's amalgamated C sources contain intentionally unused internal helpers.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
# Upstream's plain-char tables use -1 sentinels; RISC-V defaults to unsigned char.
target_compile_options(${COMPONENT_LIB} PRIVATE -fsigned-char)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-math-errno)
