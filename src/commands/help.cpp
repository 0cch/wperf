#include "commands/help.h"

#include "common.h"
#include "commands/util.h"

#include <iostream>
#include <string_view>

namespace wperf::commands {
namespace {

constexpr std::wstring_view kRecordHelp = LR"(Usage: wperf record [options] [--] [command [arguments...]]

Capture ETW profile samples or trace events. This is the wperf equivalent of
`perf record`; system ETW call stacks are enabled by default.

Options:
  --elevate                 Start the hidden elevated capture worker.
  -F, --freq HZ             Sampling frequency, 1..100000 (default: 1000).
  -g, --call-graph          Capture call stacks (default for system ETW).
  --no-call-graph           Disable system ETW stack tracing.
  -p, --pid PID             Attach to an existing process.
  -d, --duration MS         Capture duration in milliseconds.
  -o, --output FILE         Write the ETL trace to FILE.
  -e, --event NAME|ID       Select a cpu profile, ETW source name or source ID.
  --profile NAME            cpu, cpu-sched, sched, syscall, vm, io, kernel,
                            all, or pmu (default: cpu).
  --pmc-source ID           Select an ETW PMC profile source.
  -b, --lbr                 Enable LBR call-stack collection when supported.
  --context-registers       Enable context-register collection when supported.
  --provider GUID           Capture a private ETW provider session.
  --session NAME            Use an explicit ETW session name.
  -h, --help                Show this help.

Examples:
  wperf record --elevate -F 999 -g --duration 5000 -o cpu.etl
  wperf record --elevate -g -o child.etl -- .\app.exe --input data.txt

System capture requires administrator rights. `--elevate` keeps the controller
at normal integrity and launches one hidden elevated worker.
)";

constexpr std::wstring_view kReportHelp = LR"(Usage: wperf report -i FILE [options]

Produce a perf-style CPU profile report from an ETL trace.

Options:
  -i, --input FILE          Read FILE.
  -p, --pid PID             Include only PID.
  -t, --tid TID             Include only TID.
  --comm TEXT               Filter by process name substring.
  --dso TEXT                Filter by module/shared-object substring.
  --symbol TEXT             Filter by symbol substring.
  --symbol-path DIR         Add a local PDB search directory.
  --percent-limit N         Hide rows below N percent.
  -s, --sort FIELD          Sort by samples, comm, dso, or symbol.
  -g, --call-graph [MODE]   Text call tree; MODE is callee (default) or caller.
  --folded, --collapsed     Print collapsed stacks: comm;frame;... count.
  --stdio                   Text output (default; perf-compatible).
  --quiet                   Suppress quality diagnostics.
  --verbose                 Print complete quality counters to stderr.
  --csv                     Print CSV rows.
  --json                    Print JSON rows.
  --kind cpu|io             Select a CPU profile or I/O summary.
  -h, --help                Show this help.

Flame graph workflow (SVG rendering is external for now):
  pwsh -File scripts\flamegraph.ps1 -InputFile cpu.etl `
    -SymbolPath .\symbols -OutputFile flamegraph.svg

The helper writes UTF-8/LF folded stacks before invoking flamegraph.pl.

Notes:
  - Quality diagnostics are written to stderr.
  - --folded and --call-graph are mutually exclusive with --csv/--json.
  - Use --pid to isolate one process before rendering a multi-process trace.

Examples:
  wperf report -i cpu.etl --symbol-path .\build\Release --call-graph
  wperf report -i cpu.etl --pid 1234 --folded --percent-limit 0.5 --quiet
)";

constexpr std::wstring_view kScriptHelp = LR"(Usage: wperf script -i FILE [options]

Export one line per sample. This is the closest wperf equivalent of
`perf script`, but its text is wperf-specific. Use `wperf report --folded` for
direct FlameGraph input; no stackcollapse-perf conversion is required.

Options:
  -i, --input FILE          Read FILE.
  -p, --pid PID             Include only PID.
  -F, --fields LIST         Comma-separated fields (default:
                            timestamp,cpu,pid,tid,ip,comm,dso,symbol).
  --symbol-path DIR         Add a local PDB search directory.
  --limit N                 Emit at most N samples; 0 means unlimited.
  --csv                     Use comma-separated output and CSV quoting.
  -h, --help                Show this help.

Fields:
  timestamp, cpu, pid, tid, ip, comm, dso, symbol, stack

Examples:
  wperf script -i cpu.etl --limit 20
  wperf script -i cpu.etl -F timestamp,pid,comm,symbol --csv
)";

constexpr std::wstring_view kTraceHelp = LR"(Usage: wperf trace -i FILE [options]
       wperf trace --live [options]

Print scheduler, syscall, page-fault, I/O, or LBR events.

Options:
  -i, --input FILE          Read an offline ETL trace.
  --live                    Capture and print events in real time.
  -e, --event KIND          sched, syscall, pagefault, io, or lbr.
  -p, --pid PID             Include only PID.
  --max N                   Limit emitted events.
  --duration MS             Live capture duration in milliseconds.
  --json                    Print JSON lines.
  --elevate                 Start the hidden elevated worker for --live.
  -h, --help                Show this help.

Examples:
  wperf trace -i trace.etl -e sched --max 20
  wperf trace --live --elevate -e sched --duration 5000
)";

constexpr std::wstring_view kSchedHelp = LR"(Usage: wperf sched -i FILE [options]

Summarize scheduler activity and wakeup latency.

Options:
  -i, --input FILE          Read FILE.
  -h, --help                Show this help.

Example:
  wperf sched -i sched.etl
)";

constexpr std::wstring_view kStatHelp = LR"(Usage: wperf stat [options] [-p PID | -- command [arguments...]]

Collect process time, cycles and I/O counters.

Options:
  -p, --pid PID             Observe an existing process.
  --duration MS             Observation duration (default: 1000).
  --per-thread              Print per-thread counters.
  --pmu                     Use an ETW PMC profile source.
  -e, --event SOURCE        Select a PMC source name or numeric ID.
  -h, --help                Show this help.

Examples:
  wperf stat -p 1234 --duration 2000
  wperf stat --pmu -e TotalCycles -- .\app.exe
)";

constexpr std::wstring_view kTopHelp = LR"(Usage: wperf top [options]

Print CPU hot functions in fixed intervals.

Options:
  -p, --pid PID             Include only PID.
  -e, --event SOURCE        cpu or an ETW PMC source.
  --interval MS             Refresh interval (default: 1000).
  --iterations N            Number of snapshots; 0 means until Ctrl-C.
  --sort FIELD              Sort by samples, comm, dso, or symbol.
  --symbol-path DIR         Add a local PDB search directory.
  --percent-limit N         Hide rows below N percent (default: 1).
  --elevate                 Start the hidden elevated worker.
  -h, --help                Show this help.

System capture requires administrator rights.
)";

constexpr std::wstring_view kAnnotateHelp = LR"(Usage: wperf annotate -i FILE [options]

Show sampled instruction heat with source lines and optional disassembly.

Options:
  -i, --input FILE          Read FILE.
  --symbol-path DIR         Add a local PDB search directory.
  -p, --pid PID             Include only PID.
  --limit N                 Limit rows (default: 50).
  --disasm                  Decode instruction bytes with Zydis.
  --stdio                   Text output (default; perf-compatible).
  --json                    Print JSON rows.
  --csv                     Print CSV rows.
  -h, --help                Show this help.

Examples:
  wperf annotate -i cpu.etl --symbol-path .\build\Release --limit 20
  wperf annotate -i cpu.etl --disasm --json
)";

constexpr std::wstring_view kDiffHelp = LR"(Usage: wperf diff -1 FIRST.etl -2 SECOND.etl [options]

Compare two CPU profiles. Positive values indicate growth in the second trace.

Options:
  --symbol-path DIR         Add a local PDB search directory.
  --pid PID                 Include only PID.
  --dso TEXT                Filter by module/shared-object substring.
  --calltree                Compare call-tree weights.
  --json                    Print JSON rows.
  --csv                     Print CSV rows.
  -h, --help                Show this help.

Example:
  wperf diff -1 before.etl -2 after.etl --calltree --json
)";

constexpr std::wstring_view kArchiveHelp = LR"(Usage: wperf archive -i FILE -o DIRECTORY [options]

Copy referenced modules and write manifest.json, manifest.csv and manifest.txt.

Options:
  -i, --input FILE          Read FILE.
  -o, --output DIRECTORY    Write the reproducible archive to DIRECTORY.
  -p, --pid PID             Include only modules referenced by PID.
  --symbol-path DIR         Add a local PDB search directory.
  --max-size-mb N           Skip copied files larger than N MiB.
  -h, --help                Show this help.
)";

constexpr std::wstring_view kBuildIdHelp = LR"(Usage: wperf buildid-list -i FILE [options]

List module base, size, process, PE timestamp, checksum and image path.

Options:
  -i, --input FILE          Read FILE.
  -h, --help                Show this help.
)";

constexpr std::wstring_view kListHelp = LR"(Usage: wperf list [providers] [options]

List ETW profile sources and active sessions, or private providers.

Options:
  --max N                   Limit provider output.
  -h, --help                Show this help.

Examples:
  wperf list
  wperf list providers --max 20
)";

constexpr std::wstring_view kDoctorHelp = LR"(Usage: wperf doctor [options]

Check elevation, ETW profile sources, PMC capabilities, state and recent runs.

Options:
  --bundle DIR              Write a diagnostic bundle to DIR.
  --include-dumps           Include .dmp files in the bundle.
  -h, --help                Show this help.
)";

constexpr std::wstring_view kRecoverHelp = LR"(Usage: wperf recover (--session NAME | --all-stale)

Stop stale wperf-owned ETW sessions left by interrupted captures. Requires an
elevated process.

Options:
  --session NAME            Stop one explicit ETW session.
  --all-stale               Stop sessions referenced by stale wperf state.
  -h, --help                Show this help.
)";

constexpr std::wstring_view kTestHelp = LR"(Usage: wperf test

Run a lightweight non-invasive self-test.

)";

constexpr std::wstring_view kVersionHelp = LR"(Usage: wperf version

Print the wperf version.

)";

constexpr std::wstring_view kHelpHelp = LR"(Usage: wperf help [command]

Show general help or command-specific help.

Examples:
  wperf help
  wperf help report
  wperf report --help
)";

}  // namespace

void PrintGeneralHelp() {
    std::wcout
        << L"wperf " << kVersion << L"\n\n"
        << L"Usage: wperf <command> [options]\n"
        << L"       wperf help <command>\n\n"
        << L"Commands:\n"
        << L"  record        Capture ETW samples/events (perf record)\n"
        << L"  report        Produce a CPU profile report (perf report)\n"
        << L"  script        Export samples as text or CSV (perf script)\n"
        << L"  stat          Collect process counters (perf stat)\n"
        << L"  top           Show live CPU hot functions (perf top)\n"
        << L"  trace         Print scheduler/syscall/I/O events (perf trace)\n"
        << L"  annotate      Show instruction/source heat (perf annotate)\n"
        << L"  diff          Compare two profiles (perf diff)\n"
        << L"  sched         Summarize scheduler activity\n"
        << L"  archive       Copy modules and write a manifest\n"
        << L"  buildid-list  List module identities from an ETL\n"
        << L"  list          List profile sources, sessions or providers\n"
        << L"  doctor        Check environment and recent wperf state\n"
        << L"  test          Run a lightweight self-test\n"
        << L"  recover       Stop stale wperf-owned ETW sessions\n"
        << L"  help          Show command-specific help\n"
        << L"  version       Print version\n\n"
        << L"Common perf-style options:\n"
        << L"  -i FILE       Input ETL for report/script/annotate/archive/buildid-list\n"
        << L"  -p PID        Restrict analysis to one process\n"
        << L"  -g            Capture or print call stacks\n"
        << L"  --folded      Emit collapsed stacks for FlameGraph tools (alias: --collapsed)\n"
        << L"  --csv|--json  Machine-readable output (mutually exclusive)\n\n"
        << L"Flame graph:\n"
        << L"  wperf report -i trace.etl --symbol-path DIR --folded > stacks.folded\n"
        << L"  flamegraph.pl stacks.folded > flamegraph.svg\n\n"
        << L"Run \"wperf help <command>\" for complete options and examples.\n";
}

bool PrintCommandHelp(std::wstring_view command) {
    if (command == L"record") {
        std::wcout << kRecordHelp;
    } else if (command == L"report") {
        std::wcout << kReportHelp;
    } else if (command == L"script") {
        std::wcout << kScriptHelp;
    } else if (command == L"trace") {
        std::wcout << kTraceHelp;
    } else if (command == L"sched") {
        std::wcout << kSchedHelp;
    } else if (command == L"stat") {
        std::wcout << kStatHelp;
    } else if (command == L"top") {
        std::wcout << kTopHelp;
    } else if (command == L"annotate") {
        std::wcout << kAnnotateHelp;
    } else if (command == L"diff") {
        std::wcout << kDiffHelp;
    } else if (command == L"archive") {
        std::wcout << kArchiveHelp;
    } else if (command == L"buildid-list") {
        std::wcout << kBuildIdHelp;
    } else if (command == L"list") {
        std::wcout << kListHelp;
    } else if (command == L"doctor") {
        std::wcout << kDoctorHelp;
    } else if (command == L"recover") {
        std::wcout << kRecoverHelp;
    } else if (command == L"test") {
        std::wcout << kTestHelp;
    } else if (command == L"version") {
        std::wcout << kVersionHelp;
    } else if (command == L"help") {
        std::wcout << kHelpHelp;
    } else {
        return false;
    }
    return true;
}

}  // namespace wperf::commands