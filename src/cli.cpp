#include "cli.h"

#include "commands/help.h"
#include "commands/util.h"
#include "common.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace wperf::cli {
using commands::PrintError;
namespace {

struct OptionSpec {
    std::string names;
    std::string value;
    std::string description;
    bool optional_value = false;
    bool join_values = false;
};

struct CommandSpec {
    std::string name;
    std::vector<std::string> aliases;
    std::string description;
    std::string usage;
    bool needs_elevation = false;
    std::vector<OptionSpec> options;
    std::vector<std::string> examples;
    std::vector<std::string> notes;
};

OptionSpec Flag(std::string names, std::string description) {
    return {std::move(names), {}, std::move(description), false, false};
}

OptionSpec Value(std::string names, std::string value, std::string description,
                 bool optional_value = false) {
    return {std::move(names), std::move(value), std::move(description),
            optional_value, false};
}

OptionSpec MultiValue(std::string names, std::string value,
                      std::string description) {
    return {std::move(names), std::move(value), std::move(description),
            false, true};
}

void AddCommand(std::vector<CommandSpec>& catalog, std::string name,
                std::vector<std::string> aliases, std::string description,
                std::string usage, bool needs_elevation,
                std::vector<OptionSpec> options,
                std::vector<std::string> examples = {},
                std::vector<std::string> notes = {}) {
    catalog.push_back(CommandSpec{std::move(name), std::move(aliases),
                                  std::move(description), std::move(usage),
                                  needs_elevation, std::move(options),
                                  std::move(examples), std::move(notes)});
}

std::vector<CommandSpec> BuildCatalog() {
    std::vector<CommandSpec> catalog;

    AddCommand(catalog, "record", {"rec"},
        "Capture ETW profile samples or trace events.",
        "wperf record [options] [--] [command [arguments...]]", true,
        {
            Flag("--elevate", "Start the hidden elevated capture worker."),
            Value("-F,--freq", "HZ", "Sampling frequency, 1..100000 (default: 1000)."),
            Flag("-g,--call-graph", "Capture call stacks (default for system ETW)."),
            Flag("--no-call-graph", "Disable system ETW stack tracing."),
            Value("-p,--pid", "PID", "Attach to an existing process."),
            Value("-d,--duration", "MS", "Capture duration in milliseconds; default is until Ctrl-C."),
            Value("-o,--output", "FILE", "Write the ETL trace to FILE."),
            Flag("--json", "Print one success object with capture identity and artifact metadata."),
            Value("-e,--event", "NAME|ID", "Select a CPU profile, ETW source name or source ID."),
            Value("--profile", "NAME", "cpu, cpu-sched, sched, syscall, vm, io, kernel, all, pmu, dpc, isr, heap, pool, or driver."),
            Value("--pmc-source", "ID", "Select an ETW PMC profile source."),
            Value("--period", "N", "PMC events per interrupt (source-specific count)."),
            Value("--pmc-counters", "IDS", "Counter snapshots on CSwitch (comma-separated IDs)."),
            Flag("--stack-cache", "Emit modern StackWalkRef/Def records when supported."),
            Flag("-b,--lbr", "Enable LBR call-stack collection when supported."),
            Flag("--context-registers", "Enable context-register collection when supported."),
            Value("--provider", "GUID", "Capture a private ETW provider session."),
            Value("--pool-tag", "TAG", "Required with --profile pool: four printable ASCII characters, including spaces."),
            Value("--session", "NAME", "Use an explicit ETW session name."),
        },
        {
            "wperf record --elevate -F 999 -g --duration 5000 -o cpu.etl",
            "wperf record --elevate -g -o child.etl -- .\\app.exe --input data.txt",
        }, {
            "System capture requires administrator rights; --elevate starts one hidden elevated worker.",
            "System ETW call stacks are enabled by default. Use --no-call-graph to disable them.",
            "cpu-sched, sched, and all also request CSwitch/ReadyThread stack tracing.",
            "PMC interrupt sampling and --pmc-counters CSwitch snapshots require separate captures.",
            "heap requires --pid of an existing process; only allocations after attachment are observed.",
            "pool requires --pool-tag to bound high-rate capture; list --pool-tags shows current tag usage.",
            "dpc/isr/heap/pool/driver are opt-in event captures; -F, PMC, LBR and private providers cannot be combined with them.",
        });

    AddCommand(catalog, "report", {"rep"},
        "Produce CPU, I/O, PMC, off-CPU, interrupt, memory, or driver reports from an ETL trace.",
        "wperf report -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Value("-o,--output", "FILE", "Write the selected report format to FILE."),
            Value("-p,--pid", "PID", "Include only PID."),
            Value("-t,--tid", "TID", "Include only TID."),
            Value("--comm", "TEXT", "Filter by process name substring."),
            Value("--dso", "TEXT", "Filter by module/shared-object substring."),
            Value("--symbol", "TEXT", "Filter by symbol substring."),
            MultiValue("--symbol-path", "DIR", "Add a local PDB search directory; repeatable or semicolon-separated."),
            Value("--archive", "DIR", "Resolve unavailable paths through verified modules.tsv."),
            Value("--percent-limit", "N", "Hide rows below N percent."),
            Value("-s,--sort", "FIELD", "Sort by weight, samples, comm, dso, or symbol."),
            Value("--weight", "MODE", "CPU/PMC sample weights: period (default) or samples."),
            Value("--source", "ID", "Select one CPU/PMC sample source or PMC snapshot counter."),
            Value("-g,--call-graph", "MODE", "Text call tree; MODE is callee or caller.", true),
            Flag("--metadata", "With --json, include quality metadata and rows."),
            Flag("--strict-quality", "Fail on lost, dropped, or malformed data."),
            Value("--max-lost-events", "N", "Maximum acceptable ETW lost-event count."),
            Value("--max-missing-stacks", "N", "Maximum acceptable missing-stack count."),
            Flag("--exact-stacks", "Include only exact stack correlations."),
            Flag("--stream", "Two-pass CPU table; omits stacks and retains no samples."),
            Flag("--include-children", "Include descendants of --pid."),
            Value("--cpu", "N", "Select processor index."),
            Value("--time-start", "MS", "Select data after capture-relative time."),
            Value("--time-end", "MS", "Select data before capture-relative time."),
            Value("--scope", "SCOPE", "Select all, user, or kernel address space."),
            Flag("--folded,--collapsed", "Print collapsed stacks with counts."),
            Flag("--flamegraph", "Write a self-contained interactive HTML flame graph."),
            Value("--title", "TEXT", "FlameGraph HTML title."),
            Value("--countname", "NAME", "FlameGraph weight label; defaults to the report unit."),
            Flag("--off-cpu", "Report blocked/ready intervals weighted by wait time."),
            Flag("--off-cpu-include-inferred", "Include end-of-capture truncated intervals."),
            Flag("--stdio", "Text output (default)."),
            Flag("--quiet", "Suppress quality diagnostics."),
            Flag("--verbose", "Print complete quality counters to stderr."),
            Flag("--csv", "Print CSV rows."),
            Flag("--json", "Print JSON rows."),
            Value("--kind", "KIND", "Select cpu, io, pmc, dpc, isr, interrupt, heap, pool, or driver report."),
        },
        {
            "wperf report -i cpu.etl --symbol-path .\\build\\Release --call-graph",
            "wperf report -i cpu.etl --json --metadata",
            "wperf report -i sched.etl --off-cpu --folded --quiet -o offcpu.folded",
            "wperf report -i cpu.etl --symbol-path .\\symbols --flamegraph -o cpu.html",
        }, {
            "Quality diagnostics go to stderr; --quiet suppresses them and --verbose prints every counter.",
            "--folded/--collapsed, --flamegraph and --call-graph are mutually exclusive with each other and with --json/--csv.",
            "--flamegraph emits one self-contained HTML file with inline CSS/JavaScript and no network dependencies.",
            "--folded remains the standard collapsed-stack input for flamegraph.pl or other external SVG/HTML renderers.",
            "CPU overhead and folded output use periods (CPU ns or PMC events); --weight samples uses counts. Unknown periods fall back to counts with a warning.",
            "Mixed sample sources require --source ID. Off-CPU weights remain nanoseconds.",
            "I/O and PMC snapshots support PID/TID/comm/children/CPU/time selection and loss/strict gates; symbol, stack, scope, weight, sort and percent options are rejected.",
            "PMC snapshots also support --csv; I/O supports text/JSON. --max-missing-stacks requires loaded CPU/off-CPU or allocation stacks.",
            "Diagnostic kinds support text/JSON/CSV, symbols, archives, CPU/time selectors and quality gates; graph, stream, weight, source, scope, sort and percent options are rejected.",
            "DPC/ISR/interrupt/pool are system-owned and reject PID/TID/comm/children filters. Heap/driver support these selectors.",
            "Heap/pool time and thread/CPU filters select allocation cohorts; releases through the selected end are always consumed. Observed live bytes do not prove a leak.",
            "-o writes UTF-8 without BOM and LF line endings.",
        });

