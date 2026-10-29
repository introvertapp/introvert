#include "install_transaction.h"
#include <iostream>

#ifdef _WIN32
int wmain(int count, wchar_t **args)
#else
int main(int count, char **args)
#endif
{
    try {
        if (count != 7 && count != 9) return 2;
        introvert::updates::InstallTransaction t;
        const auto pid = std::filesystem::path(args[1]).u8string();
        t.parentPid = std::stoll(pid);
        t.target = args[2]; t.payload = args[3]; t.workspace = args[4];
        t.token = std::filesystem::path(args[5]).u8string();
        t.version = std::filesystem::path(args[6]).u8string();
        if(count==9) {
            if(std::filesystem::path(args[7]).u8string()!="--data-dir")return 2;
            t.restartArguments={"--data-dir",std::filesystem::path(args[8]).u8string()};
        }
        return introvert::updates::installTransaction(t);
    } catch (...) { return 2; }
}
