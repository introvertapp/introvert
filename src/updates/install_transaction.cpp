#include "install_transaction.h"
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace introvert::updates {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void pause() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
#ifdef _WIN32
std::wstring quote(const std::wstring &value)
{
    std::wstring result = L"\""; unsigned slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0; result += c;
    }
    result.append(slashes * 2, L'\\'); return result + L'\"';
}
HANDLE launch(const fs::path &program, const std::vector<std::wstring> &arguments, bool installer = false)
{
    std::wstring command = quote(program.wstring());
    for (const auto &argument : arguments) command += L" " + (installer && argument.rfind(L"/D=",0)==0 ? argument : quote(argument));
    STARTUPINFOW start{}; start.cb = sizeof(start); PROCESS_INFORMATION process{};
    require(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           program.parent_path().c_str(), &start, &process), "Cannot launch the updated application or installer.");
    CloseHandle(process.hThread); return process.hProcess;
}
void waitForParent(const InstallTransaction &t)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(t.parentPid));
    if (!process) { require(GetLastError() == ERROR_INVALID_PARAMETER, "Cannot wait for Introvert to exit."); return; }
    const auto result = WaitForSingleObject(process, DWORD(t.exitWaitSeconds * 1000)); CloseHandle(process);
    require(result == WAIT_OBJECT_0, "Introvert did not exit; no application files were changed.");
}
#else
pid_t launch(const fs::path &program, const std::vector<std::string> &arguments)
{
    const auto child = fork(); require(child >= 0, "Cannot start the installer or application.");
    if (child == 0) {
        std::vector<char *> argv{const_cast<char *>(program.c_str())};
        for (const auto &arg : arguments) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr); execv(program.c_str(), argv.data()); _exit(127);
    }
    return child;
}
int wait(pid_t child) { int status = 0; while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1; return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
void waitForParent(const InstallTransaction &t)
{
    const auto until = Clock::now() + std::chrono::seconds(t.exitWaitSeconds);
    while (kill(pid_t(t.parentPid), 0) == 0 || errno == EPERM) {
        require(Clock::now() < until, "Introvert did not exit; no application files were changed."); pause();
    }
}
#endif
bool healthy(const InstallTransaction &t)
{
    std::ifstream file(t.workspace / "healthy"); std::string token, version;
    return bool(std::getline(file, token)) && bool(std::getline(file, version)) && token == t.token && version == t.version;
}
fs::path executable(const fs::path &target)
{
#ifdef __APPLE__
    return target / "Contents/MacOS/Introvert";
#elif defined(_WIN32)
    return target / "Introvert.exe";
#else
    return target / "launch-introvert";
#endif
}
bool startAndConfirm(const InstallTransaction &t)
{
    const auto health = (t.workspace / "healthy").u8string();
#ifdef _WIN32
    std::vector<std::wstring> arguments{L"--update-health",fs::u8path(health).wstring(),L"--update-token",fs::u8path(t.token).wstring()};
    for(const auto &arg:t.restartArguments)arguments.push_back(fs::u8path(arg).wstring());
    const auto child = launch(executable(t.target), arguments);
#else
    std::vector<std::string> arguments{"--update-health",health,"--update-token",t.token};
    arguments.insert(arguments.end(),t.restartArguments.begin(),t.restartArguments.end());
    const auto child = launch(executable(t.target), arguments);
#endif
    const auto until = Clock::now() + std::chrono::seconds(t.healthWaitSeconds);
    while (Clock::now() < until) {
        if (healthy(t)) {
#ifdef _WIN32
            CloseHandle(child);
#endif
            return true;
        }
        pause();
    }
#ifdef _WIN32
    TerminateProcess(child, 1); WaitForSingleObject(child, 5000); CloseHandle(child);
#else
    kill(child, SIGTERM);
    int status = 0; bool exited = false;
    for (int i = 0; i < 30; ++i) { if (waitpid(child,&status,WNOHANG)==child) {exited=true;break;} pause(); }
    if (!exited) {kill(child,SIGKILL); wait(child);}
#endif
    return false;
}
void restartOld(const InstallTransaction &t)
{
#ifdef _WIN32
    std::vector<std::wstring> arguments;
    for(const auto &arg:t.restartArguments)arguments.push_back(fs::u8path(arg).wstring());
    arguments.push_back(L"--update-result");arguments.push_back((t.workspace/"result.txt").wstring());
    CloseHandle(launch(executable(t.target), arguments));
#else
    auto arguments=t.restartArguments;arguments.push_back("--update-result");arguments.push_back((t.workspace/"result.txt").string());
    launch(executable(t.target), arguments);
#endif
}
}