    AddCommand(catalog, "flamegraph", {"flame"},
        "Convert folded stacks to a self-contained interactive HTML flame graph.",
        "wperf flamegraph -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read folded stacks from FILE."),
            Value("-o,--output", "FILE", "Write HTML to FILE; otherwise print HTML to stdout."),
            Value("--title", "TEXT", "HTML document title."),
            Value("--countname", "NAME", "Weight label (default: samples)."),
        },
        {
            "wperf report -i cpu.etl --folded --quiet -o cpu.folded",
            "wperf flamegraph -i cpu.folded --countname nanoseconds -o cpu.html",
        }, {
            "Input is the standard semicolon-separated folded-stack format.",
            "The output is one self-contained HTML document with inline CSS and JavaScript.",
            "Use --countname nanoseconds, events, or samples to label the weights accurately.",
            "Use report --folded when an external flamegraph.pl SVG workflow is preferred.",
        });

    AddCommand(catalog, "script", {"scr"},
        "Export one line per sample.",
        "wperf script -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Value("-o,--output", "FILE", "Write output to FILE."),
            Value("-p,--pid", "PID", "Include only PID."),
            Value("-F,--fields", "LIST", "Comma-separated fields (default: timestamp,cpu,pid,tid,ip,comm,dso,symbol)."),
            MultiValue("--symbol-path", "DIR", "Add a local PDB search directory; repeatable or semicolon-separated."),
            Value("--archive", "DIR", "Resolve unavailable paths through verified modules.tsv."),
            Value("--limit", "N", "Emit at most N samples; 0 means unlimited."),
            Flag("--csv", "Use comma-separated output and CSV quoting."),
        },
        {
            "wperf script -i cpu.etl --limit 20",
            "wperf script -i cpu.etl -F timestamp,pid,comm,symbol --csv",
        }, {
            "Supported fields: timestamp, cpu, pid, tid, ip, comm, dso, symbol, stack, source, period, and stack_quality.",
            "Use --csv for comma-separated output; otherwise each field is separated by a space.",
        });

    AddCommand(catalog, "trace", {},
        "Print scheduler, syscall, page-fault, I/O, LBR, or context-register events.",
        "wperf trace -i FILE [options] | wperf trace --live [options]", false,
        {
            Value("-i,--input", "FILE", "Read an offline ETL trace."),
            Flag("--live", "Capture and print events in real time."),
            Value("-e,--event", "KIND", "sched, syscall, pagefault, io, lbr, or context (offline)."),
            Value("-p,--pid", "PID", "Include only PID."),
            Value("--max", "N", "Limit emitted events; 0 means unlimited (default)."),
            Value("--duration", "MS", "Live capture duration in milliseconds."),
            Flag("--json", "Print JSON events."),
            Flag("--metadata", "Wrap JSON events with quality and truncation metadata."),
            Flag("--elevate", "Start the hidden elevated worker for --live."),
        },
        {
            "wperf trace -i trace.etl -e sched --max 20",
            "wperf trace --live --elevate -e sched --duration 5000",
            "wperf trace -i trace.etl -e context --json",
        }, {
            "Offline replay reads -i FILE. --live captures events in real time and may require --elevate.",
            "Event kinds are sched, syscall, pagefault, io, lbr, and context.",
        });

    AddCommand(catalog, "sched", {},
        "Summarize scheduler activity and wakeup latency.",
        "wperf sched -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Flag("--json", "Print JSON scheduler rows and quality metadata."),
            Flag("--csv", "Print CSV scheduler rows."),
        },
        {"wperf sched -i sched.etl"});

    AddCommand(catalog, "stat", {},
        "Collect process time, cycles and I/O counters.",
        "wperf stat [options] [-p PID | -- command [arguments...]]", false,
        {
            Value("-p,--pid", "PID", "Observe an existing process."),
            Value("--duration", "MS", "Observation duration (default: 1000)."),
            Flag("--per-thread", "Include observation-delta thread counters in text, JSON, or CSV."),
            Value("--interval", "MS", "Sample interval for repeated process statistics."),
            Flag("--pmu", "Use an ETW PMC profile source."),
            Value("-e,--event", "SOURCE", "Select a PMC source name or numeric ID."),
            Value("--period", "N", "PMC events per interrupt (default: 1000000)."),
            Flag("--json", "Print JSON process statistics."),
            Flag("--csv", "Print CSV process statistics."),
        },
        {
            "wperf stat -p 1234 --duration 2000",
            "wperf stat --pmu -e TotalCycles -- .\\app.exe",
        }, {
            "Without -p, stat launches the command after -- and observes its root process.",
            "--per-thread discovers threads every 50 ms, retains observed exited threads, and reports deltas since observation. Short-lived unobserved threads may be missed.",
            "--per-thread JSON adds threads and threads_complete; CSV uses process/thread row_type records. PMC mode does not support --per-thread.",
            "PMC mode uses an ETW profile source; it does not access PMU MSRs directly.",
        });

    AddCommand(catalog, "top", {},
        "Print CPU hot functions in fixed intervals.",
        "wperf top [options]", true,
        {
            Value("-p,--pid", "PID", "Include only PID."),
            Value("-e,--event", "SOURCE", "cpu or an ETW PMC source."),
            Value("--interval", "MS", "Refresh interval (default: 1000)."),
            Value("--iterations", "N", "Number of snapshots; 0 means until Ctrl-C."),
            Value("--sort", "FIELD", "Sort by samples, comm, dso, or symbol."),
            MultiValue("--symbol-path", "DIR", "Add a local PDB search directory; repeatable or semicolon-separated."),
            Value("--archive", "DIR", "Resolve unavailable paths through verified modules.tsv."),
            Value("--percent-limit", "N", "Hide rows below N percent (default: 1)."),
            Flag("--elevate", "Start the hidden elevated worker."),
        },
        {"wperf top --elevate --interval 1000"}, {
            "System capture requires administrator rights. --elevate keeps the controller at normal integrity.",
        });

    AddCommand(catalog, "annotate", {"ann"},
        "Show sampled instruction heat with source lines and optional disassembly.",
        "wperf annotate -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            MultiValue("--symbol-path", "DIR", "Add a local PDB search directory; repeatable or semicolon-separated."),
            Value("--archive", "DIR", "Resolve unavailable paths through verified modules.tsv."),
            Value("-p,--pid", "PID", "Include only PID."),
            Value("--limit", "N", "Limit rows (default: 50)."),
            Flag("--disasm", "Decode instruction bytes with Zydis."),
            Flag("--stdio", "Text output (default)."),
            Flag("--json", "Print JSON rows."),
            Flag("--csv", "Print CSV rows."),
        },
        {
            "wperf annotate -i cpu.etl --symbol-path .\\build\\Release --limit 20",
            "wperf annotate -i cpu.etl --disasm --json",
        }, {
            "Text output is the default. --disasm adds Zydis instruction decoding when available.",
            "Symbols require matching PDB files or a verified archive created by wperf archive.",
        });

    AddCommand(catalog, "diff", {"dif"},
        "Compare two CPU profiles; positive values indicate growth in the second trace.",
        "wperf diff -1 FIRST.etl -2 SECOND.etl [options]", false,
        {
            Value("-1,--first", "FILE", "Read the baseline profile from FILE."),
            Value("-2,--second", "FILE", "Read the comparison profile from FILE."),
            MultiValue("--symbol-path", "DIR", "Add a local PDB search directory; repeatable or semicolon-separated."),
            Value("--archive", "DIR", "Resolve unavailable paths through verified modules.tsv."),
            Value("--pid", "PID", "Include only PID."),
            Value("--dso", "TEXT", "Filter by module/shared-object substring."),
            Value("--first-pid", "PID", "PID filter for the first trace."),
            Value("--second-pid", "PID", "PID filter for the second trace."),
            Value("--normalize", "MODE", "Normalize shares by weight or samples (default: weight)."),
            Value("--match", "MODE", "Match by module/function symbol (default) or strict identity."),
            Flag("--calltree,--weighted", "Compare call-tree weights."),
            Flag("--json", "Print JSON rows."),
            Flag("--csv", "Print CSV rows."),
        },
        {"wperf diff -1 before.etl -2 after.etl --calltree --json"}, {
            "Positive values indicate growth in the second profile; negative values indicate reduction.",
            "--calltree and --weighted are equivalent.",
            "Symbol matching ignores build paths, timestamps and RVAs for resolved functions. Unresolved addresses retain strict identity keys; unmapped IPs use trace-local keys. match_status identifies added/removed rows.",
        });

    AddCommand(catalog, "archive", {},
        "Copy referenced modules and write reproducible archive manifests.",
        "wperf archive -i FILE -o DIRECTORY [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Value("-o,--output", "DIRECTORY", "Write the archive to DIRECTORY."),
            Value("-p,--pid", "PID", "Include only modules referenced by PID."),
            MultiValue("--symbol-path", "DIR", "Add a local PDB search directory; repeatable or semicolon-separated."),
            Value("--max-size-mb", "N", "Skip copied files larger than N MiB."),
            Flag("--include-trace", "Copy the input ETL as trace.etl."),
        }, {
            "The archive directory contains copied modules plus manifest.json, manifest.csv, and manifest.txt.",
            "Use --include-trace to copy the input ETL as trace.etl for a self-contained artifact.",
        });

    AddCommand(catalog, "buildid-list", {},
        "List module identity and symbolization metadata from an ETL.",
        "wperf buildid-list -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Flag("--hash", "Hash each distinct local module file once (SHA256; optional)."),
            Flag("--json", "Print JSON module identity rows."),
            Flag("--csv", "Print CSV module identity rows."),
        }, {
            "Output preserves ETL PE timestamp/checksum and mapping rows, and separates recorded_identity from local_identity.",
            "Status is matching, mismatched, or missing. --hash adds local-file SHA256; without it hash_status is not-requested.",
        });

    AddCommand(catalog, "list", {},
        "List profile sources, sessions, private providers, or pool tag snapshots.",
        "wperf list [providers] [options]", false,
        {
            Value("--max", "N", "Limit provider or pool-tag output."),
            Flag("--json", "Print JSON profile sources, sessions, providers, or pool tags."),
            Flag("--pool-tags", "List current system pool tag counters; use --max to limit rows."),
        },
        {"wperf list", "wperf list providers --max 20"}, {
            "Without providers or --pool-tags, list profile sources and active wperf-owned sessions.",
            "Provider and pool-tag enumeration can be bounded with --max. Pool counters are snapshots and allocation/free counters may wrap.",
        });

    AddCommand(catalog, "doctor", {},
        "Check elevation, ETW capabilities, state and recent runs.",
        "wperf doctor [options]", false,
        {
            Value("--bundle", "DIR", "Write a diagnostic bundle to DIR."),
            Flag("--include-dumps", "Include .dmp files in the bundle."),
            Flag("--json", "Print JSON diagnostics."),
        }, {}, {
            "--bundle writes a diagnostic bundle. Crash dumps are included only with --include-dumps; review contents before sharing.",
        });

    AddCommand(catalog, "test", {},
        "Run a lightweight non-invasive self-test.",
        "wperf test", false,
        {});

    AddCommand(catalog, "recover", {},
        "Stop stale wperf-owned ETW sessions.",
        "wperf recover (--session NAME | --all-stale)", true,
        {
            Value("--session", "NAME", "Stop one explicit ETW session."),
            Flag("--all-stale", "Stop sessions referenced by stale wperf state."),
        }, {
            "Recovery requires elevation and only stops ETW sessions recorded as wperf-owned stale state.",
        });

    AddCommand(catalog, "capabilities", {},
        "Print the machine-readable command and Agent capability catalog.",
        "wperf capabilities [--json]", false,
        {
            Flag("--json", "Print the capability catalog as JSON."),
        }, {
            "wperf capabilities --json",
        }, {
            "The catalog is generated from the same command definitions as help and runtime validation.",
            "Use it to discover commands, options, aliases, elevation requirements, and Agent hints."
        });
    AddCommand(catalog, "help", {},
        "Show general help or help for one command.",
        "wperf help [command] [--format text|markdown]", false,
        {
            Value("--format", "FORMAT", "Output format: text or markdown (default: text)."),
        },
        {
            "wperf help",
            "wperf help report",
            "wperf help --format markdown",
        }, {
            "Markdown output is generated from the same command catalog used by runtime validation.",
            "Regenerate docs/COMMAND_REFERENCE.md with: wperf help --format markdown.",
        });

    AddCommand(catalog, "version", {},
        "Print the wperf version.",
        "wperf version", false,
        {});

    return catalog;
}
std::string DisplayNames(const std::string& names) {
    std::string display;
    for (char character : names) {
        if (character == ',') display += ", ";
        else display += character;
    }
    return display;
}

