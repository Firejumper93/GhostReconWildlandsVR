#pragma once
namespace grwxr { namespace ansel {
// Hook ansel::setConfiguration via its IAT slot and capture the config struct,
// which declares the engine's right/up/forward basis vectors.
bool install();
void drain();
}}
