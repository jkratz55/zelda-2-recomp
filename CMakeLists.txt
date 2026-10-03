cmake_minimum_required(VERSION 3.20)
project(zelda2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Your own dump of Zelda II (USA). Defaults to the first .nes file in the
# project directory or rom/.
file(GLOB _roms "${CMAKE_SOURCE_DIR}/*.nes" "${CMAKE_SOURCE_DIR}/rom/*.nes")
list(GET _roms 0 _default_rom)
set(ZELDA2_ROM "${_default_rom}" CACHE FILEPATH "Path to the Zelda II ROM")
if(NOT EXISTS "${ZELDA2_ROM}")
  message(FATAL_ERROR "Zelda II ROM not found. Configure with -DZELDA2_ROM=/path/to/zelda2.nes")
endif()

# ---- Recompile the ROM's 6502 code to C --------------------------------
set(GEN_DIR "${CMAKE_SOURCE_DIR}/generated")
set(GEN_SRCS)
foreach(b RANGE 0 7)
  list(APPEND GEN_SRCS "${GEN_DIR}/bank${b}.c")
endforeach()
list(APPEND GEN_SRCS "${GEN_DIR}/dispatch.c")

# discovered.txt is appended to while you play; make sure it exists so the
# dependency below is valid.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/config/discovered.txt")
  file(WRITE "${CMAKE_SOURCE_DIR}/config/discovered.txt" "")
endif()

add_custom_command(
  OUTPUT ${GEN_SRCS} "${GEN_DIR}/recomp_funcs.h"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/recomp.py"
          --rom "${ZELDA2_ROM}" --config "${CMAKE_SOURCE_DIR}/config" --out "${GEN_DIR}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/recomp.py" "${CMAKE_SOURCE_DIR}/tools/opcodes.py"
          "${CMAKE_SOURCE_DIR}/config/entries.txt" "${CMAKE_SOURCE_DIR}/config/discovered.txt"
          "${CMAKE_SOURCE_DIR}/config/labels.txt" "${ZELDA2_ROM}"
  COMMENT "Recompiling 6502 code to C"
  VERBATIM)

# ---- Game executable ----------------------------------------------------
add_executable(zelda2
  runtime/main.c runtime/cpu.c runtime/bus.c runtime/ppu.c runtime/apu.c runtime/png.c
  mods/mods.c
  ${GEN_SRCS})
target_include_directories(zelda2 PRIVATE runtime "${GEN_DIR}")
target_link_libraries(zelda2 PRIVATE SDL3::SDL3)
target_compile_definitions(zelda2 PRIVATE
  ZELDA2_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
  ZELDA2_DEFAULT_ROM="${ZELDA2_ROM}")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(zelda2 PRIVATE -Wall -Wextra -Wno-unused-label -Wno-unused-parameter)
  # generated files are huge and goto-heavy; keep them warning-free but quiet
  set_source_files_properties(${GEN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable")
endif()

# ---- Verification build -------------------------------------------------
# Same game, but the generated code yields before every instruction so that
# interrupts land exactly where the interpreter delivers them. Running this
# and `zelda2 --interpret` on the same input must give identical results;
# see tools/verify.sh.
set(GEN_STRICT_DIR "${CMAKE_BINARY_DIR}/generated_strict")
string(REPLACE "${GEN_DIR}" "${GEN_STRICT_DIR}" GEN_STRICT_SRCS "${GEN_SRCS}")
add_custom_command(
  OUTPUT ${GEN_STRICT_SRCS} "${GEN_STRICT_DIR}/recomp_funcs.h"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/recomp.py" --strict
          --rom "${ZELDA2_ROM}" --config "${CMAKE_SOURCE_DIR}/config" --out "${GEN_STRICT_DIR}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/recomp.py" "${CMAKE_SOURCE_DIR}/tools/opcodes.py"
          "${CMAKE_SOURCE_DIR}/config/entries.txt" "${CMAKE_SOURCE_DIR}/config/discovered.txt"
          "${CMAKE_SOURCE_DIR}/config/labels.txt" "${ZELDA2_ROM}"
  COMMENT "Recompiling 6502 code to C (strict verification mode)"
  VERBATIM)
add_executable(zelda2_verify EXCLUDE_FROM_ALL
  runtime/main.c runtime/cpu.c runtime/bus.c runtime/ppu.c runtime/apu.c runtime/png.c
  mods/mods.c
  ${GEN_STRICT_SRCS})
target_include_directories(zelda2_verify PRIVATE runtime "${GEN_STRICT_DIR}")
target_link_libraries(zelda2_verify PRIVATE SDL3::SDL3)
target_compile_definitions(zelda2_verify PRIVATE
  ZELDA2_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
  ZELDA2_DEFAULT_ROM="${ZELDA2_ROM}")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(${GEN_STRICT_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable")
endif()