std::wstring RenderGeneralHelp(const std::vector<CommandSpec>& catalog) {
    std::wostringstream out;
    out << L"wperf " << kVersion << L"\n\n"
        << L"Usage: wperf <command> [options]\n"
        << L"       wperf help <command>\n\n"
        << L"Commands:\n";
    for (const auto& spec : catalog) {
        out << L"  " << ToUtf16(spec.name);
        if (!spec.aliases.empty()) {
            out << L", ";
            for (size_t index = 0; index < spec.aliases.size(); ++index) {
                if (index != 0) out << L", ";
                out << ToUtf16(spec.aliases[index]);
            }
        }
        out << L"  " << ToUtf16(spec.description) << L"\n";
    }
    out << L"\nCommon perf-style options:\n"
        << L"  -i FILE       Input ETL for offline commands\n"
        << L"  -p PID        Restrict analysis to one process\n"
        << L"  -g            Capture or print call stacks\n"
        << L"  --folded      Emit collapsed stacks (alias: --collapsed)\n"
        << L"  --flamegraph  Emit a self-contained interactive HTML flame graph\n"
        << L"  --csv|--json  Machine-readable output (mutually exclusive in report)\n"
        << L"  --json-errors Emit one structured JSON error object on failure\n\n"
        << L"Flame graph:\n"
        << L"  wperf report -i trace.etl --symbol-path DIR --flamegraph -o trace.html\n"
        << L"  wperf flamegraph -i stacks.folded --countname nanoseconds -o graph.html\n"
        << L"  wperf report -i trace.etl --symbol-path DIR --folded -o stacks.folded\n"
        << L"  flamegraph.pl --countname nanoseconds stacks.folded > trace.svg\n\n"
        << L"Run \"wperf help <command>\" for complete options and examples.\n";
    return out.str();
}

