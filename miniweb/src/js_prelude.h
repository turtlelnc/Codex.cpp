#pragma once
// Byte embedding avoids MSVC literal-size limits; no runtime file is needed.
extern const unsigned char codex_miniweb_prelude[];
inline const char* miniweb_prelude(){return reinterpret_cast<const char*>(codex_miniweb_prelude);}
