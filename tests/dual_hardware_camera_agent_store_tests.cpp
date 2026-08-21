#include "a0/phase0/dual_hardware_camera_agent_store.hpp"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace a0::phase0;

int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Action>
void CheckStoreError(
    std::string_view expected_code,
    Action&& action,
    std::string_view message) {
    try {
        action();
        Check(false, message);
    } catch (const DualHardwarePairJournalStoreError& error) {
        Check(error.Code() == expected_code, message);
    } catch (...) {
        Check(false, message);
    }
}

std::string UniqueSuffix() {
    static std::atomic<unsigned long long> sequence{};
    return std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64()) + "-" +
        std::to_string(++sequence);
}

class TempSandbox final {
public:
    TempSandbox() {
        parent_ = fs::absolute(fs::temp_directory_path()).lexically_normal();
        if (parent_.filename().empty()) parent_ = parent_.parent_path();
        root_ = parent_ / ("a0-dual-pair-store-test-" + UniqueSuffix());
        if (root_.parent_path() != parent_ || root_.filename().empty()) {
            throw std::runtime_error("test sandbox path escaped the temporary parent");
        }
        fs::create_directory(root_);
    }

    ~TempSandbox() {
        const fs::path normalized = fs::absolute(root_).lexically_normal();
        if (normalized.parent_path() != parent_ || normalized.filename().empty()) {
            return;
        }
        std::error_code cleanup_error;
        fs::remove_all(normalized, cleanup_error);
    }

    [[nodiscard]] fs::path Child(std::string_view name) const {
        return root_ / std::string(name);
    }

private:
    fs::path parent_;
    fs::path root_;
};

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("test file could not be opened");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) throw std::runtime_error("test file could not be written");
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("test file could not be read");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

fs::path JournalPath(const fs::path& root) {
    return root / "active" / "pair-journal.json";
}

fs::path TerminalPath(const fs::path& root, std::string_view transaction_id) {
    return root / "terminal" / (std::string(transaction_id) + ".json");
}

void PrepareJournal(const fs::path& root, const std::string& contents) {
    fs::create_directories(root / "active");
    WriteText(JournalPath(root), contents);
}

void TestReserveAndRestartQuery() {
    TempSandbox sandbox;
    const fs::path root = sandbox.Child("valid-store");
    const std::string transaction_id = "0123456789abcdef0123456789abcdef";
    const std::string other_id = "fedcba9876543210fedcba9876543210";

    DualHardwarePairJournalStore first(root);
    Check(!first.Query(transaction_id).has_value(),
        "an absent pair journal must query as not found");

    const auto reserved = first.Reserve(transaction_id);
    Check(reserved.transaction_id == transaction_id &&
          reserved.state == DualHardwarePairJournalState::reserved &&
          reserved.automatic_retry_count == 0,
        "reserve must return one no-retry Reserved record");
    Check(fs::is_regular_file(JournalPath(root)),
        "reserve must atomically publish the canonical pair journal");
    Check(!fs::exists(root / "active" / "pair-journal.json.partial"),
        "successful reserve must not leave a partial journal");

    const std::string original_journal = ReadText(JournalPath(root));
    DualHardwarePairJournalStore restarted(root);
    const auto queried = restarted.Query(transaction_id);
    Check(queried.has_value() && queried->transaction_id == transaction_id &&
          queried->state == DualHardwarePairJournalState::reserved &&
          queried->automatic_retry_count == 0,
        "a new store instance must recover the same Reserved transaction");
    Check(!restarted.Query(other_id).has_value(),
        "querying another ID must never return the active transaction");

    CheckStoreError("DuplicateTransactionId", [&] {
        (void)restarted.Reserve(transaction_id);
    }, "reserving the same ID twice must fail closed");
    Check(ReadText(JournalPath(root)) == original_journal,
        "same-ID duplicate rejection must not overwrite the journal");

    CheckStoreError("ActiveTransactionExists", [&] {
        (void)restarted.Reserve(other_id);
    }, "reserving another ID while one is active must fail closed");
    Check(ReadText(JournalPath(root)) == original_journal,
        "other-ID active rejection must not overwrite the journal");
}