std::wstring RenderCommandHelp(const CommandSpec& spec) {
    std::wostringstream out;
    out << L"Usage: " << ToUtf16(spec.usage) << L"\n\n"
        << ToUtf16(spec.description) << L"\n\n"
        << L"Options:\n";
    for (const auto& option : spec.options) {
        out << L"  " << ToUtf16(DisplayNames(option.names));
        if (!option.value.empty()) out << L" " << ToUtf16(option.value);
        out << L"  " << ToUtf16(option.description) << L"\n";
    }
    out << L"  -h, --help                Show this help.\n";
    out << L"Elevation: " << (spec.needs_elevation ? L"required" : L"not required") << L"\n";
    if (!spec.aliases.empty()) {
        out << L"Aliases: ";
        for (size_t index = 0; index < spec.aliases.size(); ++index) {
            if (index != 0) out << L", ";
            out << ToUtf16(spec.aliases[index]);
        }
        out << L"\n";
    }
    if (!spec.examples.empty()) {
        out << L"\nExamples:\n";
        for (const auto& example : spec.examples) out << L"  " << ToUtf16(example) << L"\n";
    }
    if (!spec.notes.empty()) {
        out << L"\nNotes:\n";
        for (const auto& note : spec.notes) out << L"  - " << ToUtf16(note) << L"\n";
    }
    return out.str();
}

