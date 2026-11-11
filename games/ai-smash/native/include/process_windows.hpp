#pragma once
// Windows subprocess transport for trusted local bots; not a security sandbox.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace smash {
using Clock = std::chrono::steady_clock;
struct Answer {
  Clock::time_point arrived;
  std::string text;
  bool too_long = false;
  bool eof = false;
};
// Job Object limits for one child process tree; 0 means not limited. They bound
// resources only: a job gives no file-system or network isolation.
struct WorkerLimits {
  unsigned long long memory_bytes = 0; // committed memory of the whole job
  unsigned long processes = 0;         // active processes in the job
  unsigned long cpu_rate = 0;          // hard cap, 1/100 % of all processors
};
inline std::wstring wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              int(s.size()), nullptr, 0);
  if (!n)
    throw std::runtime_error("invalid UTF-8 in process command");
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()),
                      w.data(), n);
  return w;
}
inline std::string utf8(const std::wstring &s) {
  if (s.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                              int(s.size()), nullptr, 0, nullptr, nullptr);
  if (!n)
    throw std::runtime_error("invalid Unicode in process command");
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}
inline std::wstring quote(const std::wstring &s) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t c : s) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'\"') {
      result.append(slashes * 2 + 1, L'\\');
      result += c;
    } else {
      result.append(slashes, L'\\');
      result += c;
    }
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  return result + L'\"';
}
class Process {
  HANDLE process = nullptr, job = nullptr, in = nullptr, out = nullptr,
         err = nullptr;
  std::mutex read_mutex, write_mutex;
  std::condition_variable ready, writable;
  std::deque<Answer> inbox;
  std::deque<std::string> outbox;
  bool ended = false, stopping = false;
  Clock::time_point ended_at;
  std::thread reader, writer, logger;
  static void close_handle(HANDLE &h) {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
    h = nullptr;
  }
  void arrive(Answer a) {
    std::lock_guard<std::mutex> lock(read_mutex);
    if (inbox.size() >= 64) {
      inbox.pop_front();
      ++dropped_lines;
    }
    inbox.push_back(std::move(a));
    ready.notify_all();
  }
  void read_loop() {
    char bytes[65536];
    std::string line;
    bool skip = false;
    DWORD count = 0;
    while (ReadFile(out, bytes, sizeof bytes, &count, nullptr) && count) {
      auto now = Clock::now();
      for (DWORD i = 0; i < count; ++i) {
        char c = bytes[i];
        if (c == '\n') {
          if (!skip) {
            if (!line.empty() && line.back() == '\r')
              line.pop_back();
            arrive({now, line});
          }
          line.clear();
          skip = false;
        } else if (!skip) {
          line += c;
          if (line.size() > 65536) {
            line.clear();
            skip = true;
            ++long_lines;
            arrive({now, "", true});
          }
        }
      }
    }
    if (!line.empty())
      arrive({Clock::now(), line});
    std::lock_guard<std::mutex> lock(read_mutex);
    ended = true;
    ended_at = Clock::now();
    ready.notify_all();
  }
  void write_loop() {
    for (;;) {
      std::string text;
      {
        std::unique_lock<std::mutex> lock(write_mutex);
        writable.wait(lock, [&] { return stopping || !outbox.empty(); });
        if (stopping)
          break;
        text = std::move(outbox.front());
        outbox.pop_front();
      }
      size_t offset = 0;
      while (offset < text.size()) {
        DWORD n = 0;
        if (!WriteFile(in, text.data() + offset, DWORD(text.size() - offset),
                       &n, nullptr) ||
            !n) {
          broken = true;
          return;
        }
        offset += n;
      }
    }
  }
  void log_loop() {
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(err, buf, sizeof buf, &n, nullptr) && n)
      stderr_bytes += n;
  }

