// SPDX-License-Identifier: Apache-2.0
#include "process.hpp"
#include "windows.hpp"
#include <array>
#include <algorithm>

namespace pocketvr::win {
std::wstring quote_argument(std::wstring_view value) {
    if (value.find(L'\0') != std::wstring_view::npos)
        throw std::invalid_argument("Embedded NUL in process argument");
    std::wstring result = L"\"";
    size_t slashes{};
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
ProcessResult run_process(const std::filesystem::path &executable,
    std::span<const std::wstring> arguments, std::stop_token stop,
    std::chrono::milliseconds timeout, size_t output_limit) {
    if (stop.stop_requested()) throw std::runtime_error("Operation cancelled");
    if (!executable.is_absolute() || timeout <= std::chrono::milliseconds::zero() ||
        output_limit > 1024 * 1024 || !output_limit)
        throw std::invalid_argument("Invalid bounded process request");
    auto command = quote_argument(executable.native());
    for (const auto &arg : arguments) { command += L' '; command += quote_argument(arg); }
    if (command.size() >= 32767) throw std::invalid_argument("Process command too long");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_raw{}, write_raw{};
    if (!CreatePipe(&read_raw, &write_raw, &security, 0))
        throw std::runtime_error("Cannot capture helper output");
    Handle reader(read_raw), writer(write_raw);
    if (!SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("Cannot protect helper capture handle");
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input) throw std::runtime_error("Cannot create helper standard input");
    SIZE_T attribute_bytes{};
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<uint8_t> attributes(attribute_bytes);
    auto *list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_bytes))
        throw std::runtime_error("Cannot initialize helper process attributes");
    struct AttributeCleanup {
        PPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(list); }
    } cleanup{list};
    const std::array inherited{writer.get(), input.get()};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE *>(inherited.data()), sizeof(inherited), nullptr, nullptr))
        throw std::runtime_error("Cannot restrict helper inherited handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.get();
    startup.lpAttributeList = list;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
            &startup.StartupInfo, &info))
        throw std::runtime_error("Cannot run the selected helper executable");
    Handle process(info.hProcess), thread(info.hThread);
    writer.reset(); input.reset();
    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    try {
        for (;;) {
            if (stop.stop_requested()) throw std::runtime_error("Operation cancelled");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Helper timed out; cancel and retry");
            DWORD available{};
            if (!PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE)
                    throw std::runtime_error("Cannot read helper output");
                available = 0;
            }
            if (available) {
                std::array<char, 4096> buffer{}; DWORD count{};
                if (!ReadFile(reader.get(), buffer.data(),
                        std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr))
                    throw std::runtime_error("Cannot read helper output");
                if (result.output.size() + count > output_limit)
                    throw std::runtime_error("Helper output exceeds the diagnostic limit");
                result.output.append(buffer.data(), count);
                continue;
            }
            const auto wait = WaitForSingleObject(process.get(), 10);
            if (wait == WAIT_OBJECT_0) {
                // Drain bytes written immediately before process exit. Do not wait
                // for pipe EOF: the shared adb server can outlive its client.
                if (PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr) && available)
                    continue;
                DWORD code{};
                if (!GetExitCodeProcess(process.get(), &code))
                    throw std::runtime_error("Cannot read helper exit status");
                result.exit_code = code;
                return result;
            }
            if (wait == WAIT_FAILED) throw std::runtime_error("Cannot wait for helper");
        }
    } catch (...) {
        // Terminate only this command process. Never kill/restart the shared adb
        // server: doing so would disturb other tools and reverse mappings.
        (void)TerminateProcess(process.get(), 1);
        (void)WaitForSingleObject(process.get(), 1000);
        throw;
    }
}
} // namespace pocketvr::win
