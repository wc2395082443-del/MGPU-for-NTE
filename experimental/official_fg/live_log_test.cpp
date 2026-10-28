#include <windows.h>
#include <cstdio>
#include <cstring>

int wmain()
{
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"mgl", 0, path)) return 1;
    if (!_wfreopen(path, L"w", stdout)) { DeleteFileW(path); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("live helper progress\n");
    HANDLE reader = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) { std::fclose(stdout); DeleteFileW(path); return 3; }
    char content[128]{};
    DWORD bytes = 0;
    const bool read = ReadFile(reader, content, sizeof(content) - 1, &bytes, nullptr) != FALSE;
    CloseHandle(reader);
    const bool correct = read && std::strstr(content, "live helper progress") != nullptr;
    std::fclose(stdout);
    DeleteFileW(path);
    return correct ? 0 : 4;
}
