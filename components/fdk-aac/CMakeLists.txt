# Encoder-only subset of Fraunhofer FDK AAC (see UPSTREAM.md). sources.txt is shared with
# tests/run_bt_encoder_host.py so the host tests compile exactly the firmware's file set.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/sources.txt" FDK_SOURCES)
idf_component_register(SRCS ${FDK_SOURCES}
                       INCLUDE_DIRS "libAACenc/include" "libSYS/include"
                       PRIV_INCLUDE_DIRS "libFDK/include" "libMpegTPEnc/include" "libPCMutils/include"
                                         "libSACenc/include" "libSBRenc/include")
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
# Upstream fixed-point code is built as published; its warnings are not ours to fix.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -O2)
