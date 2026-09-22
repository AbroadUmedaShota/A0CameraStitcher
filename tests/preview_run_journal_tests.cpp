#include "a0/phase0/preview_run_journal.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using a0::phase0::experimental::PreviewRunJournal;
namespace fs = std::filesystem;

namespace {
int failures{};
void Check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
fs::path JournalPath() {
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) throw std::runtime_error("temporary directory unavailable");
    return fs::path(temporary) / (L"A0PreviewRunJournal-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
}
bool Rejects(auto&& action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
bool MatchesRecord(std::string_view line, std::uint64_t sequence, std::string_view event, std::uint64_t value) {
    const auto prefix = "{\"sequence\":" + std::to_string(sequence) + ",\"tickCount64\":";
    const auto suffix = ",\"event\":\"" + std::string(event) + "\",\"value\":" + std::to_string(value) + "}";
    if (!line.starts_with(prefix) || !line.ends_with(suffix)) return false;
    const auto tick = line.substr(prefix.size(), line.size() - prefix.size() - suffix.size());
    return !tick.empty() && tick.find_first_not_of("0123456789") == std::string_view::npos;
}
}

int main() {
    const auto path = JournalPath();
    try {
        {
            PreviewRunJournal journal(path);
            journal.Record("worker_started");
            journal.Record("worker_closed", 2);
            Check(Rejects([&] { PreviewRunJournal duplicate(path); }), "existing journal must never be overwritten");
            Check(Rejects([&] { journal.Record("Worker_started"); }), "uppercase event must reject");
            Check(Rejects([&] { journal.Record("has-dash"); }), "punctuation event must reject");
            Check(Rejects([&] { journal.Record("event_2"); }), "digit event must reject");
            Check(Rejects([&] { journal.Record(std::string(49, 'a')); }), "overlong event must reject");
        }

        std::ifstream input(path, std::ios::binary);
        std::string first, second, extra;
        std::getline(input, first);
        std::getline(input, second);
        std::getline(input, extra);
        Check(MatchesRecord(first, 1, "worker_started", 0),
              "first record must have sequence one and zero value");
        Check(MatchesRecord(second, 2, "worker_closed", 2),
              "second record must advance sequence and retain numeric value");
        Check(extra.empty(), "journal must contain exactly two lines");
        input.close();
        if (!DeleteFileW(path.c_str())) throw std::runtime_error("exact journal cleanup failed");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    std::cout << "{\"mode\":\"preview-run-journal\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
