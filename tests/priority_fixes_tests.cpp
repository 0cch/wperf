#include "analysis.h"
#include "cpu_report.h"
#include "common.h"
#include "state.h"
#include "trace_events.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace {
int failures=0;
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void TestWeightsAndDomains() {
    wperf::TraceData trace;
    trace.processes.push_back({100,0,L"app.exe",L"",1,1000});
    trace.images.push_back({100,0x1000,0x100,0,0,0,0,L"one.dll"});
    trace.images.push_back({100,0x2000,0x100,0,0,0,0,L"two.dll"});
    wperf::SampleRecord first;
    first.pid=100; first.tid=10; first.timestamp_ns=10; first.instruction_pointer=0x1010; first.period=1000000;
    auto second=first; second.instruction_pointer=0x2010; second.period=9000000;
    trace.samples={first,second};
    wperf::Symbolizer symbols;
    wperf::CpuReportOptions options; options.json=true; options.quiet=true;
    const auto report=[&]() {
        std::wostringstream output; wperf::WriteCpuReport(trace,symbols,options,output);
        return nlohmann::json::parse(wperf::ToUtf8(output.str()));
    };
    auto rows=report();
    Check(rows[0]["overhead"]==90.0 && rows[1]["overhead"]==10.0,"unequal periods determine overhead and default sort");
    Check(rows[0]["weight"]==9000000 && rows[0]["weight_unit"]=="ns","CPU weights carry time units");
    options.weight_mode=wperf::CpuReportOptions::WeightMode::Samples;
    rows=report(); Check(rows[0]["overhead"]==50.0 && rows[1]["overhead"]==50.0,"explicit sample count mode");
    options.weight_mode=wperf::CpuReportOptions::WeightMode::Period;
    trace.samples[1].period=0; rows=report();
    Check(rows[0]["normalization_fallback"]==true && rows[0]["overhead"]==50.0,"unknown period does not bias known-period rows");
    trace.samples[1].period=9000000; options.json=false; options.folded=true;
    std::wostringstream folded; wperf::WriteCpuReport(trace,symbols,options,folded);
    Check(folded.str().find(L" 9000000\n")!=std::wstring::npos,"folded stacks retain period weights");
    options.folded=false; options.call_tree=true;
    std::wostringstream tree; wperf::WriteCpuReport(trace,symbols,options,tree);
    Check(tree.str().find(L"90.00%")!=std::wstring::npos,"call trees use the same period denominator");
    trace.samples[1].kind=wperf::SampleKind::Pmc; trace.samples[1].profile_source=19;
    bool rejected=false;
    try { std::wostringstream out; wperf::WriteCpuReport(trace,symbols,options,out); }
    catch (const wperf::CpuReportLimitError&) { rejected=true; }
    Check(rejected,"CPU time and PMC counter domains cannot be mixed");
    options.profile_source=19; options.call_tree=false; options.json=true; rows=report();
    Check(rows.size()==1 && rows[0]["weight_unit"]=="events","source selection keeps PMC event-count units");
}
void TestRecoveryCheckpoint() {
    const auto directory=std::filesystem::temp_directory_path()/(L"wperf-checkpoint-test-"+wperf::NewId());
    wperf::StateStore store(directory/L"state.json");
    wperf::RunState worker; worker.correlation_id=L"test"; worker.worker_pid=123;
    worker.restore_source=19; worker.restore_interval=100000; worker.configured_interval=1000000; worker.restore_clear_pmc=1;
    Check(store.WriteRecovery(worker),"persist recovery checkpoint");
    wperf::RunState controller; controller.correlation_id=L"test";
    Check(store.Write(controller),"write controller status");
    const auto restored=store.ReadRecovery();
    Check(restored && restored->restore_interval==100000 && restored->restore_clear_pmc==1 && restored->worker_pid==123,
          "controller updates preserve worker recovery and identity");
    std::error_code ec; std::filesystem::remove_all(directory,ec);
}
void TestComparisonKeys() {
    wperf::ResolvedSymbol before; before.module=L"app.exe"; before.symbol=L"Burn(int)";
    before.resolved=true; before.function_rva=0x1000; before.module_identity=L"c:\\old\\app.exe|1|2|4096";
    auto after=before; after.function_rva=0x2000; after.module_identity=L"d:\\new\\app.exe|3|4|8192";
    Check(wperf::SymbolComparisonKey(before,false)==wperf::SymbolComparisonKey(after,false),"resolved functions align across rebuilds and paths");
    Check(wperf::SymbolComparisonKey(before,true)!=wperf::SymbolComparisonKey(after,true),"strict matching retains build identity");
    before.resolved=after.resolved=false;
    Check(wperf::SymbolComparisonKey(before,false)!=wperf::SymbolComparisonKey(after,false),"unresolved addresses do not claim cross-build matches");
    wperf::ResolvedSymbol unmapped; unmapped.module=L"[unknown]"; unmapped.symbol=L"[unknown]";
    Check(wperf::SymbolComparisonKey(unmapped,false,L"before|100|4096")!=
          wperf::SymbolComparisonKey(unmapped,false,L"after|100|4096"),"unmapped IPs cannot masquerade as a shared symbol across traces");
}
void TestBoundariesAndFinalization() {
    wperf::TraceData trace; trace.start_ns=1; trace.end_ns=1000;
    trace.processes.push_back({100,0,L"app.exe",L"",1,100}); trace.threads.push_back({100,10,1,100});
    wperf::TraceEvent block; block.kind=wperf::TraceEventKind::ContextSwitch; block.timestamp_ns=50;
    block.related_pid=100; block.related_tid=10; block.previous_state=5;
    trace.events={block}; wperf::BuildOffCpuIntervals(trace);
    Check(trace.off_cpu_intervals.size()==1 && trace.off_cpu_intervals[0].end_ns==100 && trace.off_cpu_intervals[0].duration_ns==50,
          "inferred waits stop at thread termination");
    trace.threads[0].end_ns=200; wperf::BuildOffCpuIntervals(trace);
    Check(trace.off_cpu_intervals[0].end_ns==100,"process termination also bounds inferred waits");
    trace.threads[0].end_ns=0; trace.processes[0].end_ns=0;
    wperf::TraceEvent ready; ready.kind=wperf::TraceEventKind::ReadyThread; ready.timestamp_ns=60; ready.related_pid=100; ready.related_tid=10;
    auto again=ready; again.timestamp_ns=70;
    wperf::TraceEvent run; run.kind=wperf::TraceEventKind::ContextSwitch; run.timestamp_ns=80; run.pid=100; run.tid=10;
    trace.events={block,ready,again,run}; wperf::BuildOffCpuIntervals(trace);
    Check(trace.quality.off_cpu_duplicate_ready==1 && trace.quality.incomplete,"derived anomalies finalize completeness");
    wperf::BuildOffCpuIntervals(trace);
    Check(trace.quality.off_cpu_duplicate_ready==1,"rebuilding derived intervals does not double quality counters");
}
void TestPmcSelectionKeepsBaseline() {
    wperf::TraceData trace; trace.pmc_counter_sources={19};
    for (uint64_t time=1;time<=3;++time) {
        wperf::TraceEvent point; point.kind=wperf::TraceEventKind::ContextSwitch;
        point.timestamp_ns=time; point.related_pid=100; point.related_tid=10; point.pmc_counters={time*100};
        trace.events.push_back(point);
    }
    const auto rows=wperf::AggregatePmcCounters(trace,[](uint64_t start,const wperf::TraceEvent& end){return start>=2 && end.timestamp_ns<=3;});
    Check(rows.size()==1 && rows[0].delta==100 && rows[0].intervals==1,"PMC windows retain prior baseline and exclude straddling intervals");
}
}
int main() {
    TestWeightsAndDomains(); TestRecoveryCheckpoint(); TestComparisonKeys();
    TestBoundariesAndFinalization(); TestPmcSelectionKeepsBaseline();
    if (!failures) std::cout << "priority fixes: PASS\n";
    return failures ? 1 : 0;
}
