#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace introvert::updates {
struct InstallTransaction {
    long long parentPid = 0;
    std::filesystem::path target, payload, workspace;
    std::string token, version;
    std::vector<std::string> restartArguments;
    int exitWaitSeconds = 60, healthWaitSeconds = 45;
};
// Standalone standard-library/OS helper: it can outlive the application without
// loading Qt or keeping Windows application DLLs open. Never opens app.db.
int installTransaction(const InstallTransaction &transaction);
}
