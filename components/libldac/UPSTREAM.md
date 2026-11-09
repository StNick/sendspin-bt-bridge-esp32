Sony LDAC encoder from the Android Open Source Project:
https://android.googlesource.com/platform/external/libldac/

Pinned revision: eeee1a3f5f8df1282e3a6d297085885fd886737b

LICENSE and NOTICE are retained. The sole source change is four sqrt() calls in
src/sigana_ldac.c changed to sqrtf(): their input/output SCALAR values are float,
so the ESP32-S31 can use its hardware FPU instead of software double arithmetic.
All other inc/ and src/ files match the pinned upstream revision.
The build uses the floating point encoder on the ESP32-S31 and -fsigned-char,
which preserves upstream's negative sentinel values in plain-char tables.
