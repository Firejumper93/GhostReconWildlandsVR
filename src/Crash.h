#pragma once
#include <windows.h>
namespace grwxr { namespace crash {
// Vectored exception handler: logs fault code, address, owning module, registers
// and a stack scan, then declines to handle so the game behaves normally.
void install(HMODULE self);
void remove();
}}
