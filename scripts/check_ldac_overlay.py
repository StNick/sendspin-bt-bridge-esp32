"""Invalidate PlatformIO's CMake cache when any LDAC overlay input changes."""
from hashlib import sha256
from pathlib import Path

Import("env")

root = Path(env.subst("$PROJECT_DIR")).resolve()
build = Path(env.subst("$BUILD_DIR")).resolve()
if not build.is_relative_to(root / '.pio' / 'build'):
    raise RuntimeError(f"Refusing to invalidate CMake cache outside .pio/build: {build}")
inputs = ('cmake/ldac_transport.cmake', 'scripts/prepare_ldac_transport.py',
          'scripts/repo_ldac_transport.h', 'scripts/check_ldac_overlay.py')
fingerprint = sha256(b''.join((root / name).read_bytes() for name in inputs)).hexdigest()
stamp = build / '.ldac-overlay-sha256'
if not stamp.exists() or stamp.read_text() != fingerprint:
    # PlatformIO only watches top-level CMakeLists files, not their includes.
    # Removing this generated cache forces it to reconfigure the modified ABI.
    (build / 'CMakeCache.txt').unlink(missing_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    stamp.write_text(fingerprint)
