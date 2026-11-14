#pragma once
// Native-only artifact beside the PNG; never transported through page logs.
inline constexpr const char* miniweb_report_suffix = ".report.json";
int codex_miniweb_worker(int argc, char** argv);
