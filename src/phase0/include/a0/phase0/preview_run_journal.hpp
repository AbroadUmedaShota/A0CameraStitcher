#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::phase0::experimental {

// Small, non-sensitive evidence journal for a preview attempt. Its schema has
// no field for frames, camera IDs, capabilities, free-form diagnostics, or IPC
// credentials: callers can record only a fixed event label and a numeric value.
class PreviewRunJournal final {
  public:
    explicit PreviewRunJournal(const std::filesystem::path& path) {
        if (!path.is_absolute()) throw std::invalid_argument("preview journal path must be absolute");
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = nullptr;
            throw std::runtime_error("preview journal create failed");
        }
    }

    ~PreviewRunJournal() {
        if (handle_) CloseHandle(handle_);
    }

    PreviewRunJournal(const PreviewRunJournal&) = delete;
    PreviewRunJournal& operator=(const PreviewRunJournal&) = delete;
    PreviewRunJournal(PreviewRunJournal&&) = delete;
    PreviewRunJournal& operator=(PreviewRunJournal&&) = delete;

    void Record(std::string_view event, std::uint64_t value = 0) {
        if (failed_) throw std::runtime_error("preview journal is failed");
        if (!ValidEvent(event)) throw std::invalid_argument("preview journal event is invalid");
        const auto line = "{\"sequence\":" + std::to_string(sequence_ + 1) +
            ",\"tickCount64\":" + std::to_string(GetTickCount64()) + ",\"event\":\"" +
            std::string(event) + "\",\"value\":" + std::to_string(value) + "}\n";
        DWORD written{};
        const BOOL written_all = WriteFile(handle_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
            written == line.size();
        const BOOL flushed = written_all && FlushFileBuffers(handle_);
        if (!flushed) {
            failed_ = true;
            throw std::runtime_error("preview journal record failed");
        }
        ++sequence_;
    }

  private:
    static bool ValidEvent(std::string_view value) noexcept {
        if (value.empty() || value.size() > 48) return false;
        for (const auto character : value) {
            if (!((character >= 'a' && character <= 'z') || character == '_')) return false;
        }
        return true;
    }

    HANDLE handle_{};
    std::uint64_t sequence_{};
    bool failed_{};
};

} // namespace a0::phase0::experimental