std::wstring MarkdownCell(std::wstring text) {
    for (size_t position = 0; (position = text.find(L'|', position)) != std::wstring::npos;
         position += 2) {
        text.insert(position, L"\\");
    }
    std::replace(text.begin(), text.end(), L'\n', L' ');
    std::replace(text.begin(), text.end(), L'\r', L' ');
    return text;
}
std::wstring RenderMarkdown(const std::vector<CommandSpec>& catalog) {
    std::wostringstream out;
    out << L"# wperf Command Reference\n\n"
        << L"Generated from the same command catalog used by wperf runtime validation. "
        << L"Do not edit manually; run `wperf help --format markdown` instead.\n\n"
        << L"## Conventions\n\n"
        << L"- Commands use perf-style names and short aliases.\n"
        << L"- Values may be separated (`-F 99`) or attached (`-F99`); long values may use `--name=value`.\n"
        << L"- `--` ends wperf option parsing and forwards the remaining arguments to a launched command.\n"
        << L"- `wperf help <command>` and `wperf <command> --help` render the same text help.\n"
        << L"- System capture commands require an elevated hidden worker; offline analysis does not.\n"
        << L"- `--json-errors` is a global option before the command or before `--` and emits one structured error object on failure.\n\n"
        << L"## Command index\n\n"
        << L"| Command | Aliases | Purpose |\n|---|---|---|\n";
    for (const auto& spec : catalog) {
        std::wstring aliases;
        for (size_t index = 0; index < spec.aliases.size(); ++index) {
            if (index != 0) aliases += L", ";
            aliases += L"`" + ToUtf16(spec.aliases[index]) + L"`";
        }
        if (aliases.empty()) aliases = L"-";
        out << L"| `" << ToUtf16(spec.name) << L"` | " << aliases << L" | "
            << MarkdownCell(ToUtf16(spec.description)) << L" |\n";
    }
    out << L"\n## Commands\n\n";
    for (const auto& spec : catalog) {
        out << L"## `" << ToUtf16(spec.name) << L"`\n\n"
            << ToUtf16(spec.description) << L"\n\n"
            << L"```text\n" << ToUtf16(spec.usage) << L"\n```\n\n"
            << L"Elevation: " << (spec.needs_elevation ? L"required" : L"not required") << L"\n\n";
        if (!spec.aliases.empty()) {
            out << L"Aliases: ";
            for (size_t index = 0; index < spec.aliases.size(); ++index) {
                if (index != 0) out << L", ";
                out << L"`" << ToUtf16(spec.aliases[index]) << L"`";
            }
            out << L"\n\n";
        }
        out << L"| Option | Description |\n|---|---|\n";
        for (const auto& option : spec.options) {
            std::wstring option_text = ToUtf16(DisplayNames(option.names));
            if (!option.value.empty()) option_text += L" " + ToUtf16(option.value);
            out << L"| `" << MarkdownCell(option_text) << L"` | "
                << MarkdownCell(ToUtf16(option.description)) << L" |\n";
        }
        out << L"| `-h, --help` | Show this help. |\n\n";
        if (!spec.examples.empty()) {
            out << L"Examples:\n\n```powershell\n";
            for (const auto& example : spec.examples) out << ToUtf16(example) << L"\n";
            out << L"```\n\n";
        }
        if (!spec.notes.empty()) {
            out << L"Notes:\n\n";
            for (const auto& note : spec.notes) out << L"- " << ToUtf16(note) << L"\n";
            out << L"\n";
        }
    }
    return out.str();
}

