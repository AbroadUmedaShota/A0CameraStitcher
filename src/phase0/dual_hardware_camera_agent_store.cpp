#include "a0/phase0/dual_hardware_camera_agent_store.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace a0::phase0 {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMaximumJournalBytes = 64U * 1024U;
constexpr std::string_view kJournalPrefix =
    "{\"schemaVersion\":\"a0.camera-agent.hardware-dual.pair-journal.v1\","
    "\"cameraMode\":\"DualCamera\",\"transactionId\":\"";
constexpr std::string_view kJournalSuffix =
    "\",\"state\":\"Reserved\",\"automaticRetryCount\":0}";
constexpr std::string_view kV2Prefix =
    "{\"schemaVersion\":\"a0.camera-agent.hardware-dual.pair-journal.v2\","
    "\"cameraMode\":\"DualCamera\",\"transactionId\":\"";
constexpr std::string_view kTerminalDirectory = "terminal";

[[noreturn]] void StoreFailure(std::string code, std::string message) {
    throw DualHardwarePairJournalStoreError(
        std::move(code), std::move(message));
}

bool IsSafeTransactionId(std::string_view value) noexcept {
    return value.size() == 32 &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        });
}

void ValidateTransactionId(std::string_view transaction_id) {
    if (!IsSafeTransactionId(transaction_id)) {
        StoreFailure(
            "InvalidTransactionId",
            "pair transactionId must contain exactly 32 hexadecimal characters");
    }
}

bool IsMissingPathError(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

DWORD AttributesOrMissing(const fs::path& path, bool& missing) {
    SetLastError(ERROR_SUCCESS);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        missing = false;
        return attributes;
    }
    const DWORD error = GetLastError();
    if (IsMissingPathError(error)) {
        missing = true;
        return 0;
    }
    StoreFailure(
        "StoreScopeInvalid",
        "pair journal path attributes could not be inspected safely");
}

void ValidateExistingPathNoReparse(
    const fs::path& path,
    bool require_directory,
    std::string_view error_code) {
    bool missing = false;
    const DWORD attributes = AttributesOrMissing(path, missing);
    if (missing || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (require_directory && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ||
        (!require_directory && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        StoreFailure(
            std::string(error_code),
            "pair journal path type or reparse policy is invalid");
    }
}

void ValidatePathChainNoReparse(const fs::path& path) {
    fs::path current = path.root_path();
    bool missing = false;
    DWORD attributes = AttributesOrMissing(current, missing);
    if (missing || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        StoreFailure(
            "InvalidStoreRoot",
            "pair journal drive root cannot be inspected as a reparse-free directory");
    }

    const fs::path relative = path.lexically_relative(path.root_path());
    for (const auto& component : relative) {
        current /= component;
        attributes = AttributesOrMissing(current, missing);
        if (missing) continue;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            StoreFailure(
                "InvalidStoreRoot",
                "pair journal root path chain contains a reparse point");
        }
    }
}

fs::path ValidateFixedLocalRoot(const fs::path& path) {
    const std::wstring input = path.native();
    const auto is_drive_letter = [](wchar_t value) noexcept {
        return (value >= L'A' && value <= L'Z') ||
            (value >= L'a' && value <= L'z');
    };
    const bool drive_qualified =
        input.size() >= 3 && is_drive_letter(input[0]) && input[1] == L':' &&
        (input[2] == L'\\' || input[2] == L'/');
    if (!path.is_absolute() || !path.has_root_name() ||
        !path.has_root_directory() || !drive_qualified ||
        input.find(L'\0') != std::wstring::npos ||
        input.find(L':', 2) != std::wstring::npos ||
        input.starts_with(L"\\\\") || input.starts_with(L"\\??\\")) {
        StoreFailure(
            "InvalidStoreRoot",
            "pair journal root must be an absolute drive-qualified local path");
    }

    for (const auto& component : path.relative_path()) {
        if (component == "." || component == "..") {
            StoreFailure(
                "InvalidStoreRoot",
                "pair journal root must not contain traversal components");
        }
    }

    const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        StoreFailure("InvalidStoreRoot", "pair journal root could not be normalized");
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        StoreFailure("InvalidStoreRoot", "pair journal root could not be normalized");
    }
    buffer.resize(written);
    const fs::path normalized = fs::path(buffer).lexically_normal();
    if (normalized == normalized.root_path()) {
        StoreFailure(
            "InvalidStoreRoot", "pair journal root cannot be the drive root");
    }

    const std::wstring normalized_native = normalized.native();
    std::wstring drive_root{
        normalized_native[0], L':', L'\\'};
    if (GetDriveTypeW(drive_root.c_str()) != DRIVE_FIXED) {
        StoreFailure(
            "InvalidStoreRoot", "pair journal root must reside on a fixed local drive");
    }
    ValidatePathChainNoReparse(normalized);

    bool missing = false;
    const DWORD attributes = AttributesOrMissing(normalized, missing);
    if (!missing && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        StoreFailure(
            "InvalidStoreRoot", "pair journal root exists but is not a directory");
    }
    return normalized;
}

