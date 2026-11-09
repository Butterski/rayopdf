// SPDX-License-Identifier: Apache-2.0
// Start-up of the fully static Linux binary (RAYOPDF_STATIC): what glibc's libc.a runs before main
// that RayoPDF has no use for.
//
// _dl_non_dynamic_init asks _dl_get_origin for the directory of the executable, which it keeps for
// $ORIGIN in the search paths of a static program's dlopen: a readlink of /proc/self/exe and a
// malloc on every start, about 18 us of a 1.1 ms one-page conversion (odl 183, kernel time of the
// /proc lookup). RayoPDF never calls dlopen and nothing in the static binary does (no NSS, no
// iconv), so the origin is "unknown", the value glibc itself uses when the readlink fails. Defined
// here, the linker takes this definition and leaves libc.a's dl-origin.o out.

extern "C" const char* _dl_get_origin() {
    return reinterpret_cast<const char*>(-1);
}
