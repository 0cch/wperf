#pragma once
#include "etw.h"
#include "state.h"

namespace wperf {
class SessionGuard {
public:
    ~SessionGuard();
    SessionGuard() = default;
    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;
    bool Start(const std::wstring& session, const ProfileRestoreInfo& restore,
               std::wstring* error = nullptr);
    void Complete();
    bool UpdateRestore(const ProfileRestoreInfo& restore, std::wstring* error = nullptr);
private:
    HANDLE clean_event_ = nullptr;
    RunState state_;
};
}