int installTransaction(const InstallTransaction &t)
{
    const auto backup = t.target.parent_path() / (t.target.filename().u8string() + ".update-backup-" + t.token);
    bool backedUp = false, parentExited = false, committed = false, validated = false;
    try {
        require(t.parentPid > 1 && t.token.size() == 64 && !t.version.empty(), "Invalid update transaction.");
        for (auto c : t.token) require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "Invalid update token.");
        require(t.target.is_absolute() && t.payload.is_absolute() && t.workspace.is_absolute(), "Update paths must be absolute.");
        require(fs::is_directory(t.workspace) && !fs::is_symlink(t.workspace)
            && !fs::is_symlink(t.target) && !fs::exists(backup), "Unsafe update staging or backup location.");
        require(fs::is_regular_file(executable(t.target)) && fs::exists(t.payload) && !fs::is_symlink(t.payload), "Application or update payload is missing.");
        require(t.workspace.filename().u8string().rfind(".introvert-update-",0)==0, "The updater requires its private staging directory.");
        const auto staging=fs::canonical(t.workspace);
        const auto within=[&](const fs::path &path) {
            const auto relative=fs::weakly_canonical(path).lexically_relative(staging);
            return !relative.empty() && !relative.is_absolute() && *relative.begin()!="..";
        };
        require(within(t.payload) && !within(t.target), "Update payload and installation paths overlap.");
        validated=true;
        waitForParent(t);
        parentExited=true;
#ifdef __APPLE__
        require(t.target.filename() == "Introvert.app" && t.payload.filename() == "Introvert.app", "The update must contain Introvert.app.");
        require(wait(launch("/usr/bin/codesign", {"--verify","--deep","--strict",t.payload.string()})) == 0, "The staged application's integrity signature is invalid.");
        fs::rename(t.target, backup); backedUp = true;
        fs::rename(t.payload, t.target);
#elif defined(_WIN32)
        require(t.payload.extension() == L".exe", "The update must be a Windows installer.");
        fs::copy(t.target, backup, fs::copy_options::recursive | fs::copy_options::copy_symlinks); backedUp = true;
        const auto installer = launch(t.payload, {L"/S", L"/D=" + t.target.wstring()}, true);
        // Do not roll files back underneath an installer that is still running.
        const auto waited = WaitForSingleObject(installer, INFINITE);
        DWORD code = 1; GetExitCodeProcess(installer, &code); CloseHandle(installer);
        require(waited == WAIT_OBJECT_0 && code == 0, "The application installer did not complete successfully.");
#else
        require(t.target == "/opt/introvert", "Linux updates must use the installed system package.");
        const auto extension = t.payload.extension().string();
        require(extension == ".deb" || extension == ".rpm", "Unsupported Linux package.");
        const std::string manager = extension == ".deb" ? (fs::exists("/usr/bin/apt-get") ? "/usr/bin/apt-get" : "/usr/bin/dpkg")
            : (fs::exists("/usr/bin/dnf") ? "/usr/bin/dnf" : "/usr/bin/rpm");
        require(fs::exists("/usr/bin/pkexec") && fs::exists(manager), "The system authorization or package manager is unavailable.");
        std::vector<std::string> install{manager};
        if(manager=="/usr/bin/apt-get" || manager=="/usr/bin/dnf")install.insert(install.end(),{"install","--yes",t.payload.string()});
        else install.insert(install.end(),{extension==".deb" ? "-i" : "-U",t.payload.string()});
        require(wait(launch("/usr/bin/pkexec", install)) == 0,
                "The system package update was cancelled or failed. The existing application has been reopened.");
#endif
        require(startAndConfirm(t), "The updated application could not confirm a successful startup.");
        committed = true;
        std::error_code cleanup;
        if (backedUp) fs::remove_all(backup,cleanup);
        // Cleanup must never turn a committed healthy update into a rollback.
        // Windows retains only its running helper, which cannot unlink itself.
#ifdef _WIN32
        for(const auto &entry:fs::directory_iterator(t.workspace))
            if(entry.path().filename()!=L"introvert-update.exe")fs::remove_all(entry.path(),cleanup);
#else
        fs::remove_all(t.workspace,cleanup);
#endif
        return 0;
    } catch (const std::exception &) {
        if(!validated)return 1;
        if(committed)return 0;
        // Do not expose native process/trace details to the application UI.
        try {
            if (backedUp) {
                if (fs::exists(t.target)) fs::remove_all(t.target);
                fs::rename(backup, t.target);
#if defined(_WIN32) && defined(INTROVERT_VERSION)
                HKEY uninstall{};
                if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Introvert",0,KEY_SET_VALUE,&uninstall)==ERROR_SUCCESS) {
                    const auto previous=fs::u8path(INTROVERT_VERSION).wstring();
                    RegSetValueExW(uninstall,L"DisplayVersion",0,REG_SZ,reinterpret_cast<const BYTE *>(previous.c_str()),DWORD((previous.size()+1)*sizeof(wchar_t)));
                    RegCloseKey(uninstall);
                }
#endif
            }
            std::ofstream(t.workspace / "result.txt") << "The update failed. Introvert was reopened; try again from Settings.\n";
            if (parentExited && fs::exists(executable(t.target))) restartOld(t);
        } catch (...) {}
        return 1;
    }
}
} // namespace introvert::updates