const CommandSpec* FindCommand(const std::vector<CommandSpec>& catalog,
                               const std::string& name) {
    for (const auto& spec : catalog) {
        if (spec.name == name ||
            std::find(spec.aliases.begin(), spec.aliases.end(), name) != spec.aliases.end()) {
            return &spec;
        }
    }
    return nullptr;
}
std::string OptionKey(const std::string& names) {
    size_t start = 0;
    std::string first;
    while (start <= names.size()) {
        const size_t comma = names.find(',', start);
        const std::string token = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (first.empty()) first = token;
        if (token.starts_with("--")) return token;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return first;
}

void AddOption(CLI::App& command, const OptionSpec& spec, OptionValues& values,
               OptionFlags& flags,
               std::vector<std::pair<std::string, CLI::Option*>>& value_options) {
    const std::string key = OptionKey(spec.names);
    if (spec.value.empty()) {
        bool& value = flags[key];
        auto* flag = command.add_flag(spec.names, value, spec.description);
        flag->disable_flag_override();
        return;
    }
    auto [iterator, inserted] = values.try_emplace(key);
    (void)inserted;
    auto* option = command.add_option(spec.names, iterator->second, spec.description);
    option->type_name(spec.value);
    if (spec.optional_value) option->expected(0, 1);
    if (spec.join_values) {
        option->multi_option_policy(CLI::MultiOptionPolicy::Join);
        option->delimiter(';');
    }
    value_options.emplace_back(key, option);
}

const OptionSpec* FindOption(const CommandSpec& command, std::string_view name) {
    for (const auto& option : command.options) {
        size_t start = 0;
        while (start <= option.names.size()) {
            const size_t comma = option.names.find(',', start);
            const std::string_view candidate(
                option.names.data() + start,
                comma == std::string::npos ? option.names.size() - start : comma - start);
            if (candidate == name) return &option;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return nullptr;
}

std::vector<std::string> SplitOptionNames(const std::string& names) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= names.size()) {
        const size_t comma = names.find(',', start);
        result.push_back(names.substr(start, comma == std::string::npos
                                                   ? std::string::npos
                                                   : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

bool SupportsJson(const CommandSpec& command) {
    return FindOption(command, "--json") != nullptr;
}

bool MayMutateGlobalState(const std::string& name) {
    return name == "record" || name == "top" || name == "stat" ||
           name == "trace" || name == "recover";
}

std::vector<std::string> AgentHints(const std::string& name) {
    if (name == "record") {
        return {"set --duration for bounded automation",
                "use a unique output path; existing files are not overwritten",
                "use --json with --json-errors for structured success and failure"};
    }
    if (name == "top") {
        return {"set --iterations for bounded automation",
                "expect an elevation prompt unless the runner is pre-authorized"};
    }
    if (name == "trace" || name == "script") {
        return {"set --max or --limit to bound machine-readable output"};
    }
    if (name == "report") {
        return {"prefer --json --metadata --strict-quality",
                "write large results to --output and consume the artifact separately"};
    }
    if (name == "capabilities") {
        return {"call this command before relying on optional options or hardware"};
    }
    return {};
}

std::string RenderCapabilities(const std::vector<CommandSpec>& catalog) {
    nlohmann::ordered_json root;
    root["schema_version"] = 1;
    root["tool_version"] = ToUtf8(kVersion);
    root["commands"] = nlohmann::ordered_json::array();

    for (const auto& command : catalog) {
        nlohmann::ordered_json item;
        item["name"] = command.name;
        item["aliases"] = command.aliases;
        item["description"] = command.description;
        item["usage"] = command.usage;
        item["requires_elevation"] = command.needs_elevation;
        item["supports_json"] = SupportsJson(command);
        item["may_mutate_global_state"] = MayMutateGlobalState(command.name);
        item["agent_hints"] = AgentHints(command.name);

        item["options"] = nlohmann::ordered_json::array();
        for (const auto& option : command.options) {
            nlohmann::ordered_json option_item;
            option_item["names"] = SplitOptionNames(option.names);
            option_item["value"] = option.value;
            option_item["description"] = option.description;
            option_item["optional_value"] = option.optional_value;
            item["options"].push_back(std::move(option_item));
        }
        item["examples"] = command.examples;
        item["notes"] = command.notes;
        root["commands"].push_back(std::move(item));
    }

    root["global_options"] = nlohmann::ordered_json::array();
    root["global_options"].push_back({
        {"names", {"--json-errors"}},
        {"description", "Emit one structured JSON error object on failure."},
        {"position", "before the command or before the -- target separator"}
    });

    root["agent_guidance"] = {
        {"artifact_first", true},
        {"notes", {
            "Check the process exit code before consuming success data.",
            "Use --json-errors for machine-readable failures.",
            "Use bounded commands and write large outputs to files.",
            "Check quality.incomplete in report metadata before drawing conclusions."
        }}
    };
    return root.dump(2);
}
std::vector<std::wstring> NormalizeArguments(const CommandSpec& command,
                                             const std::vector<std::wstring>& arguments) {
    std::vector<std::wstring> normalized;
    normalized.reserve(arguments.size());
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--") {
            normalized.insert(normalized.end(), arguments.begin() + index, arguments.end());
            break;
        }
        if (command.name == "diff" && argument == L"-1") {
            normalized.push_back(L"--first");
            continue;
        }
        if (command.name == "diff" && argument == L"-2") {
            normalized.push_back(L"--second");
            continue;
        }
        const std::string utf8 = ToUtf8(argument);
        if (utf8.starts_with("--")) {
            const size_t equals = utf8.find('=');
            if (equals != std::string::npos) {
                const std::string name = utf8.substr(0, equals);
                const auto* option = FindOption(command, name);
                if (option != nullptr && !option->value.empty()) {
                    normalized.push_back(ToUtf16(name));
                    normalized.push_back(ToUtf16(utf8.substr(equals + 1)));
                    continue;
                }
            }
        } else if (utf8.size() > 2 && utf8[0] == '-' && utf8[1] != '-') {
            const std::string name = utf8.substr(0, 2);
            const auto* option = FindOption(command, name);
            if (option != nullptr && !option->value.empty()) {
                std::string value = utf8.substr(2);
                if (value.starts_with("=")) value.erase(0, 1);
                normalized.push_back(ToUtf16(name));
                normalized.push_back(ToUtf16(value));
                continue;
            }
        }
        normalized.push_back(argument);
    }
    return normalized;
}

}  // namespace

int RunHelp(const std::vector<std::wstring>& arguments,
            const std::vector<CommandSpec>& catalog) {
    std::string format = "text";
    std::string requested_name;
    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"-h" || argument == L"--help") {
            const auto* help_spec = FindCommand(catalog, "help");
            if (help_spec != nullptr) std::wcout << RenderCommandHelp(*help_spec);
            return 0;
        }
        if (argument == L"--format") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--format requires text or markdown");
                return 2;
            }
            format = ToUtf8(arguments[++index]);
            if (format != "text" && format != "markdown") {
                PrintError(L"--format requires text or markdown");
                return 2;
            }
            continue;
        }
        if (!argument.empty() && argument.front() == L'-') {
            PrintError(L"unknown help option: " + argument);
            return 2;
        }
        if (!requested_name.empty()) {
            PrintError(L"help accepts at most one command name");
            return 2;
        }
        requested_name = ToUtf8(argument);
    }

    const CommandSpec* requested = requested_name.empty()
                                       ? nullptr
                                       : FindCommand(catalog, requested_name);
    if (!requested_name.empty() && requested == nullptr) {
        PrintError(L"unknown command: " + ToUtf16(requested_name));
        return 2;
    }
    if (format == "markdown") {
        const std::wstring rendered = requested == nullptr
                                          ? RenderMarkdown(catalog)
                                          : RenderMarkdown(std::vector<CommandSpec>{*requested});
        std::wcout << rendered;
    } else if (requested == nullptr) {
        std::wcout << RenderGeneralHelp(catalog);
    } else {
        std::wcout << RenderCommandHelp(*requested);
    }
    return 0;
}