fs::path ActiveDirectory(const fs::path& root) {
    return root / std::string(kDualHardwarePairJournalActiveDirectory);
}

fs::path JournalPath(const fs::path& root) {
    return ActiveDirectory(root) /
        std::string(kDualHardwarePairJournalFileName);
}

fs::path PartialJournalPath(const fs::path& root) {
    fs::path partial = JournalPath(root);
    partial += ".partial";
    return partial;
}

fs::path TerminalDirectory(const fs::path& root) {
    return root / std::string(kTerminalDirectory);
}

fs::path TerminalJournalPath(const fs::path& root, std::string_view transaction_id) {
    return TerminalDirectory(root) / (std::string(transaction_id) + ".json");
}

void ValidateRootDirectory(const fs::path& root) {
    (void)ValidateFixedLocalRoot(root);
    ValidateExistingPathNoReparse(root, true, "StoreScopeInvalid");

    std::error_code iterator_error;
    fs::directory_iterator iterator(root, iterator_error);
    if (iterator_error) {
        StoreFailure(
            "StoreScopeInvalid", "pair journal root could not be enumerated safely");
    }
    const fs::directory_iterator end;
    for (; iterator != end; iterator.increment(iterator_error)) {
        if (iterator_error) {
            StoreFailure(
                "StoreScopeInvalid", "pair journal root enumeration failed");
        }
        const auto name = iterator->path().filename();
        if (name != std::string(kDualHardwarePairJournalActiveDirectory) &&
            name != std::string(kTerminalDirectory)) {
            StoreFailure(
                "StoreScopeInvalid",
                "pair journal root contains an unexpected entry");
        }
    }
    if (iterator_error) {
        StoreFailure(
            "StoreScopeInvalid", "pair journal root enumeration failed");
    }
}

void ValidateActiveDirectory(const fs::path& root) {
    ValidateRootDirectory(root);
    ValidateExistingPathNoReparse(
        ActiveDirectory(root), true, "StoreScopeInvalid");
}

std::string SerializeRecord(std::string_view transaction_id) {
    return std::string(kV2Prefix) + std::string(transaction_id) +
        "\",\"state\":\"Reserved\",\"automaticRetryCount\":0}";
}

std::string StateName(DualHardwarePairJournalState state) {
    switch (state) {
    case DualHardwarePairJournalState::reserved: return "Reserved";
    case DualHardwarePairJournalState::dispatching: return "Dispatching";
    case DualHardwarePairJournalState::succeeded: return "Succeeded";
    case DualHardwarePairJournalState::failed: return "Failed";
    case DualHardwarePairJournalState::failed_partial: return "FailedPartial";
    case DualHardwarePairJournalState::watchdog_expired: return "WatchdogExpired";
    }
    StoreFailure("JournalInvalid", "unsupported pair journal state");
}

bool IsTerminal(DualHardwarePairJournalState state) noexcept {
    return state == DualHardwarePairJournalState::succeeded ||
        state == DualHardwarePairJournalState::failed ||
        state == DualHardwarePairJournalState::failed_partial ||
        state == DualHardwarePairJournalState::watchdog_expired;
}

std::string HexEncode(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded; encoded.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        encoded.push_back(digits[byte >> 4]); encoded.push_back(digits[byte & 15]);
    }
    return encoded;
}

