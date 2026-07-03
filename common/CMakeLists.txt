# SPDX-License-Identifier: GPL-3.0-or-later
#
# common/ — the platform-neutral engine, compiled by BOTH the Windows and macOS
# apps. Pure C++; links only sqlite3. No UI, no HTTP, no OS-specific paths —
# those live in the per-platform layers (../Win32/platform, ../mac/platform).
#
# The PUBLIC include dir is this directory, so any consumer that links
# RabbitEarsCore resolves "core/..", "db/..", "models/..", "platform/<seam>.h",
# and "ui/DockLayout.h" from here.
add_library(RabbitEarsCore STATIC
  core/M3uParser.cpp
  db/Database.cpp
  ui/DockLayout.cpp   # pure layout-tree logic (no HWNDs) — unit-tested headlessly
  ui/Skin.cpp)        # shared skin model + codecs (theme engine) — unit-tested headlessly
target_include_directories(RabbitEarsCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(RabbitEarsCore PUBLIC sqlite3)