void TestDispatchAndTerminalTransitionsAreDurable() {
    TempSandbox sandbox;
    const fs::path root = sandbox.Child("terminal-store");
    const std::string transaction_id = "33333333333333333333333333333333";
    const std::string other_id = "44444444444444444444444444444444";
    const std::string result =
        "{\"transactionId\":\"" + transaction_id +
        "\",\"originals\":[],\"terminalState\":\"Succeeded\","
        "\"failureCode\":\"None\",\"evidence\":{\"automaticRetryCount\":0}}";
    DualHardwarePairJournalStore store(root);
    (void)store.Reserve(transaction_id);
    const auto dispatching = store.BeginDispatch(transaction_id);
    Check(dispatching.state == DualHardwarePairJournalState::dispatching &&
          dispatching.automatic_retry_count == 0,
        "BeginDispatch must durably transition Reserved to no-retry Dispatching");
    DualHardwarePairJournalStore restarted_dispatching(root);
    const auto recovered_dispatching = restarted_dispatching.Query(transaction_id);
    Check(recovered_dispatching && recovered_dispatching->state == DualHardwarePairJournalState::dispatching,
        "restart must recover Dispatching without reverting to Reserved");
    CheckStoreError("DispatchAlreadyStarted", [&] {
        (void)restarted_dispatching.BeginDispatch(transaction_id);
    }, "Dispatching must never be started twice");
    CheckStoreError("TransactionIdMismatch", [&] {
        (void)restarted_dispatching.CompleteTerminal(
            other_id, DualHardwarePairJournalState::succeeded, result);
    }, "terminal completion must require the exact active transaction ID");

    const auto terminal = restarted_dispatching.CompleteTerminal(
        transaction_id, DualHardwarePairJournalState::succeeded, result);
    Check(terminal.state == DualHardwarePairJournalState::succeeded &&
          terminal.terminal_result_json == result && terminal.automatic_retry_count == 0,
        "terminal completion must return the reread durable result");
    Check(!fs::exists(root / "active"),
        "active state must be removed only after terminal publication");
    Check(fs::is_regular_file(TerminalPath(root, transaction_id)) &&
          !fs::exists(TerminalPath(root, transaction_id).wstring() + L".partial"),
        "terminal completion must leave one canonical journal and no partial");
    DualHardwarePairJournalStore restarted_terminal(root);
    const auto recovered_terminal = restarted_terminal.Query(transaction_id);
    Check(recovered_terminal && recovered_terminal->state == DualHardwarePairJournalState::succeeded &&
          recovered_terminal->terminal_result_json == result,
        "restart query must recover the exact terminal result without active state");
    Check(!restarted_terminal.Query(other_id),
        "terminal query for another ID must be not found without leaking the active ID");

    (void)restarted_terminal.Reserve(other_id);
    Check(restarted_terminal.Query(transaction_id)->state ==
              DualHardwarePairJournalState::succeeded &&
          restarted_terminal.Query(other_id)->state ==
              DualHardwarePairJournalState::reserved,
        "one historical terminal and one different active reservation must coexist");
    (void)restarted_terminal.BeginDispatch(other_id);
    const std::string result2 =
        "{\"transactionId\":\"" + other_id +
        "\",\"originals\":[],\"terminalState\":\"Failed\","
        "\"failureCode\":\"CaptureCameraA\",\"evidence\":{\"automaticRetryCount\":0}}";
    (void)restarted_terminal.CompleteTerminal(
        other_id, DualHardwarePairJournalState::failed, result2);
    DualHardwarePairJournalStore restarted_twice(root);
    Check(restarted_twice.Query(transaction_id)->state ==
              DualHardwarePairJournalState::succeeded &&
          restarted_twice.Query(other_id)->state ==
              DualHardwarePairJournalState::failed,
        "restart must recover both exact-ID terminal journals");
    CheckStoreError("DuplicateTransactionId", [&] {
        (void)restarted_twice.Reserve(transaction_id);
    }, "a historical terminal must reject only the same transaction ID");
}