int RunCapabilities(const std::vector<std::wstring>& arguments,
                    const std::vector<CommandSpec>& catalog) {
    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"-h" || argument == L"--help") {
            const auto* spec = FindCommand(catalog, "capabilities");
            if (spec != nullptr) std::wcout << RenderCommandHelp(*spec);
            return 0;
        }
        if (argument == L"--json") continue;
        PrintError(L"capabilities accepts only --json");
        return 2;
    }
    std::wcout << ToUtf16(RenderCapabilities(catalog)) << L"\n";
    return 0;
}
int Run(const std::vector<std::wstring>& arguments, const Dispatch& dispatch) {
    const auto catalog = BuildCatalog();
    if (arguments.empty()) {
        std::wcout << RenderGeneralHelp(catalog);
        return 0;
    }
    if (arguments.front() == L"capabilities") return RunCapabilities(arguments, catalog);
    if (arguments.front() == L"help") return RunHelp(arguments, catalog);
    if (arguments.front() == L"-h" || arguments.front() == L"--help") {
        if (arguments.size() == 1) {
            std::wcout << RenderGeneralHelp(catalog);
            return 0;
        }
        const CommandSpec* requested = FindCommand(catalog, ToUtf8(arguments[1]));
        if (requested == nullptr) {
            PrintError(L"unknown command: " + arguments[1]);
            return 2;
        }
        std::wcout << RenderCommandHelp(*requested);
        return 0;
    }
    if (arguments.front() == L"--version") {
        std::wcout << L"wperf " << kVersion << L"\n";
        return 0;
    }

    const CommandSpec* selected = FindCommand(catalog, ToUtf8(arguments.front()));
    if (selected == nullptr) {
        PrintError(L"unknown command: " + arguments.front());
        std::wcout << RenderGeneralHelp(catalog);
        return 2;
    }

    CLI::App app{"wperf " + selected->name};
    app.set_help_flag("");
    app.prefix_command(CLI::PrefixCommandMode::SeparatorOnly);
    bool help = false;
    app.add_flag("-h,--help", help, "Show this help.");

    OptionValues option_values;
    OptionFlags option_flags;
    std::vector<std::pair<std::string, CLI::Option*>> value_options;
    std::string list_mode;
    for (const auto& option : selected->options) {
        AddOption(app, option, option_values, option_flags, value_options);
    }
    if (selected->name == "list") {
        auto* mode = app.add_option("mode", list_mode, "Optional providers clause.");
        mode->type_name("providers");
        mode->expected(0, 1);
    }

    const std::vector<std::wstring> normalized_rest = NormalizeArguments(
        *selected, std::vector<std::wstring>(arguments.begin() + 1, arguments.end()));
    std::vector<std::string> utf8_arguments;
    utf8_arguments.reserve(normalized_rest.size());
    for (const auto& argument : normalized_rest) {
        utf8_arguments.push_back(ToUtf8(argument));
    }
    std::reverse(utf8_arguments.begin(), utf8_arguments.end());
    try {
        app.parse(utf8_arguments);
    } catch (const CLI::ParseError& error) {
        PrintError(ToUtf16(error.what()));
        return 2;
    }
    for (const auto& [key, option] : value_options) {
        if (option->count() == 0) option_values.erase(key);
    }

    if (help) {
        std::wcout << RenderCommandHelp(*selected);
        return 0;
    }
    if (selected->name == "version") {
        std::wcout << L"wperf " << kVersion << L"\n";
        return 0;
    }
    if (selected->name == "list" && !list_mode.empty()) option_values["@mode"] = list_mode;

    std::vector<std::wstring> target;
    const auto separator = std::find(normalized_rest.begin(), normalized_rest.end(), L"--");
    if (separator != normalized_rest.end()) {
        target.assign(separator + 1, normalized_rest.end());
    }
    CommandRequest request;
    std::wstring request_error;
    if (!BuildCommandRequest(selected->name, option_values, option_flags, target, request,
                             &request_error)) {
        PrintError(request_error.empty() ? L"invalid command request" : request_error);
        return 2;
    }
    return dispatch(request);
}
std::wstring GeneralHelp() {
    return RenderGeneralHelp(BuildCatalog());
}

std::wstring CommandHelp(const std::wstring& command) {
    const std::string name = ToUtf8(command);
    const auto catalog = BuildCatalog();
    const CommandSpec* spec = FindCommand(catalog, name);
    return spec == nullptr ? std::wstring{} : RenderCommandHelp(*spec);
}

std::string MarkdownReference() {
    return ToUtf8(RenderMarkdown(BuildCatalog()));
}

std::string CapabilitiesJson() {
    return RenderCapabilities(BuildCatalog());
}

}  // namespace wperf::cli

namespace wperf::commands {
void PrintGeneralHelp() {
    std::wcout << cli::GeneralHelp();
}

bool PrintCommandHelp(std::wstring_view command) {
    const std::wstring help = cli::CommandHelp(std::wstring(command));
    if (help.empty()) return false;
    std::wcout << help;
    return true;
}
}
