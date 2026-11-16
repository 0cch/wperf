#include "common.h"
#include "commands/live_control.h"
#include "elevation.h"
#include "etw.h"
#include "state.h"
#include <iostream>
#include <fstream>
#include <filesystem>

using namespace wperf;
bool RequireLive() { return GetEnvironmentVariableW(L"WPERF_REQUIRE_LIVE_TESTS",nullptr,0)!=0; }
bool WaitGone(const std::wstring& session) {
    const auto end=GetTickCount64()+6000;
    while(GetTickCount64()<end) { if(!SessionExists(session))return true; Sleep(20); }
    return false;
}
bool WaitOutput(LiveWorkerClient& worker,bool top) {
    const auto end=GetTickCount64()+15000;
    while(GetTickCount64()<end) {
        if(worker.HasData()) {
            std::wstring line;
            if(!worker.ReceiveLine(line,1000,nullptr))return false;
            if(top&&line.rfind(L"TOP2_SAMPLE ",0)==0)return true;
            if(!top&&line.rfind(L"OUT ",0)==0&&line.size()>4&&line[4]>=L'0'&&line[4]<=L'9')return true;
            if(line.rfind(L"FINISHED ",0)==0||line.rfind(L"ERROR ",0)==0) { std::wcerr<<line<<L"\n"; return false; }
        } else Sleep(10);
    }
    return false;
}
bool TestRecordRecovery(const std::filesystem::path& bin) {
    TRACE_PROFILE_INTERVAL before{0,0}; ULONG returned=0;
    if (TraceQueryInformation(0,TraceSampledProfileIntervalInfo,&before,sizeof(before),&returned)!=ERROR_SUCCESS) return false;
    const auto session=L"wperf-integration-record-"+NewId();
    const auto output=std::filesystem::temp_directory_path()/(session+L".etl");
    ProcessLaunchResult controller;
    if (!LaunchWorkerProcess((bin/L"wperf.exe").wstring(),
        {L"record",L"-F",L"99",L"--no-call-graph",L"--session",session,L"--duration",L"15000",L"-o",output.wstring()},false,controller,nullptr)) return false;
    ScopedHandle parent(controller.process);
    RunState active;
    const auto deadline=GetTickCount64()+10000;
    while (GetTickCount64()<deadline) {
        for (const auto& state:StateStore::ReadAll(GetSessionsDirectory()))
            if (state.session_name==session && state.controller_role==L"controller" && state.phase==L"CAPTURING") active=state;
        if (active.worker_pid!=0) break;
        Sleep(20);
    }
    const auto checkpoint=StateStore(StateStore::PathFor(active.correlation_id)).ReadRecovery();
    bool valid=checkpoint && checkpoint->worker_pid==active.worker_pid &&
               checkpoint->restore_interval==before.Interval && checkpoint->configured_interval!=before.Interval;
    std::cout<<"record checkpoint: before="<<before.Interval<<" original="<<(checkpoint?checkpoint->restore_interval:0)
             <<" configured="<<(checkpoint?checkpoint->configured_interval:0)<<" worker="<<active.worker_pid<<'\n';
    ScopedHandle child(OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,active.worker_pid));
    if (child.get()) TerminateProcess(child.get(),93);
    else TerminateProcess(parent.get(),93);
    valid=WaitGone(session) && valid;
    if (WaitForSingleObject(parent.get(),10000)!=WAIT_OBJECT_0) { TerminateProcess(parent.get(),94); valid=false; }
    TRACE_PROFILE_INTERVAL after{0,0};
    // Session disappearance precedes guard configuration restore; wait for both.
    bool restored=false;
    const auto restore_deadline=GetTickCount64()+6000;
    while (GetTickCount64()<restore_deadline) {
        restored=TraceQueryInformation(0,TraceSampledProfileIntervalInfo,&after,sizeof(after),&returned)==ERROR_SUCCESS && after.Interval==before.Interval;
        if (restored) break;
        Sleep(20);
    }
    if (!restored && checkpoint) {
        // Restore only this test's owned configuration if the assertion fails.
        RestoreProfileConfiguration({0,before.Interval,checkpoint->configured_interval,false},nullptr);
    }
    std::error_code ec; std::filesystem::remove(output,ec);
    if (!valid || !restored) std::wcerr<<L"record checkpoint/restore validation failed: after="<<after.Interval<<L"\n";
    return valid && restored;
}
int wmain(int argc,wchar_t** argv) {
    const auto bin=std::filesystem::path(GetExecutablePath()).parent_path();
    if(argc==3 && std::wstring(argv[1])==L"--controller-child") {
        LiveWorkerOptions options; options.mode=L"trace"; options.event_filter=L"sched";
        options.pid_text=L"4294967294"; options.duration_text=L"0"; options.max_text=L"500";
        options.session_name=L"wperf-integration-orphan-"+NewId();
        LiveWorkerClient worker; std::wstring error;
        if(!worker.StartWithExecutable(options,(bin/L"wperf.exe").wstring(),false,&error))return 1;
        const auto end=GetTickCount64()+5000;
        while(!SessionExists(options.session_name)&&GetTickCount64()<end)Sleep(10);
        if(!SessionExists(options.session_name))return 1;
        std::ofstream info(std::filesystem::path(argv[2]),std::ios::binary);
        info<<worker.WorkerPid()<<'\n'<<ToUtf8(options.session_name)<<'\n'; info.close();
        Sleep(INFINITE);
        return 1;
    }
    bool elevated=false; IsProcessElevated(elevated,nullptr);
    if(!elevated) {
        std::cout<<"SKIP: live ETW tests require elevation\n";
        return RequireLive()?1:77;
    }
    EnablePrivilege(L"SeSystemProfilePrivilege",nullptr);
    ProcessLaunchResult burn;
    if(!LaunchTargetProcess({(bin/L"wperf_cpu_burn.exe").wstring(),L"10000",L"2"},burn,nullptr))return 1;
    ScopedHandle workload(burn.process);
    auto fail=[&](const wchar_t* message) { std::wcerr<<L"FAIL: "<<message<<L"\n"; TerminateProcess(workload.get(),1); return 1; };
    LiveWorkerOptions options; options.mode=L"top"; options.interval_ms=100;
    options.session_name=L"wperf-integration-top-"+NewId();
    LiveWorkerClient worker; std::wstring error;
    if(!worker.StartWithExecutable(options,(bin/L"wperf.exe").wstring(),false,&error))return fail(error.c_str());
    if(!WaitOutput(worker,true))return fail(L"top produced no real CPU samples");
    worker.SendStop(nullptr); DWORD exit=0;
    if(!worker.StopAndDrain(10000,exit,&error)||exit!=0||!WaitGone(options.session_name))return fail(L"graceful STOP failed");
    worker.Close();
    options.mode=L"trace"; options.event_filter=L"sched"; options.max_text=L"1000";
    options.session_name=L"wperf-integration-trace-"+NewId();
    if(!worker.StartWithExecutable(options,(bin/L"wperf.exe").wstring(),false,&error)||!WaitOutput(worker,false))
        return fail(L"live scheduler trace produced no decoded events");
    worker.SendStop(nullptr);
    if(!worker.StopAndDrain(10000,exit,&error)||exit!=0||!WaitGone(options.session_name))return fail(L"trace STOP failed");
    worker.Close(); options.mode=L"top";
    options.session_name=L"wperf-integration-killed-"+NewId();
    if(!worker.StartWithExecutable(options,(bin/L"wperf.exe").wstring(),false,&error)||!WaitOutput(worker,true))return fail(L"second capture failed");
    ScopedHandle child(OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,worker.WorkerPid()));
    if(!child.get()||!TerminateProcess(child.get(),91)||!WaitGone(options.session_name))return fail(L"guard left killed worker's session alive");
    worker.Close();

    const auto info_path=std::filesystem::temp_directory_path()/(L"wperf-controller-"+NewId()+L".txt");
    ProcessLaunchResult controller;
    if(!LaunchWorkerProcess(GetExecutablePath(),{L"--controller-child",info_path.wstring()},false,controller,&error))return fail(L"create owned controller");
    ScopedHandle parent(controller.process);
    const auto deadline=GetTickCount64()+10000;
    while(!std::filesystem::exists(info_path)&&GetTickCount64()<deadline)Sleep(20);
    std::ifstream info(info_path,std::ios::binary); uint32_t pid=0; std::string session;
    if(!(info>>pid>>session)) { TerminateProcess(parent.get(),92); return fail(L"orphan test did not become ready"); }
    ScopedHandle orphan(OpenProcess(SYNCHRONIZE,FALSE,pid));
    TerminateProcess(parent.get(),92);
    if(!orphan.get()||WaitForSingleObject(orphan.get(),10000)!=WAIT_OBJECT_0||!WaitGone(ToUtf16(session)))
        return fail(L"worker did not stop after its controller exited with no matching events");
    std::error_code ignored; std::filesystem::remove(info_path,ignored);
    if (!TestRecordRecovery(bin)) return fail(L"record worker kill did not preserve and restore the timer checkpoint");
    TerminateProcess(workload.get(),0);
    std::cout<<"live ETW CPU, STOP, worker kill, empty-filter controller kill and record timer recovery: PASS\n";
    return 0;
}
