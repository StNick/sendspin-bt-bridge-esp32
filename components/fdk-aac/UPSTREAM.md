Fraunhofer FDK AAC Codec Library, from the fdk-aac standalone mirror of Android's
external/aac: https://github.com/mstorsjo/fdk-aac

Pinned release: v2.0.3 (716f4394641d53f0d79c9ddac3fa93b03a49f278)

Only the encoder's libraries are included: libAACenc, libFDK, libMpegTPEnc, libPCMutils,
libSACenc, libSBRenc and libSYS. Their files are unmodified copies of the pinned release.
The decoder libraries, build files and tools are omitted. NOTICE (the FDK licence) and
ChangeLog are retained. sources.txt lists the encoder sources from upstream's CMakeLists.txt.

The firmware opens the encoder with the AAC module only (no SBR or MPEG Surround) and
uses AAC-LC with a peak bitrate derived from the A2DP MTU.

The FDK licence grants no patent rights; see NOTICE section 3.