std::string HexDecode(std::string_view value) {
    if ((value.size() & 1U) != 0) StoreFailure("JournalInvalid", "terminal result encoding is invalid");
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string decoded; decoded.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = digit(value[i]), low = digit(value[i + 1]);
        if (high < 0 || low < 0) StoreFailure("JournalInvalid", "terminal result encoding is invalid");
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

std::string SerializeV2(
    std::string_view id, DualHardwarePairJournalState state,
    std::string_view result = {}) {
    std::string json = std::string(kV2Prefix) + std::string(id) +
        "\",\"state\":\"" + StateName(state) +
        "\",\"automaticRetryCount\":0";
    if (IsTerminal(state)) json += ",\"terminalResultHex\":\"" + HexEncode(result) + "\"";
    return json + "}";
}

DualHardwarePairJournalRecord ParseRecord(const std::string& json) {
    const std::size_t expected_size =
        kJournalPrefix.size() + 32U + kJournalSuffix.size();
    if (json.size() == expected_size &&
        std::equal(kJournalPrefix.begin(), kJournalPrefix.end(), json.begin()) &&
        std::equal(kJournalSuffix.begin(), kJournalSuffix.end(),
            json.begin() + static_cast<std::ptrdiff_t>(kJournalPrefix.size() + 32U))) {
        const std::string transaction_id = json.substr(kJournalPrefix.size(), 32U);
        if (!IsSafeTransactionId(transaction_id)) StoreFailure("JournalInvalid", "pair journal transactionId is invalid");
        return {transaction_id, DualHardwarePairJournalState::reserved, 0, {}};
    }
    if (json.size() < kV2Prefix.size() + 32U ||
        !std::equal(kV2Prefix.begin(), kV2Prefix.end(), json.begin())) {
        StoreFailure(
            "JournalInvalid", "pair journal JSON is malformed or unsupported");
    }
    const std::string transaction_id =
        json.substr(kV2Prefix.size(), 32U);
    if (!IsSafeTransactionId(transaction_id)) {
        StoreFailure("JournalInvalid", "pair journal transactionId is invalid");
    }
    const std::string rest = json.substr(kV2Prefix.size() + 32U);
    constexpr std::string_view state_prefix = "\",\"state\":\"";
    constexpr std::string_view retry_suffix = "\",\"automaticRetryCount\":0}";
    if (!rest.starts_with(state_prefix)) StoreFailure("JournalInvalid", "pair journal state is missing");
    const auto end = rest.find('"', state_prefix.size());
    if (end == std::string::npos) StoreFailure("JournalInvalid", "pair journal state is invalid");
    const std::string name = rest.substr(state_prefix.size(), end - state_prefix.size());
    DualHardwarePairJournalState state;
    if (name == "Reserved") state = DualHardwarePairJournalState::reserved;
    else if (name == "Dispatching") state = DualHardwarePairJournalState::dispatching;
    else if (name == "Succeeded") state = DualHardwarePairJournalState::succeeded;
    else if (name == "Failed") state = DualHardwarePairJournalState::failed;
    else if (name == "FailedPartial") state = DualHardwarePairJournalState::failed_partial;
    else if (name == "WatchdogExpired") state = DualHardwarePairJournalState::watchdog_expired;
    else StoreFailure("JournalInvalid", "pair journal state is unsupported");
    const std::string tail = rest.substr(end);
    if (!IsTerminal(state)) {
        if (tail != retry_suffix) StoreFailure("JournalInvalid", "pair journal JSON is malformed");
        return {transaction_id, state, 0, {}};
    }
    constexpr std::string_view terminal_prefix = "\",\"automaticRetryCount\":0,\"terminalResultHex\":\"";
    if (!tail.starts_with(terminal_prefix) || !tail.ends_with("\"}"))
        StoreFailure("JournalInvalid", "terminal pair journal is malformed");
    const auto encoded = std::string_view(tail).substr(
        terminal_prefix.size(), tail.size() - terminal_prefix.size() - 2U);
    const std::string result = HexDecode(encoded);
    const std::string expected_id = "\"transactionId\":\"" + transaction_id + "\"";
    const std::string expected_state = "\"terminalState\":\"" + StateName(state) + "\"";
    if (result.empty() || result.front() != '{' || result.back() != '}' ||
        result.find(expected_id) == std::string::npos ||
        result.find(expected_state) == std::string::npos ||
        result.find("\"automaticRetryCount\":0") == std::string::npos ||
        std::any_of(result.begin(), result.end(), [](unsigned char value) {
            return value == 0 || (value < 0x20U && value != '\t' && value != '\r' && value != '\n');
        }))
        StoreFailure("JournalInvalid", "terminal result is invalid");
    return {transaction_id, state, 0, result};
}

std::string ReadJournalFile(const fs::path& path) {
    bool missing = false;
    const DWORD attributes = AttributesOrMissing(path, missing);
    if (missing || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        StoreFailure(
            "JournalInvalid",
            "pair journal is missing, non-regular, or a reparse point");
    }

    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        StoreFailure("JournalInvalid", "pair journal could not be opened safely");
    }

    try {
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            StoreFailure("JournalInvalid", "pair journal handle type is invalid");
        }
        const std::uint64_t size =
            (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
            information.nFileSizeLow;
        if (size == 0 || size > kMaximumJournalBytes ||
            size > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            StoreFailure("JournalInvalid", "pair journal size is invalid");
        }

        std::string contents(static_cast<std::size_t>(size), '\0');
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                contents.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD read = 0;
            if (!ReadFile(
                    handle, contents.data() + offset, chunk, &read, nullptr) ||
                read == 0) {
                StoreFailure("JournalInvalid", "pair journal could not be read completely");
            }
            offset += read;
        }
        CloseHandle(handle);
        return contents;
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
}

