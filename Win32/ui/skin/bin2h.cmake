# SPDX-License-Identifier: GPL-3.0-or-later
#
# Embed a binary file (a compiled .cso shader blob) as a C byte array in a header,
# so the /MT static-CRT app ships no extra runtime DLLs (no d3dcompiler_47.dll) and
# no loose .cso files. Pure CMake — no external bin2c tool — using file(READ HEX).
#
# Usage (script mode):
#   cmake -DIN=<blob> -DOUT=<header> -DVAR=<symbol> -P bin2h.cmake
#
# Emits:  static const unsigned char <VAR>[]     = { 0x.., ... };
#         static const unsigned int  <VAR>_len   = <byte count>;

if(NOT DEFINED IN OR NOT DEFINED OUT OR NOT DEFINED VAR)
  message(FATAL_ERROR "bin2h.cmake requires -DIN= -DOUT= -DVAR=")
endif()

file(READ "${IN}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _bytes "${_hexlen} / 2")

set(_body "")
set(_i 0)
set(_col 0)
while(_i LESS _hexlen)
  string(SUBSTRING "${_hex}" ${_i} 2 _b)
  string(APPEND _body "0x${_b},")
  math(EXPR _col "${_col} + 1")
  if(_col GREATER_EQUAL 16)
    string(APPEND _body "\n    ")
    set(_col 0)
  endif()
  math(EXPR _i "${_i} + 2")
endwhile()

set(_out "// Generated from ${IN} by bin2h.cmake — do not edit.\n")
string(APPEND _out "static const unsigned char ${VAR}[] = {\n    ${_body}\n};\n")
string(APPEND _out "static const unsigned int ${VAR}_len = ${_bytes};\n")
file(WRITE "${OUT}" "${_out}")