public:
  std::atomic<size_t> dropped_lines{0}, long_lines{0}, unsent{0},
      stderr_bytes{0};
  std::atomic<bool> broken{false};
  explicit Process(const std::vector<std::string> &command,
                   const WorkerLimits &worker = {}) {
    if (command.empty())
      throw std::runtime_error("empty bot command");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE child_in = nullptr, child_out = nullptr, child_err = nullptr;
    SIZE_T attr_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    HANDLE thread = nullptr;
    bool attrs_initialized = false;
    try {
      if (!CreatePipe(&child_in, &in, &sa, 0) ||
          !CreatePipe(&out, &child_out, &sa, 0) ||
          !CreatePipe(&err, &child_err, &sa, 0))
        throw std::runtime_error("pipe failed");
      if (!SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0) ||
          !SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0) ||
          !SetHandleInformation(err, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("pipe inheritance failed");
      job = CreateJobObjectW(nullptr, nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      // DIE_ON_UNHANDLED_EXCEPTION: a crashing bot must not wait on an error dialog.
      limits.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
          JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
      if (worker.memory_bytes) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = SIZE_T(worker.memory_bytes);
      }
      if (worker.processes) {
        limits.BasicLimitInformation.LimitFlags |=
            JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = worker.processes;
      }
      if (!job ||
          !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &limits, sizeof limits))
        throw std::runtime_error("job creation failed");
      if (worker.cpu_rate) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
        rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
                            JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        rate.CpuRate = worker.cpu_rate;
        if (!SetInformationJobObject(job, JobObjectCpuRateControlInformation,
                                     &rate, sizeof rate))
          throw std::runtime_error("job CPU limit failed: " +
                                   std::to_string(GetLastError()));
      }
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
      attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
          HeapAlloc(GetProcessHeap(), 0, attr_size));
      if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size))
        throw std::runtime_error("process attributes failed");
      attrs_initialized = true;
      HANDLE handles[] = {child_in, child_out, child_err};
      if (!UpdateProcThreadAttribute(attrs, 0,
                                     PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                     sizeof handles, nullptr, nullptr))
        throw std::runtime_error("process handles failed");
      STARTUPINFOEXW si{};
      si.StartupInfo.cb = sizeof si;
      si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
      si.StartupInfo.hStdInput = child_in;
      si.StartupInfo.hStdOutput = child_out;
      si.StartupInfo.hStdError = child_err;
      si.lpAttributeList = attrs;
      std::wstring cmd;
      for (const auto &s : command) {
        if (!cmd.empty())
          cmd += L' ';
        cmd += quote(wide(s));
      }
      PROCESS_INFORMATION pi{};
      auto executable = wide(command[0]);
      if (!CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr,
                          TRUE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED |
                              EXTENDED_STARTUPINFO_PRESENT,
                          nullptr, nullptr, &si.StartupInfo, &pi))
        throw std::runtime_error("CreateProcess failed: " +
                                 std::to_string(GetLastError()));
      process = pi.hProcess;
      thread = pi.hThread;
      if (!AssignProcessToJobObject(job, process))
        throw std::runtime_error("process job assignment failed: " +
                                 std::to_string(GetLastError()));
      if (ResumeThread(thread) == DWORD(-1))
        throw std::runtime_error("process resume failed");
      close_handle(thread);
      close_handle(child_in);
      close_handle(child_out);
      close_handle(child_err);
      DeleteProcThreadAttributeList(attrs);
      attrs_initialized = false;
      HeapFree(GetProcessHeap(), 0, attrs);
      attrs = nullptr;
      reader = std::thread([this] { read_loop(); });
      writer = std::thread([this] { write_loop(); });
      logger = std::thread([this] { log_loop(); });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(write_mutex);
        stopping = true;
        writable.notify_all();
      }
      if (job)
        TerminateJobObject(job, 1);
      if (process)
        TerminateProcess(process, 1);
      close_handle(thread);
      close_handle(child_in);
      close_handle(child_out);
      close_handle(child_err);
      if (reader.joinable())
        reader.join();
      if (writer.joinable())
        writer.join();
      if (logger.joinable())
        logger.join();
      if (attrs) {
        if (attrs_initialized)
          DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
      }
      close_handle(job);
      close_handle(process);
      close_handle(in);
      close_handle(out);
      close_handle(err);
      throw;
    }
  }
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;
  bool send(std::string text) {
    if (broken)
      return false;
    std::lock_guard<std::mutex> lock(write_mutex);
    if (outbox.size() >= 2) {
      ++unsent;
      return false;
    }
    outbox.push_back(std::move(text));
    writable.notify_one();
    return true;
  }
  std::optional<Answer> receive(Clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(read_mutex);
    ready.wait_until(lock, deadline, [&] { return ended || !inbox.empty(); });
    if (!inbox.empty()) {
      auto a = std::move(inbox.front());
      inbox.pop_front();
      return a;
    }
    if (ended)
      return Answer{ended_at, "", false, true};
    return std::nullopt;
  }
  bool exited() {
    std::lock_guard<std::mutex> lock(read_mutex);
    return ended;
  }
  ~Process() {
    {
      std::lock_guard<std::mutex> lock(write_mutex);
      stopping = true;
      outbox.clear();
      writable.notify_all();
    }
    if (job)
      TerminateJobObject(job, 1);
    if (process)
      WaitForSingleObject(process, INFINITE);
    if (writer.joinable())
      writer.join();
    if (reader.joinable())
      reader.join();
    if (logger.joinable())
      logger.join();
    close_handle(in);
    close_handle(out);
    close_handle(err);
    close_handle(process);
    close_handle(job);
  }
};
} // namespace smash
