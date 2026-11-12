#include "common.h"
#include "cpu_report.h"
#include "elevation.h"
#include "etw.h"
#include "ipc.h"
#include "state.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
int g_failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        ++g_failures;
    }
}

void TestQuoteArgument() {
    Check(wperf::QuoteCommandLineArgument(L"plain") == L"plain", L"plain quoting");
    Check(wperf::QuoteCommandLineArgument(L"hello world") == L"\"hello world\"",
          L"space quoting");
    Check(wperf::QuoteCommandLineArgument(L"a b\\") == L"\"a b\\\\\"",
          L"trailing backslash quoting");
}

void TestStateRoundTrip() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"wperf-tests-" + wperf::NewId());
    std::wstring error;
    Check(wperf::EnsureDirectory(directory, &error), L"create state directory");

    wperf::RunState state;
    state.correlation_id = wperf::NewId();
    state.phase = L"CAPTURING";
    state.session_name = L"wperf-test-session";
    state.trace_path = L"C:\\path with space\\trace.etl";
    state.worker_pid = 20;

    wperf::StateStore store(directory / (state.correlation_id + L".json"));
    Check(store.Write(state, &error), L"write state file");
    const auto states = wperf::StateStore::ReadAll(directory);
    Check(states.size() == 1, L"read one state file");
    if (!states.empty()) {
        Check(states[0].phase == L"CAPTURING", L"state phase round trip");
        Check(states[0].worker_pid == 20, L"state pid round trip");
        Check(states[0].trace_path == state.trace_path, L"state path round trip");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void TestEtwQuery() {
    std::wstring error;
    const auto sources = wperf::QueryProfileSources(&error);
    Check(error.empty(), L"profile source query");
    Check(!sources.empty(), L"profile source list");
}

void TestCpuReportSynthetic() {
    wperf::TraceData trace;
    trace.quality.samples_total = 1;
    trace.pointer_size = 8;
    wperf::ProcessRecord process;
    process.pid = 42;
    process.image_name = L"app.exe";
    trace.processes.push_back(process);
    wperf::ImageRecord image;
    image.pid = 42;
    image.base = 0x1000;
    image.size = 0x1000;
    image.path = L"app.exe";
    trace.images.push_back(image);
    wperf::SampleRecord sample;
    sample.pid = 42;
    sample.tid = 7;
    sample.cpu = 0;
    sample.instruction_pointer = 0x1010;
    sample.stack = {0x1010, 0x1000};
    trace.samples.push_back(sample);

    wperf::Symbolizer symbolizer;
    wperf::CpuReportOptions options;
    std::wostringstream output;
    wperf::WriteCpuReport(trace, symbolizer, options, output);
    Check(output.str().find(L"app.exe") != std::wstring::npos,
          L"CPU report resolves synthetic image");
}
void TestWorkerIpcFailurePath() {
    const std::wstring correlation_id = wperf::NewId();
    const std::wstring nonce = wperf::NewId();
    const std::wstring session_name = L"wperf-test-" + correlation_id;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"wperf-ipc-" + correlation_id);
    std::wstring error;
    Check(wperf::EnsureDirectory(directory, &error), L"create IPC directory");

    wperf::PipeServer pipe;
    Check(pipe.Create(correlation_id, &error), L"create pipe");

    const std::filesystem::path executable =
        std::filesystem::path(wperf::GetExecutablePath()).parent_path() / L"wperf.exe";
    std::vector<std::wstring> arguments{
        L"--internal-elevated-worker",
        L"--pipe", wperf::MakePipePath(correlation_id),
        L"--nonce", nonce,
        L"--session", session_name,
        L"--output", (directory / L"trace.etl").wstring(),
        L"--log", (directory / L"worker.jsonl").wstring(),
        L"--guard-log", (directory / L"guard.jsonl").wstring(),
        L"--state", (directory / L"state.json").wstring(),
        L"--correlation", correlation_id,
        L"--duration-ms", L"1000",
    };

    wperf::ProcessLaunchResult launch;
    Check(wperf::LaunchWorkerProcess(executable.wstring(), arguments, false, launch, &error),
          L"launch worker");
    if (launch.process == nullptr) {
        return;
    }
    Check(pipe.WaitForClient(10000, &error), L"worker connects");
    std::wstring line;
    Check(pipe.ReceiveLine(line, 10000, &error), L"receive hello");
    Check(line == L"HELLO " + nonce + L" " + std::to_wstring(launch.process_id),
          L"validate hello");
    Check(pipe.SendLine(L"START", &error), L"send start");

    bool finished = false;
    int result = -1;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (!pipe.HasData()) {
            Sleep(20);
            continue;
        }
        if (!pipe.ReceiveLine(line, 2000, &error)) {
            break;
        }
        if (line.rfind(L"FINISHED ", 0) == 0) {
            result = _wtoi(line.c_str() + 9);
            finished = true;
            break;
        }
    }
    Check(finished, L"worker finished");
    Check(result == 0 || result == 4, L"expected worker result");
    DWORD exit_code = 0;
    Check(wperf::WaitForProcessExit(launch.process, 10000, exit_code), L"worker process exit");
    CloseHandle(launch.process);
    if (result == 0) {
        wperf::StopSessionByName(session_name, nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
}  // namespace

int wmain() {
    TestQuoteArgument();
    TestStateRoundTrip();
    TestEtwQuery();
    TestCpuReportSynthetic();
    TestWorkerIpcFailurePath();
    if (g_failures == 0) {
        std::wcout << L"wperf unit tests: PASS\n";
        return 0;
    }
    std::wcerr << L"wperf unit tests: " << g_failures << L" failure(s)\n";
    return 1;
}