void TestCloseReservedBeforeDispatchIsDurableAndExact() {
    TempSandbox sandbox;
    const fs::path root = sandbox.Child("close-reserved-store");
    const std::string transaction_id = "55555555555555555555555555555555";
    const std::string other_id = "66666666666666666666666666666666";

    DualHardwarePairJournalStore store(root);
    CheckStoreError("TransactionIdMismatch", [&] {
        (void)store.CloseReservedBeforeDispatch(transaction_id);
    }, "closing a missing reservation must fail closed");

    (void)store.Reserve(transaction_id);
    CheckStoreError("TransactionIdMismatch", [&] {
        (void)store.CloseReservedBeforeDispatch(other_id);
    }, "closing another transaction ID must not alter the active reservation");

    const auto closed = store.CloseReservedBeforeDispatch(transaction_id);
    Check(closed.transaction_id == transaction_id &&
          closed.state == DualHardwarePairJournalState::closed_before_dispatch &&
          closed.automatic_retry_count == 0 &&
          closed.terminal_result_json.empty(),
        "exact Reserved close must return a reread ClosedBeforeDispatch tombstone");
    Check(!fs::exists(root / "active") &&
          fs::is_regular_file(TerminalPath(root, transaction_id)),
        "active state must be removed only after the close tombstone is durable");

    DualHardwarePairJournalStore restarted(root);
    const auto recovered = restarted.Query(transaction_id);
    Check(recovered &&
          recovered->state == DualHardwarePairJournalState::closed_before_dispatch,
        "restart query must recover the exact ClosedBeforeDispatch tombstone");
    const auto idempotent = restarted.CloseReservedBeforeDispatch(transaction_id);
    Check(idempotent.state == DualHardwarePairJournalState::closed_before_dispatch,
        "same-ID close retry after an unknown response must be idempotent");

    (void)restarted.Reserve(other_id);
    (void)restarted.BeginDispatch(other_id);
    CheckStoreError("DispatchAlreadyStarted", [&] {
        (void)restarted.CloseReservedBeforeDispatch(other_id);
    }, "Dispatching must never be downgraded to ClosedBeforeDispatch");

    const std::string terminal_result =
        "{\"transactionId\":\"" + other_id +
        "\",\"originals\":[],\"terminalState\":\"Failed\","
        "\"failureCode\":\"CaptureCameraA\","
        "\"evidence\":{\"automaticRetryCount\":0}}";
    const auto capture_terminal = restarted.CompleteTerminal(
        other_id, DualHardwarePairJournalState::failed, terminal_result);
    CheckStoreError("DispatchAlreadyStarted", [&] {
        (void)restarted.CloseReservedBeforeDispatch(other_id);
    }, "a capture-terminal same-ID close must be rejected");
    const auto unchanged_terminal = restarted.Query(other_id);
    Check(unchanged_terminal &&
          unchanged_terminal->state == capture_terminal.state &&
          unchanged_terminal->terminal_result_json == terminal_result,
        "capture-terminal evidence must remain unchanged after close rejection");

    const fs::path partial_root = sandbox.Child("partial-close-store");
    const std::string partial_id = "77777777777777777777777777777777";
    const std::string next_id = "88888888888888888888888888888888";
    DualHardwarePairJournalStore partial_store(partial_root);
    (void)partial_store.Reserve(partial_id);
    const HANDLE active_lock = CreateFileW(
        JournalPath(partial_root).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (active_lock == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("test could not lock the active journal");
    }
    CheckStoreError("StoreIoFailure", [&] {
        (void)partial_store.CloseReservedBeforeDispatch(partial_id);
    }, "active deletion failure after tombstone publication must fail closed");
    Check(fs::is_regular_file(TerminalPath(partial_root, partial_id)) &&
          fs::is_regular_file(JournalPath(partial_root)),
        "partial close failure must retain both tombstone and active reservation");
    (void)CloseHandle(active_lock);

    DualHardwarePairJournalStore partial_restarted(partial_root);
    const auto recovered_close =
        partial_restarted.CloseReservedBeforeDispatch(partial_id);
    Check(recovered_close.state ==
              DualHardwarePairJournalState::closed_before_dispatch &&
          !fs::exists(partial_root / "active"),
        "restart exact close must finish active removal from the tombstone");
    const auto next_reserved = partial_restarted.Reserve(next_id);
    Check(next_reserved.transaction_id == next_id &&
          next_reserved.state == DualHardwarePairJournalState::reserved,
        "a different transaction may reserve only after partial close recovery");
}

void TestLegacyV1ReservedJournalRemainsReadable() {
    TempSandbox sandbox;
    const fs::path root = sandbox.Child("legacy-v1-store");
    const std::string transaction_id = "55555555555555555555555555555555";
    PrepareJournal(root,
        "{\"schemaVersion\":\"a0.camera-agent.hardware-dual.pair-journal.v1\","
        "\"cameraMode\":\"DualCamera\",\"transactionId\":\"" + transaction_id +
        "\",\"state\":\"Reserved\",\"automaticRetryCount\":0}");
    DualHardwarePairJournalStore store(root);
    const auto record = store.Query(transaction_id);
    Check(record && record->state == DualHardwarePairJournalState::reserved &&
          record->automatic_retry_count == 0,
        "v1 Reserved journals must remain restart-readable after v2 introduction");
}

void TestIdsAndRootScopeAreStrict() {
    TempSandbox sandbox;
    const fs::path root = sandbox.Child("strict-store");

    CheckStoreError("InvalidStoreRoot", [&] {
        DualHardwarePairJournalStore relative("relative-store");
        (void)relative;
    }, "a relative journal root must be rejected");

    const fs::path traversal =
        sandbox.Child("traversal-anchor") / ".." / "outside-store";
    CheckStoreError("InvalidStoreRoot", [&] {
        DualHardwarePairJournalStore escaped(traversal);
        (void)escaped;
    }, "a root containing traversal components must be rejected");

    CheckStoreError("InvalidStoreRoot", [&] {
        DualHardwarePairJournalStore unc(
            fs::path(L"\\\\server\\share\\pair-store"));
        (void)unc;
    }, "a UNC root must be rejected without network access");

    DualHardwarePairJournalStore store(root);
    CheckStoreError("InvalidTransactionId", [&] {
        (void)store.Reserve("../outside");
    }, "transaction traversal must be rejected before filesystem access");
    CheckStoreError("InvalidTransactionId", [&] {
        (void)store.Query("0123456789abcdef0123456789abcdeg");
    }, "a non-hex transaction ID must be rejected");
    CheckStoreError("InvalidTransactionId", [&] {
        (void)store.Query("0123456789abcdef");
    }, "a short transaction ID must be rejected");
    Check(!fs::exists(root),
        "invalid transaction IDs must not create the journal root");
}

void TestMalformedOversizedAndNonRegularJournalsFailClosed() {
    TempSandbox sandbox;
    const std::string transaction_id = "11111111111111111111111111111111";

    const fs::path malformed_root = sandbox.Child("malformed");
    PrepareJournal(malformed_root, "{\"state\":\"Reserved\"}");
    DualHardwarePairJournalStore malformed(malformed_root);
    CheckStoreError("JournalInvalid", [&] {
        (void)malformed.Query(transaction_id);
    }, "a malformed journal must fail closed");

    const fs::path unknown_root = sandbox.Child("unknown-field");
    PrepareJournal(
        unknown_root,
        "{\"schemaVersion\":\"a0.camera-agent.hardware-dual.pair-journal.v1\","
        "\"cameraMode\":\"DualCamera\",\"transactionId\":\"" +
            transaction_id +
        "\",\"state\":\"Reserved\",\"automaticRetryCount\":0,"
        "\"unexpected\":true}");
    DualHardwarePairJournalStore unknown(unknown_root);
    CheckStoreError("JournalInvalid", [&] {
        (void)unknown.Query(transaction_id);
    }, "a journal with an unknown field must fail closed");

    const fs::path oversized_root = sandbox.Child("oversized");
    PrepareJournal(oversized_root, std::string(64U * 1024U + 1U, 'x'));
    DualHardwarePairJournalStore oversized(oversized_root);
    CheckStoreError("JournalInvalid", [&] {
        (void)oversized.Query(transaction_id);
    }, "an oversized journal must fail before unbounded parsing");

    const fs::path non_regular_root = sandbox.Child("non-regular");
    fs::create_directories(JournalPath(non_regular_root));
    DualHardwarePairJournalStore non_regular(non_regular_root);
    CheckStoreError("JournalInvalid", [&] {
        (void)non_regular.Query(transaction_id);
    }, "a non-regular journal path must fail closed");

    const fs::path unexpected_root = sandbox.Child("unexpected-entry");
    fs::create_directories(unexpected_root);
    WriteText(unexpected_root / "foreign.txt", "not a pair journal");
    DualHardwarePairJournalStore unexpected(unexpected_root);
    CheckStoreError("StoreScopeInvalid", [&] {
        (void)unexpected.Query(transaction_id);
    }, "an ambiguous store root must fail closed");
}

#pragma pack(push, 1)
struct MountPointReparseBufferHeader final {
    DWORD reparse_tag;
    WORD reparse_data_length;
    WORD reserved;
    WORD substitute_name_offset;
    WORD substitute_name_length;
    WORD print_name_offset;
    WORD print_name_length;
};
#pragma pack(pop)

static_assert(sizeof(MountPointReparseBufferHeader) == 16U);

class ScopedDirectoryJunction final {
public:
    ScopedDirectoryJunction(
        const fs::path& link,
        const fs::path& target,
        std::string_view message)
        : link_(link) {
        if (!CreateDirectoryW(link_.c_str(), nullptr)) {
            throw std::runtime_error(
                std::string(message) + "; create-directory win32=" +
                std::to_string(GetLastError()));
        }

        const HANDLE handle = CreateFileW(
            link_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            (void)RemoveDirectoryW(link_.c_str());
            throw std::runtime_error(
                std::string(message) + "; open-directory win32=" +
                std::to_string(error));
        }

        const std::wstring print_name =
            fs::absolute(target).lexically_normal().native();
        const std::wstring substitute_name = L"\\??\\" + print_name;
        const std::size_t substitute_bytes =
            substitute_name.size() * sizeof(wchar_t);
        const std::size_t print_bytes = print_name.size() * sizeof(wchar_t);
        const std::size_t path_bytes = substitute_bytes + sizeof(wchar_t) +
            print_bytes + sizeof(wchar_t);
        const std::size_t reparse_data_length = 8U + path_bytes;
        const std::size_t total_bytes = 8U + reparse_data_length;
        if (reparse_data_length > MAXIMUM_REPARSE_DATA_BUFFER_SIZE ||
            substitute_bytes > MAXWORD || print_bytes > MAXWORD ||
            substitute_bytes + sizeof(wchar_t) > MAXWORD) {
            (void)CloseHandle(handle);
            (void)RemoveDirectoryW(link_.c_str());
            throw std::runtime_error(std::string(message) + "; path too long");
        }

        std::vector<unsigned char> buffer(total_bytes, 0U);
        auto* header = reinterpret_cast<MountPointReparseBufferHeader*>(
            buffer.data());
        header->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
        header->reparse_data_length =
            static_cast<WORD>(reparse_data_length);
        header->substitute_name_offset = 0;
        header->substitute_name_length = static_cast<WORD>(substitute_bytes);
        header->print_name_offset =
            static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
        header->print_name_length = static_cast<WORD>(print_bytes);

        auto* path_buffer = reinterpret_cast<wchar_t*>(
            buffer.data() + sizeof(MountPointReparseBufferHeader));
        CopyMemory(path_buffer, substitute_name.data(), substitute_bytes);
        auto* print_buffer = reinterpret_cast<wchar_t*>(
            buffer.data() + sizeof(MountPointReparseBufferHeader) +
            header->print_name_offset);
        CopyMemory(print_buffer, print_name.data(), print_bytes);

        DWORD bytes_returned = 0;
        const BOOL created = DeviceIoControl(
            handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
            static_cast<DWORD>(buffer.size()), nullptr, 0,
            &bytes_returned, nullptr);
        const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
        (void)CloseHandle(handle);
        if (!created) {
            (void)RemoveDirectoryW(link_.c_str());
            throw std::runtime_error(
                std::string(message) + "; set-reparse win32=" +
                std::to_string(create_error));
        }

        const DWORD attributes = GetFileAttributesW(link_.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            (void)RemoveDirectoryW(link_.c_str());
            throw std::runtime_error(
                std::string(message) + "; reparse attribute missing");
        }
        active_ = true;
    }

    ~ScopedDirectoryJunction() {
        if (active_) (void)RemoveDirectoryW(link_.c_str());
    }

    ScopedDirectoryJunction(const ScopedDirectoryJunction&) = delete;
    ScopedDirectoryJunction& operator=(const ScopedDirectoryJunction&) = delete;

private:
    fs::path link_;
    bool active_ = false;
};

void TestReparsePointsAreRejected() {
    TempSandbox sandbox;
    const std::string transaction_id = "22222222222222222222222222222222";

    const fs::path outside_root = sandbox.Child("outside-root-target");
    fs::create_directory(outside_root);
    const fs::path outside_sentinel = outside_root / "sentinel.txt";
    const std::string sentinel_contents = "outside-root-unchanged";
    WriteText(outside_sentinel, sentinel_contents);
    const fs::path root_link = sandbox.Child("root-link");
    ScopedDirectoryJunction root_junction(
        root_link, outside_root,
        "root reparse test could not be established safely");
    Check(
        (GetFileAttributesW(root_link.c_str()) &
            FILE_ATTRIBUTE_REPARSE_POINT) != 0,
        "root fixture must be an actual reparse point");
    CheckStoreError("InvalidStoreRoot", [&] {
        DualHardwarePairJournalStore linked_root(root_link);
        (void)linked_root;
    }, "a reparse-point store root must be rejected");
    Check(ReadText(outside_sentinel) == sentinel_contents,
        "root reparse rejection must not alter the outside target");

    const fs::path journal_root = sandbox.Child("journal-link-store");
    fs::create_directories(journal_root);
    const fs::path outside_active = sandbox.Child("outside-active-target");
    fs::create_directory(outside_active);
    const fs::path outside_journal = outside_active / "pair-journal.json";
    const std::string journal_contents =
        "{\"schemaVersion\":\"a0.camera-agent.hardware-dual.pair-journal.v1\","
        "\"cameraMode\":\"DualCamera\",\"transactionId\":\"" +
            transaction_id +
        "\",\"state\":\"Reserved\",\"automaticRetryCount\":0}";
    WriteText(outside_journal, journal_contents);
    ScopedDirectoryJunction active_junction(
        journal_root / "active", outside_active,
        "active-directory reparse test could not be established safely");
    Check(
        (GetFileAttributesW((journal_root / "active").c_str()) &
            FILE_ATTRIBUTE_REPARSE_POINT) != 0,
        "active fixture must be an actual reparse point");
    DualHardwarePairJournalStore linked_journal(journal_root);
    CheckStoreError("StoreScopeInvalid", [&] {
        (void)linked_journal.Query(transaction_id);
    }, "a reparse-point active directory must be rejected without following it");
    Check(ReadText(outside_journal) == journal_contents,
        "active reparse rejection must not alter the outside target");

    const fs::path terminal_link_root = sandbox.Child("terminal-link-store");
    fs::create_directories(terminal_link_root);
    const fs::path outside_terminal = sandbox.Child("outside-terminal-target");
    fs::create_directory(outside_terminal);
    ScopedDirectoryJunction terminal_junction(
        terminal_link_root / "terminal", outside_terminal,
        "terminal-directory reparse test could not be established safely");
    DualHardwarePairJournalStore linked_terminal(terminal_link_root);
    CheckStoreError("StoreScopeInvalid", [&] {
        (void)linked_terminal.Query(transaction_id);
    }, "a reparse-point terminal directory must be rejected");

    const fs::path terminal_entry_root = sandbox.Child("terminal-entry-store");
    fs::create_directories(terminal_entry_root / "terminal");
    const fs::path outside_entry = sandbox.Child("outside-terminal-entry");
    fs::create_directory(outside_entry);
    ScopedDirectoryJunction terminal_entry_junction(
        terminal_entry_root / "terminal" / (transaction_id + ".json"),
        outside_entry,
        "terminal-entry reparse test could not be established safely");
    DualHardwarePairJournalStore linked_terminal_entry(terminal_entry_root);
    CheckStoreError("JournalInvalid", [&] {
        (void)linked_terminal_entry.Query(transaction_id);
    }, "a reparse-point terminal journal entry must be rejected");

    for (const std::string_view foreign_name : {std::string_view{"foreign.txt"},
             std::string_view{"22222222222222222222222222222222.json.partial"}}) {
        const fs::path foreign_root = sandbox.Child(
            std::string("terminal-") + std::string(foreign_name.substr(0, 7)));
        fs::create_directories(foreign_root / "terminal");
        WriteText(foreign_root / "terminal" / std::string(foreign_name), "poison");
        DualHardwarePairJournalStore foreign_store(foreign_root);
        CheckStoreError("JournalInvalid", [&] {
            (void)foreign_store.Query(transaction_id);
        }, "foreign or partial terminal entries must fail closed");
    }
}

} // namespace

int main() {
    try {
        TestReserveAndRestartQuery();
        TestDispatchAndTerminalTransitionsAreDurable();
        TestCloseReservedBeforeDispatchIsDurableAndExact();
        TestLegacyV1ReservedJournalRemainsReadable();
        TestIdsAndRootScopeAreStrict();
        TestMalformedOversizedAndNonRegularJournalsFailClosed();
        TestReparsePointsAreRejected();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: Dual pair journal store test threw: "
                  << error.what() << '\n';
    }
    if (failures != 0) {
        std::cerr << failures << " Dual pair journal store test(s) failed\n";
        return 1;
    }
    std::cout << "Dual hardware pair journal store contracts passed\n";
    return 0;
}
