"""
Build script for zpaq.

We use pybind11's Pybind11Extension which sets sensible defaults
(C++14, suppress noisy warnings, hidden symbol visibility on *nix).

The vendored libzpaq sources live under src/zpaq/vendor/. We define
NOJIT so the resulting binary contains no x86-only JIT path - this lets a
single source build cleanly across x86_64, aarch64 and Apple Silicon.
"""
import sys
from setuptools import setup
from pybind11.setup_helpers import Pybind11Extension, build_ext


SRC = "src/zpaq"
SOURCES = [
    f"{SRC}/_zpaq.cpp",
    f"{SRC}/vendor/libzpaq.cpp",
    # libsais (Apache 2.0, Ilya Grebnov) - linear-time suffix array
    # constructor used at levels 3-5 (BWT / LZ77-SA pre-pass). Several
    # times faster than the libdivsufsort-lite that libzpaq vendors
    # internally. We rename the original libsais.c to libsais.cpp so it
    # compiles uniformly through the C++ toolchain and avoids the mess
    # of mixing -std=c++ flags into a .c source on macOS/Linux. The
    # prefetch macros are patched (void* -> char* casts) so the C++
    # compiler accepts them.
    f"{SRC}/vendor/libsais.cpp",
]

extra_compile_args = []
extra_link_args = []
# NOJIT toggle. The libzpaq JIT path is x86_64-only and significantly
# faster on compression levels 3-5. We currently enable it only for
# Windows x86_64 wheels - that's where it's been verified to work end
# to end. Linux/manylinux builds segfault inside the JIT-emitted code
# (likely a libzpaq codegen issue against the manylinux_2_28 toolchain
# or a W^X restriction inside the build container); macOS would
# additionally need hardened-runtime carve-outs (MAP_JIT entitlements).
# Disabling JIT on those platforms means they fall back to the bytecode
# interpreter for the level 3-5 predictor - correct, just slower.
import platform as _plat
_machine = _plat.machine().lower()
_is_x86_64 = _machine in ("x86_64", "amd64", "x64")
define_macros = [
    # Use libsais (Apache 2.0) for suffix-array construction instead of the
    # libdivsufsort-lite that libzpaq vendors. Much faster on levels 3-5.
    ("ZPAQ_USE_LIBSAIS", "1"),
]
# JIT on Windows x86_64 + Linux x86_64. macOS still off because its
# hardened runtime requires the MAP_JIT entitlement, which we don't
# currently set up. Linux uses the manylinux_2_34 build image
# (AlmaLinux 9 / glibc 2.34) because the older 2_28 image crashed in
# the JIT-emitted code path on GH Actions runners.
_enable_jit = _is_x86_64 and sys.platform in ("win32", "linux")
# Diagnostic override: ZPAQ_FORCE_JIT=1 / =0 overrides the default.
import os as _os
_force = _os.environ.get("ZPAQ_FORCE_JIT")
if _force == "1" and _is_x86_64:
    _enable_jit = True
elif _force == "0":
    _enable_jit = False
if not _enable_jit:
    define_macros.append(("NOJIT", "1"))

# Profile-guided optimization. Two-phase build:
#   phase 1 (instrument): build instrumented .pyd, run scripts/pgo_workload.py
#   phase 2 (use):        recompile reading the profile data
# Controlled by ZPAQ_PGO_PHASE env var. Unset = normal non-PGO build.
# Profile data lives under build/pgo/ (relative to the package source dir)
# so it persists across the two builds within a single cibuildwheel cell.
_pgo_phase = _os.environ.get("ZPAQ_PGO_PHASE")
_pgo_dir = _os.environ.get("ZPAQ_PGO_DIR") or _os.path.abspath("build/pgo")
if _pgo_phase in ("instrument", "use"):
    _os.makedirs(_pgo_dir, exist_ok=True)

if sys.platform == "win32":
    # /MT statically links the C and C++ runtimes into the .pyd so end users
    # do not need any "VC++ redistributable" installed.
    # /arch:AVX2 lets the optimizer auto-vectorize where it can (Haswell+).
    extra_compile_args += ["/MT", "/O2", "/EHsc", "/std:c++14"]
    if _is_x86_64:
        extra_compile_args += ["/arch:AVX2"]
    libraries = ["advapi32"]
    # MSVC PGO uses /LTCG + /GENPROFILE / /USEPROFILE. Profile file path
    # is set with PGD= on the link line.
    _pgd_path = _os.path.join(_pgo_dir, "zpaq.pgd")
    if _pgo_phase == "instrument":
        extra_compile_args += ["/GL"]
        extra_link_args += ["/LTCG:PGI", f"/GENPROFILE:PGD={_pgd_path}"]
    elif _pgo_phase == "use":
        extra_compile_args += ["/GL"]
        extra_link_args += ["/LTCG:PGO", f"/USEPROFILE:PGD={_pgd_path}"]
else:
    extra_compile_args += ["-O2", "-fvisibility=hidden"]
    if _is_x86_64:
        extra_compile_args += ["-mavx2"]
    define_macros.append(("unix", "1"))
    libraries = []
    # gcc/clang PGO. Both compilers accept -fprofile-generate / -fprofile-use
    # with a directory. clang additionally needs llvm-profdata merge between
    # phases (handled by the workflow), but the compile flags are the same.
    if _pgo_phase == "instrument":
        extra_compile_args += [f"-fprofile-generate={_pgo_dir}"]
        extra_link_args += [f"-fprofile-generate={_pgo_dir}"]
    elif _pgo_phase == "use":
        extra_compile_args += [f"-fprofile-use={_pgo_dir}",
                               "-Wno-missing-profile"]
        extra_link_args += [f"-fprofile-use={_pgo_dir}"]


ext_modules = [
    Pybind11Extension(
        "zpaq._zpaq",
        sources=SOURCES,
        include_dirs=[f"{SRC}/vendor"],
        define_macros=define_macros,
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
        libraries=libraries,
        cxx_std=14,
    ),
]


class build_ext_static(build_ext):
    """On Windows, distutils inserts /MD by default. Strip it so /MT wins."""

    def build_extensions(self):
        if sys.platform == "win32":
            try:
                self.compiler.compile_options = [
                    o for o in self.compiler.compile_options
                    if o not in ("/MD", "/MDd")
                ]
            except AttributeError:
                pass
        super().build_extensions()


setup(
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext_static},
)
