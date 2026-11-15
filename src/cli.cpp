#include "cli.h"

#include "commands/help.h"
#include "commands/util.h"
#include "common.h"

#include <CLI/CLI.hpp>
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
    return {std::move(names), {}, std::move(description), false};
}

OptionSpec Value(std::string names, std::string value, std::string description,
                 bool optional_value = false) {
    return {std::move(names), std::move(value), std::move(description), optional_value};
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
            Value("-e,--event", "NAME|ID", "Select a CPU profile, ETW source name or source ID."),
            Value("--profile", "NAME", "cpu, cpu-sched, sched, syscall, vm, io, kernel, all, or pmu."),
            Value("--pmc-source", "ID", "Select an ETW PMC profile source."),
            Value("--period", "N", "PMC events per interrupt (source-specific count)."),
            Value("--pmc-counters", "IDS", "Counter snapshots on CSwitch (comma-separated IDs)."),
            Flag("--stack-cache", "Emit modern StackWalkRef/Def records when supported."),
            Flag("-b,--lbr", "Enable LBR call-stack collection when supported."),
            Flag("--context-registers", "Enable context-register collection when supported."),
            Value("--provider", "GUID", "Capture a private ETW provider session."),
            Value("--session", "NAME", "Use an explicit ETW session name."),
        },
        {
            "wperf record --elevate -F 999 -g --duration 5000 -o cpu.etl",
            "wperf record --elevate -g -o child.etl -- .\\app.exe --input data.txt",
        }, {
            "System capture requires administrator rights; --elevate starts one hidden elevated worker.",
            "System ETW call stacks are enabled by default. Use --no-call-graph to disable them.",
            "cpu-sched, sched, and all also request CSwitch/ReadyThread stack tracing.",
        });

    AddCommand(catalog, "report", {"rep"},
        "Produce a perf-style CPU, I/O, PMC, or off-CPU report from an ETL trace.",
        "wperf report -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Value("-o,--output", "FILE", "Write the selected report format to FILE."),
            Value("-p,--pid", "PID", "Include only PID."),
            Value("-t,--tid", "TID", "Include only TID."),
            Value("--comm", "TEXT", "Filter by process name substring."),
            Value("--dso", "TEXT", "Filter by module/shared-object substring."),
            Value("--symbol", "TEXT", "Filter by symbol substring."),
            Value("--symbol-path", "DIR", "Add a local PDB search directory."),
            Value("--archive", "DIR", "Resolve unavailable paths through verified modules.tsv."),
            Value("--percent-limit", "N", "Hide rows below N percent."),
            Value("-s,--sort", "FIELD", "Sort by samples, comm, dso, or symbol."),
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
            Flag("--off-cpu", "Report blocked/ready intervals weighted by wait time."),
            Flag("--off-cpu-include-inferred", "Include end-of-capture truncated intervals."),
            Flag("--stdio", "Text output (default)."),
            Flag("--quiet", "Suppress quality diagnostics."),
            Flag("--verbose", "Print complete quality counters to stderr."),
            Flag("--csv", "Print CSV rows."),
            Flag("--json", "Print JSON rows."),
            Value("--kind", "KIND", "Select cpu, io, or pmc report."),
        },
        {
            "wperf report -i cpu.etl --symbol-path .\\build\\Release --call-graph",
            "wperf report -i cpu.etl --json --metadata",
            "wperf report -i sched.etl --off-cpu --folded --quiet -o offcpu.folded",
        }, {
            "Quality diagnostics go to stderr; --quiet suppresses them and --verbose prints every counter.",
            "--folded/--collapsed and --call-graph cannot be combined with --json or --csv.",
            "Folded output is directly accepted by FlameGraph scripts. Counts are samples for CPU profiles and nanoseconds for off-CPU profiles.",
            "-o writes UTF-8 without BOM and LF line endings.",
        });

    AddCommand(catalog, "script", {"scr"},
        "Export one line per sample.",
        "wperf script -i FILE [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Value("-o,--output", "FILE", "Write output to FILE."),
            Value("-p,--pid", "PID", "Include only PID."),
            Value("-F,--fields", "LIST", "Comma-separated fields (default: timestamp,cpu,pid,tid,ip,comm,dso,symbol)."),
            Value("--symbol-path", "DIR", "Add a local PDB search directory."),
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
            Flag("--per-thread", "Print per-thread counters."),
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
            "Without -p, stat launches the command after -- and observes that process tree.",
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
            Value("--symbol-path", "DIR", "Add a local PDB search directory."),
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
            Value("--symbol-path", "DIR", "Add a local PDB search directory."),
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
            Value("--symbol-path", "DIR", "Add a local PDB search directory."),
            Value("--archive", "DIR", "Resolve unavailable paths through verified modules.tsv."),
            Value("--pid", "PID", "Include only PID."),
            Value("--dso", "TEXT", "Filter by module/shared-object substring."),
            Value("--first-pid", "PID", "PID filter for the first trace."),
            Value("--second-pid", "PID", "PID filter for the second trace."),
            Value("--normalize", "MODE", "Normalize shares by weight or samples (default: weight)."),
            Flag("--calltree,--weighted", "Compare call-tree weights."),
            Flag("--json", "Print JSON rows."),
            Flag("--csv", "Print CSV rows."),
        },
        {"wperf diff -1 before.etl -2 after.etl --calltree --json"}, {
            "Positive values indicate growth in the second profile; negative values indicate reduction.",
            "--calltree and --weighted are equivalent.",
        });

    AddCommand(catalog, "archive", {},
        "Copy referenced modules and write reproducible archive manifests.",
        "wperf archive -i FILE -o DIRECTORY [options]", false,
        {
            Value("-i,--input", "FILE", "Read FILE."),
            Value("-o,--output", "DIRECTORY", "Write the archive to DIRECTORY."),
            Value("-p,--pid", "PID", "Include only modules referenced by PID."),
            Value("--symbol-path", "DIR", "Add a local PDB search directory."),
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
            Flag("--json", "Print JSON module identity rows."),
            Flag("--csv", "Print CSV module identity rows."),
        }, {
            "Output includes module base, size, process, PE/PDB identity, SHA256 when available, and image path.",
        });

    AddCommand(catalog, "list", {},
        "List profile sources and active sessions, or private providers.",
        "wperf list [providers] [options]", false,
        {
            Value("--max", "N", "Limit provider output."),
            Flag("--json", "Print JSON profile sources, sessions, or providers."),
        },
        {"wperf list", "wperf list providers --max 20"}, {
            "Without providers, list profile sources and active wperf-owned sessions.",
            "Provider enumeration can be bounded with --max.",
        });

    AddCommand(catalog, "doctor", {},
        "Check elevation, ETW capabilities, state and recent runs.",
        "wperf doctor [options]", false,
        {
            Value("--bundle", "DIR", "Write a diagnostic bundle to DIR."),
            Flag("--include-dumps", "Include .dmp files in the bundle."),
            Flag("--json", "Print JSON diagnostics."),
        }, {
            "--bundle writes a redacted diagnostic bundle. Crash dumps are included only with --include-dumps.",
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
        << L"  --csv|--json  Machine-readable output (mutually exclusive in report)\n\n"
        << L"Flame graph:\n"
        << L"  wperf report -i trace.etl --symbol-path DIR --folded --quiet -o stacks.folded\n"
        << L"  flamegraph.pl stacks.folded > flamegraph.svg\n\n"
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
        << L"- System capture commands require an elevated hidden worker; offline analysis does not.\n\n"
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

int Run(const std::vector<std::wstring>& arguments, const Dispatch& dispatch) {
    const auto catalog = BuildCatalog();
    if (arguments.empty()) {
        std::wcout << RenderGeneralHelp(catalog);
        return 0;
    }
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
        std::wcerr << L"wperf: " << ToUtf16(error.what()) << L"\n";
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