DualHardwarePairJournalRecord ReadActiveRecord(const fs::path& root) {
    ValidateActiveDirectory(root);

    const fs::path active = ActiveDirectory(root);
    std::error_code iterator_error;
    fs::directory_iterator iterator(active, iterator_error);
    if (iterator_error) {
        StoreFailure("JournalInvalid", "active pair journal directory is unreadable");
    }
    const fs::directory_iterator end;
    std::size_t entry_count = 0;
    for (; iterator != end; iterator.increment(iterator_error)) {
        if (iterator_error) {
            StoreFailure("JournalInvalid", "active pair journal enumeration failed");
        }
        ++entry_count;
        if (iterator->path().filename() !=
            std::string(kDualHardwarePairJournalFileName)) {
            StoreFailure(
                "JournalInvalid", "active pair journal directory is ambiguous");
        }
    }
    if (iterator_error || entry_count != 1) {
        StoreFailure(
            "JournalInvalid", "active pair journal is missing or ambiguous");
    }
    return ParseRecord(ReadJournalFile(JournalPath(root)));
}

std::optional<DualHardwarePairJournalRecord> TryReadActiveRecord(
    const fs::path& root) {
    (void)ValidateFixedLocalRoot(root);
    bool root_missing = false;
    (void)AttributesOrMissing(root, root_missing);
    if (root_missing) return std::nullopt;
    ValidateRootDirectory(root);

    bool active_missing = false;
    (void)AttributesOrMissing(ActiveDirectory(root), active_missing);
    if (active_missing) return std::nullopt;
    return ReadActiveRecord(root);
}

std::optional<DualHardwarePairJournalRecord> TryReadTerminalRecord(
    const fs::path& root, std::string_view transaction_id) {
    (void)ValidateFixedLocalRoot(root);
    bool root_missing = false;
    (void)AttributesOrMissing(root, root_missing);
    if (root_missing) return std::nullopt;
    ValidateRootDirectory(root);
    bool missing = false;
    (void)AttributesOrMissing(TerminalDirectory(root), missing);
    if (missing) return std::nullopt;
    ValidateExistingPathNoReparse(TerminalDirectory(root), true, "StoreScopeInvalid");
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(TerminalDirectory(root), error)) {
        if (error) StoreFailure("JournalInvalid", "terminal pair journal enumeration failed");
        const std::string name = entry.path().filename().string();
        if (name.size() != 37U || !name.ends_with(".json") ||
            !IsSafeTransactionId(std::string_view(name).substr(0, 32U)))
            StoreFailure("JournalInvalid", "terminal pair journal filename is unsafe");
        ValidateExistingPathNoReparse(entry.path(), false, "JournalInvalid");
    }
    if (error) StoreFailure("JournalInvalid", "terminal pair journal enumeration failed");
    const fs::path exact = TerminalJournalPath(root, transaction_id);
    bool exact_missing = false;
    (void)AttributesOrMissing(exact, exact_missing);
    if (exact_missing) return std::nullopt;
    auto record = ParseRecord(ReadJournalFile(exact));
    if (!IsTerminal(record.state)) StoreFailure("JournalInvalid", "terminal pair journal state is not terminal");
    if (record.transaction_id != transaction_id)
        StoreFailure("JournalInvalid", "terminal filename and transaction ID differ");
    return record;
}

