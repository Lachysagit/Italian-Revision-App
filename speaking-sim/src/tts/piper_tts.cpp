#include "sim/tts/piper_tts.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef SIM_HAVE_PIPER
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace sim {

namespace {

// Piper writes a RIFF/WAV stream: a 44-byte canonical header, then int16 PCM.
// Reinterpret the PCM tail as samples. Assumes the standard 44-byte header,
// which piper's WAV writer emits; a non-standard header would desync this.
std::vector<std::int16_t> pcm_from_wav(const std::string& wav) {
    constexpr std::size_t kHeaderBytes = 44;
    if (wav.size() <= kHeaderBytes) {
        return {};
    }
    const std::size_t pcm_bytes = wav.size() - kHeaderBytes;
    std::vector<std::int16_t> samples(pcm_bytes / sizeof(std::int16_t));
    std::memcpy(samples.data(), wav.data() + kHeaderBytes, samples.size() * sizeof(std::int16_t));
    return samples;
}

PiperTTS::PiperTTS() = default;

std::vector<std::int16_t> PiperTTS::synthesize(const std::string& text) {
#ifdef SIM_HAVE_PIPER
    const std::string wav = run_piper(text);
    return pcm_from_wav(wav);
#else
    // existing stub body, verbatim
#endif
}

namespace {

#if defined(SIM_HAVE_PIPER) && !defined(_WIN32)
std::string run_piper(const std::string& text) {
    int in_pipe[2];   // parent writes text  -> child stdin
    int out_pipe[2];  // child writes WAV    -> parent reads
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        throw std::runtime_error("piper: pipe() failed");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("piper: fork() failed");
    }

    if (pid == 0) {
        // Child. Rewire stdin/stdout to the pipes, then exec piper.
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        // Close every original descriptor; the dup2 copies are what matter now.
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(SIM_PIPER_EXECUTABLE, SIM_PIPER_EXECUTABLE,
              "--output_raw", static_cast<char*>(nullptr));
        _exit(127);  // exec only returns on failure
    }

    // Parent. Close the child's ends so reads see EOF when piper exits.
    close(in_pipe[0]);
    close(out_pipe[1]);

    // Write the whole text, then close stdin so piper knows input is done.
    if (::write(in_pipe[1], text.data(), text.size()) < 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        throw std::runtime_error("piper: write to stdin failed");
    }
    close(in_pipe[1]);

    // Drain stdout to EOF.
    std::string wav;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(out_pipe[0], buf, sizeof(buf))) > 0) {
        wav.append(buf, static_cast<std::size_t>(n));
    }
    close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);  // reap the child; avoids a zombie
    return wav;
}
#endif

}  // namespace

namespace {

#if defined(SIM_HAVE_PIPER) && defined(_WIN32)
std::string run_piper(const std::string& text) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;  // child inherits the pipe ends

    HANDLE in_read = nullptr, in_write = nullptr;
    HANDLE out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0) ||
        !CreatePipe(&out_read, &out_write, &sa, 0)) {
        throw std::runtime_error("piper: CreatePipe failed");
    }
    // The parent's own ends must NOT be inheritable, or the child holds a copy
    // and our read never sees EOF (the Windows analogue of the POSIX close).
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::string cmd = std::string("\"") + SIM_PIPER_EXECUTABLE + "\" --output_raw";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("piper: CreateProcess failed");
    }
    // Close the child's ends in the parent — same EOF reasoning as POSIX.
    CloseHandle(in_read);
    CloseHandle(out_write);

    DWORD written = 0;
    WriteFile(in_write, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(in_write);  // signals end-of-input to piper

    std::string wav;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(out_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
        wav.append(buf, n);
    }
    CloseHandle(out_read);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return wav;
}
#endif

}  // namespace

}  // namespace