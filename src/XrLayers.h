// XrLayers.h - BUILD 233. The one hand-rolled part of the universal OpenXR
// API layer suppression: pulling a string field out of a layer manifest.
//
// It lives in a header of its own, free of Windows and of the log, so that
// tools\layertest can run it against this machine's real manifests and
// against the adversarial shapes the format permits. Everything else in the
// feature is plain Win32 (RegEnumValueA, CreateFileA, SetEnvironmentVariableA)
// and is exercised end to end by any launch on a machine that has an enabled
// layer.
//
// Why a scan and not a JSON parser: these are two fixed keys in a small file
// the OpenXR loader has already parsed and accepted before we ever see it,
// and this mod ships no JSON dependency.
#pragma once

#include <string>

namespace grwxr {
namespace xrlayers {

// Find "key" and return the string value that follows it. `from` bounds the
// search to a region of the document. Returns false when the key is absent,
// which is a real and expected case: a layer may carry no disable_environment
// at all, and the caller says so out loud rather than guessing a name.
inline bool manifest_string(const std::string& text, const char* key,
                            std::string& out, size_t from = 0) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    size_t k = text.find(needle, from);
    if (k == std::string::npos) return false;
    size_t c = text.find(':', k + needle.size());
    if (c == std::string::npos) return false;
    size_t a = text.find('"', c + 1);
    if (a == std::string::npos) return false;
    size_t b = a + 1;
    std::string v;
    while (b < text.size() && text[b] != '"') {
        // manifests escape backslashes in library_path; the fields read here
        // are plain identifiers, but handle it rather than truncate.
        if (text[b] == '\\' && b + 1 < text.size()) ++b;
        v += text[b++];
    }
    if (b >= text.size()) return false;   // unterminated string
    out = v;
    return true;
}

// The layer's own name is the first "name" inside the "api_layer" object. A
// manifest may also carry instance_extensions, whose entries each have a
// "name", and the format fixes no key order. If one of those comes first the
// name here would be an extension's, so anchor the search at "api_layer" and
// refuse the ambiguous case rather than print a wrong layer name into the
// evidence. Returns false when no trustworthy name could be read.
inline bool layer_name(const std::string& text, std::string& out) {
    size_t obj  = text.find("\"api_layer\"");
    size_t from = (obj == std::string::npos) ? 0 : obj;
    size_t exts = text.find("\"instance_extensions\"", from);
    size_t at   = text.find("\"name\"", from);
    if (at == std::string::npos) return false;
    if (exts != std::string::npos && exts < at) return false;
    return manifest_string(text, "name", out, from);
}

// disable_environment is unique in the format, so it needs no anchor. This is
// the load-bearing field: without it a layer cannot be switched off at all.
inline bool disable_env(const std::string& text, std::string& out) {
    return manifest_string(text, "disable_environment", out);
}

}  // namespace xrlayers
}  // namespace grwxr
