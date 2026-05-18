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
    # internally. Compiled as C (setuptools selects the C compiler by
    # file extension) so MSVC's _mm_prefetch overload set is happy.
    f"{SRC}/vendor/libsais.c",
]

extra_compile_args = []
extra_link_args = []
# NOJIT toggle. The libzpaq JIT path is x86_64-only and significantly
# faster on compression levels 3-5. We default to JIT-on for the local
# build / Windows + Linux x86_64 wheels and only disable it for arm64
# builds (Apple Silicon, aarch64) where the JIT would emit invalid code.
import platform as _plat
_machine = _plat.machine().lower()
_is_x86_64 = _machine in ("x86_64", "amd64", "x64")
define_macros = [
    # Use libsais (Apache 2.0) for suffix-array construction instead of the
    # libdivsufsort-lite that libzpaq vendors. Much faster on levels 3-5.
    ("ZPAQ_USE_LIBSAIS", "1"),
]
if not _is_x86_64:
    define_macros.append(("NOJIT", "1"))

if sys.platform == "win32":
    # /MT statically links the C and C++ runtimes into the .pyd so end users
    # do not need any "VC++ redistributable" installed. Python itself already
    # ships vcruntime140.dll, and once we are static neither msvcp140.dll
    # nor vcruntime140_1.dll need to be present on the target machine.
    #
    # /arch:AVX2 lets the optimizer auto-vectorize where it can. AVX2 has been
    # standard on Intel since Haswell (2013) and AMD since Excavator (2015);
    # CPUs older than that don't get accelerated x86_64 wheels but can still
    # fall back to the sdist build with the flag dropped.
    extra_compile_args += ["/MT", "/O2", "/EHsc", "/std:c++14"]
    if _is_x86_64:
        extra_compile_args += ["/arch:AVX2"]
    libraries = ["advapi32"]  # libzpaq uses wincrypt on Windows
else:
    extra_compile_args += ["-O2", "-fvisibility=hidden"]
    if _is_x86_64:
        extra_compile_args += ["-mavx2"]
    # libzpaq's Windows-vs-unix preprocessor split keys off the `unix` macro.
    define_macros.append(("unix", "1"))
    libraries = []


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
            for ext in self.extensions:
                ext.extra_compile_args = [
                    a for a in (ext.extra_compile_args or [])
                ]
            # Remove distutils' default /MD from the compiler invocation.
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