void WriteExclusiveAndFlush(
    const fs::path& path,
    const std::string& contents) {
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        StoreFailure(
            "StoreIoFailure", "pair journal partial could not be created exclusively");
    }
    try {
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                contents.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!WriteFile(
                    handle, contents.data() + offset, chunk, &written, nullptr) ||
                written == 0) {
                StoreFailure("StoreIoFailure", "pair journal partial write failed");
            }
            offset += written;
        }
        if (!FlushFileBuffers(handle)) {
            StoreFailure("StoreIoFailure", "pair journal partial flush failed");
        }
        CloseHandle(handle);
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
}

void ReplaceAndVerify(
    const fs::path& directory, const fs::path& final_path,
    const std::string& contents) {
    fs::path partial = final_path; partial += ".partial";
    bool missing = false;
    (void)AttributesOrMissing(partial, missing);
    if (!missing) StoreFailure("StoreIoFailure", "stale pair journal partial exists");
    WriteExclusiveAndFlush(partial, contents);
    ValidateExistingPathNoReparse(directory, true, "StoreScopeInvalid");
    if (!MoveFileExW(partial.c_str(), final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        StoreFailure("StoreIoFailure", "atomic pair journal replacement failed");
}

void ClassifyExistingReservation(
    const DualHardwarePairJournalRecord& active,
    std::string_view requested_transaction_id) {
    if (active.transaction_id == requested_transaction_id) {
        StoreFailure(
            "DuplicateTransactionId",
            "pair transactionId is already reserved and will not be overwritten");
    }
    StoreFailure(
        "ActiveTransactionExists",
        "another pair transaction is active and will not be overwritten");
}

} // namespace

DualHardwarePairJournalStoreError::DualHardwarePairJournalStoreError(
    std::string code,
    std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& DualHardwarePairJournalStoreError::Code() const noexcept {
    return code_;
}

DualHardwarePairJournalStore::DualHardwarePairJournalStore(fs::path root)
    : root_(ValidateFixedLocalRoot(root)) {}

DualHardwarePairJournalRecord DualHardwarePairJournalStore::Reserve(
    std::string_view transaction_id) {
    ValidateTransactionId(transaction_id);
    if (const auto active = TryReadActiveRecord(root_)) {
        ClassifyExistingReservation(*active, transaction_id);
    }
    if (TryReadTerminalRecord(root_, transaction_id)) {
        StoreFailure("DuplicateTransactionId", "pair transactionId is already terminal");
    }

    std::error_code create_error;
    fs::create_directories(root_, create_error);
    if (create_error) {
        StoreFailure("StoreIoFailure", "pair journal root could not be created");
    }
    ValidateRootDirectory(root_);

    const fs::path active_directory = ActiveDirectory(root_);
    if (!CreateDirectoryW(active_directory.c_str(), nullptr)) {
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            ClassifyExistingReservation(ReadActiveRecord(root_), transaction_id);
        }
        StoreFailure(
            "StoreIoFailure", "active pair journal directory could not be reserved");
    }
    ValidateActiveDirectory(root_);

    const std::string contents = SerializeRecord(transaction_id);
    const fs::path partial = PartialJournalPath(root_);
    const fs::path journal = JournalPath(root_);
    WriteExclusiveAndFlush(partial, contents);
    ValidateActiveDirectory(root_);

    bool final_missing = false;
    (void)AttributesOrMissing(journal, final_missing);
    if (!final_missing) {
        StoreFailure(
            "StoreIoFailure", "refusing to overwrite an existing pair journal");
    }
    if (!MoveFileExW(
            partial.c_str(), journal.c_str(), MOVEFILE_WRITE_THROUGH)) {
        StoreFailure("StoreIoFailure", "atomic pair journal publish failed");
    }

    const DualHardwarePairJournalRecord persisted = ReadActiveRecord(root_);
    if (persisted.transaction_id != transaction_id ||
        persisted.state != DualHardwarePairJournalState::reserved ||
        persisted.automatic_retry_count != 0) {
        StoreFailure(
            "JournalInvalid", "persisted pair journal does not match the reservation");
    }
    return persisted;
}

std::optional<DualHardwarePairJournalRecord>
DualHardwarePairJournalStore::Query(std::string_view transaction_id) const {
    ValidateTransactionId(transaction_id);
    if (const auto terminal = TryReadTerminalRecord(root_, transaction_id)) return terminal;
    const auto active = TryReadActiveRecord(root_);
    if (!active || active->transaction_id != transaction_id) {
        return std::nullopt;
    }
    return active;
}

DualHardwarePairJournalRecord DualHardwarePairJournalStore::BeginDispatch(
    std::string_view transaction_id) {
    ValidateTransactionId(transaction_id);
    if (TryReadTerminalRecord(root_, transaction_id)) StoreFailure("DispatchAlreadyStarted", "pair transaction is already terminal");
    const auto active = TryReadActiveRecord(root_);
    if (!active || active->transaction_id != transaction_id)
        StoreFailure("TransactionIdMismatch", "pair transaction is not the active reservation");
    if (active->state != DualHardwarePairJournalState::reserved)
        StoreFailure("DispatchAlreadyStarted", "pair transaction dispatch already started");
    ReplaceAndVerify(ActiveDirectory(root_), JournalPath(root_),
        SerializeV2(transaction_id, DualHardwarePairJournalState::dispatching));
    const auto persisted = ReadActiveRecord(root_);
    if (persisted.state != DualHardwarePairJournalState::dispatching)
        StoreFailure("JournalInvalid", "dispatch transition did not persist");
    return persisted;
}

DualHardwarePairJournalRecord DualHardwarePairJournalStore::CompleteTerminal(
    std::string_view transaction_id,
    DualHardwarePairJournalState terminal_state,
    std::string_view terminal_result_json) {
    ValidateTransactionId(transaction_id);
    if (!IsTerminal(terminal_state) || terminal_result_json.empty() ||
        terminal_result_json.size() > kMaximumJournalBytes / 2U)
        StoreFailure("JournalInvalid", "terminal pair result is invalid");
    if (const auto terminal = TryReadTerminalRecord(root_, transaction_id)) {
        StoreFailure("DispatchAlreadyStarted", "pair transaction is already terminal");
    }
    const auto active = TryReadActiveRecord(root_);
    if (!active || active->transaction_id != transaction_id)
        StoreFailure("TransactionIdMismatch", "pair transaction is not active");
    if (active->state != DualHardwarePairJournalState::dispatching)
        StoreFailure("DispatchNotStarted", "pair dispatch has not started");
    if (!CreateDirectoryW(TerminalDirectory(root_).c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        StoreFailure("StoreIoFailure", "terminal journal directory could not be created");
    ValidateExistingPathNoReparse(TerminalDirectory(root_), true, "StoreScopeInvalid");
    const auto final_path = TerminalJournalPath(root_, transaction_id);
    bool final_missing = false; (void)AttributesOrMissing(final_path, final_missing);
    if (!final_missing) StoreFailure("StoreIoFailure", "terminal journal already exists");
    fs::path partial = final_path; partial += ".partial";
    WriteExclusiveAndFlush(partial, SerializeV2(transaction_id, terminal_state, terminal_result_json));
    if (!MoveFileExW(partial.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH))
        StoreFailure("StoreIoFailure", "atomic terminal journal publication failed");
    const auto persisted = TryReadTerminalRecord(root_, transaction_id);
    if (!persisted || persisted->transaction_id != transaction_id ||
        persisted->state != terminal_state || persisted->terminal_result_json != terminal_result_json)
        StoreFailure("JournalInvalid", "terminal journal reread did not match");
    if (!DeleteFileW(JournalPath(root_).c_str()) || !RemoveDirectoryW(ActiveDirectory(root_).c_str()))
        StoreFailure("StoreIoFailure", "active journal could not be removed after terminal publication");
    return *persisted;
}

} // namespace a0::phase0
