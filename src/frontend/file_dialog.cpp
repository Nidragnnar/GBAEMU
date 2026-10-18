#include "frontend/file_dialog.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include <SDL.h>
#include <SDL_syswm.h>

std::optional<std::string> openRomDialog(SDL_Window* owner) {
    HWND hwnd = nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (owner && SDL_GetWindowWMInfo(owner, &info)) hwnd = info.info.win.window;

    wchar_t file[4096] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"ROMs de Game Boy Advance (*.gba;*.agb;*.bin)\0*.gba;*.agb;*.bin\0Todos os arquivos\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(sizeof file / sizeof file[0]);
    ofn.lpstrTitle = L"Abrir ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;

    // UTF-16 (Windows) -> UTF-8
    const int len = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::nullopt;
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file, -1, out.data(), len, nullptr, nullptr);
    return out;
}

#else

std::optional<std::string> openRomDialog(SDL_Window*) { return std::nullopt; }

#endif
