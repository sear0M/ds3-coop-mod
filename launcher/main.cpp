// DS3 Moraes e amigos - launcher
//
// Keeps every player on the same mod version:
//   1. downloads manifest.txt from the latest GitHub release,
//   2. updates itself, dinput8.dll and the Seamless Co-op settings when their SHA-256 differs,
//   3. "Iniciar jogo" opens Steam if needed and starts the game through ds3sc_launcher.exe.
// On open it checks for updates by itself; the window has buttons to update, start the game
// and open the Seamless Co-op settings file (where the co-op password is).
//
// Command line (mostly for testing):
//   --game "<dir>"    game folder (the one with DarkSoulsIII.exe)
//   --source "<url>"  where manifest.txt is (default: latest GitHub release)
//   --no-launch       update only, then close by itself (exit code 0/1)
//   --play            start the game right after the update (used after a self-update restart)
//   --updated         internal: set after a self-update restart

#include <windows.h>

#include <bcrypt.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// gdiplus.h uses min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef DS3_BUILD_VERSION
#define DS3_BUILD_VERSION "dev"
#endif

namespace {

constexpr wchar_t kAppName[] = L"DS3 Moraes e amigos";
constexpr wchar_t kLauncherVersion[] = L"" DS3_BUILD_VERSION;
constexpr wchar_t kDefaultSource[] = L"https://github.com/sear0M/ds3-coop-mod/releases/latest/download/";
constexpr wchar_t kModDll[] = L"dinput8.dll";
constexpr wchar_t kLauncherExe[] = L"DS3CoopLauncher.exe";
constexpr wchar_t kOverrides[] = L"seamless_overrides.ini";
constexpr char kModDllA[] = "dinput8.dll";
constexpr char kLauncherExeA[] = "DS3CoopLauncher.exe";
constexpr char kOverridesA[] = "seamless_overrides.ini";
constexpr size_t kMaxDownload = 64u << 20;

constexpr UINT WM_APP_START = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr UINT WM_APP_RESULT = WM_APP + 3;
constexpr UINT_PTR kCloseTimer = 1;
constexpr UINT_PTR kAnimTimer = 2;

enum class Result { Launched, Ready, Error, NeedElevation, Restarted };

struct ResultMsg {
    Result result;
    std::wstring text;
    bool networkError;
    bool launchAfter;
};

struct Options {
    std::wstring gameDir;
    std::wstring source = kDefaultSource;
    bool customSource = false;
    bool noLaunch = false;
    bool updated = false;
    bool play = false;
};

Options g_opt;
HWND g_wnd = nullptr;
int g_dpi = 96;
int g_exitCode = 0;
std::atomic<bool> g_busy{false};
FILE* g_log = nullptr;
thread_local bool t_launchAfter = false;  // what the current worker was asked to do

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fwprintf(g_log, L"[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    std::vfwprintf(g_log, fmt, args);
    va_end(args);
    std::fputwc(L'\n', g_log);
    std::fflush(g_log);
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring DirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return (dir.back() == L'\\' || dir.back() == L'/') ? dir + name : dir + L"\\" + name;
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    CloseHandle(f);
    return ok;
}

// Returns 0 on success, otherwise the Win32 error code.
DWORD WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    const bool ok = data.empty() || (WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                                     written == data.size());
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(f);
    return err;
}

bool Sha256(const std::string& data, std::string& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    const bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
                    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                   static_cast<ULONG>(data.size()), 0) >= 0 &&
                    BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    char buf[65];
    for (int i = 0; i < 32; ++i) std::snprintf(buf + i * 2, 3, "%02x", digest[i]);
    hex.assign(buf, 64);
    return true;
}

bool Sha256File(const std::wstring& path, std::string& hex) {
    std::string data;
    return ReadFileBytes(path, data) && Sha256(data, hex);
}

bool ProcessRunning(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, exeName) == 0;
    CloseHandle(snap);
    return found;
}

bool ReadExeVersion(const std::wstring& path, uint16_t v[4]) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return false;
    std::vector<unsigned char> buf(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return false;
    v[0] = HIWORD(info->dwFileVersionMS);
    v[1] = LOWORD(info->dwFileVersionMS);
    v[2] = HIWORD(info->dwFileVersionLS);
    v[3] = LOWORD(info->dwFileVersionLS);
    return true;
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// Arguments to pass on when we restart ourselves (self-update or elevation).
std::wstring RestartArgs(bool updated, bool play) {
    std::wstring args = L"--game " + Quote(g_opt.gameDir);
    if (g_opt.customSource) args += L" --source " + Quote(g_opt.source);
    if (g_opt.noLaunch) args += L" --no-launch";
    if (updated) args += L" --updated";
    if (play) args += L" --play";
    return args;
}

// ---------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------
using Progress = std::function<void(uint64_t done, uint64_t total)>;

bool HttpGet(const std::wstring& url, std::string& out, const Progress& progress, std::wstring& error) {
    out.clear();
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {}, extra[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = L"Endereço inválido: " + url;
        return false;
    }
    const std::wstring object = std::wstring(path) + extra;
    bool ok = false;
    HINTERNET session = WinHttpOpen(L"DS3CoopLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"DS3CoopLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = nullptr, request = nullptr;
    do {
        if (!session) break;
        WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
        connect = WinHttpConnect(session, host, uc.nPort, 0);
        if (!connect) break;
        request = WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request) break;
        const wchar_t* headers = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";
        if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
            break;
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            Log(L"HTTP %lu for %ls", status, url.c_str());
            error = status == 404 ? L"Versão do mod não encontrada no GitHub (o repositório está privado ou ainda não "
                                    L"tem nenhuma versão publicada)."
                                  : L"O GitHub respondeu com erro " + std::to_wstring(status) + L". Tente de novo.";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        DWORD total = 0;
        size = sizeof(total);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);
        bool readOk = true;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                readOk = false;
                break;
            }
            if (available == 0) break;
            const size_t offset = out.size();
            out.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.data() + offset, available, &read)) {
                readOk = false;
                break;
            }
            out.resize(offset + read);
            if (out.size() > kMaxDownload) {
                readOk = false;
                break;
            }
            if (progress) progress(out.size(), total);
        }
        ok = readOk && (total == 0 || out.size() == total);
    } while (false);
    if (!ok && error.empty()) {
        const DWORD err = GetLastError();
        Log(L"Network error %lu for %ls", err, url.c_str());
        error = L"Falha de rede (erro " + std::to_wstring(err) + L"). Confira sua internet e tente de novo.";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// manifest.txt and Seamless Co-op settings
// ---------------------------------------------------------------------------------------------
std::map<std::string, std::string> ParseManifest(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return out;
}

struct Override {
    std::string section;  // upper case, may be empty
    std::string key;
    std::string value;
};

std::vector<Override> ParseOverrides(const std::string& text) {
    std::vector<Override> out;
    std::string section;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = Upper(Trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.push_back({section, Trim(line.substr(0, eq)), Trim(line.substr(eq + 1))});
    }
    return out;
}

// Sets each key in its section, keeping everything else (comments, other keys) untouched.
bool ApplyOverrides(const std::wstring& iniPath, const std::vector<Override>& overrides, int& changed,
                    std::wstring& error) {
    changed = 0;
    std::string text;
    if (!ReadFileBytes(iniPath, text)) {
        error = L"Não consegui ler " + iniPath;
        return false;
    }
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;  // without line endings
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        start = end + 1;
    }

    for (const Override& o : overrides) {
        std::string section;
        int lastInSection = -1;  // last non-empty line of the wanted section
        bool sectionSeen = o.section.empty();
        bool found = false;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            const std::string t = Trim(lines[i]);
            if (!t.empty() && t[0] == '[') {
                const size_t close = t.find(']');
                section = Upper(Trim(t.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
                if (section == o.section) {
                    sectionSeen = true;
                    lastInSection = i;
                }
                continue;
            }
            if (section != o.section) continue;
            if (!t.empty()) lastInSection = i;
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos || Trim(t.substr(0, eq)) != o.key) continue;
            found = true;
            if (Trim(t.substr(eq + 1)) != o.value) {
                lines[i] = o.key + " = " + o.value;
                ++changed;
            }
        }
        if (!found) {
            if (sectionSeen && lastInSection >= 0) {
                lines.insert(lines.begin() + lastInSection + 1, o.key + " = " + o.value);
            } else {
                if (!o.section.empty()) {
                    lines.push_back("");
                    lines.push_back("[" + o.section + "]");
                }
                lines.push_back(o.key + " = " + o.value);
            }
            ++changed;
        }
    }
    if (changed == 0) return true;

    const std::wstring backup = iniPath + L".bak";
    if (!FileExists(backup)) CopyFileW(iniPath.c_str(), backup.c_str(), TRUE);
    std::string outText;
    for (const std::string& line : lines) outText += line + eol;
    if (const DWORD err = WriteFileBytes(iniPath, outText)) {
        error = L"Não consegui salvar " + iniPath + L" (erro " + std::to_wstring(err) + L")";
        SetLastError(err);  // the caller checks for ERROR_ACCESS_DENIED
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------------------------
void PostStatus(const std::wstring& text, int progress = -1) {
    Log(L"%ls", text.c_str());
    PostMessageW(g_wnd, WM_APP_STATUS, static_cast<WPARAM>(progress < 0 ? 0xFFFF : progress),
                 reinterpret_cast<LPARAM>(new std::wstring(text)));
}

void PostResult(Result result, const std::wstring& text, bool networkError = false) {
    Log(L"Result %d: %ls", static_cast<int>(result), text.c_str());
    PostMessageW(g_wnd, WM_APP_RESULT, 0,
                 reinterpret_cast<LPARAM>(new ResultMsg{result, text, networkError, t_launchAfter}));
}

Progress Range(int from, int to) {
    return [from, to](uint64_t done, uint64_t total) {
        if (total == 0) return;
        const int p = from + static_cast<int>((to - from) * static_cast<double>(done) / static_cast<double>(total));
        PostMessageW(g_wnd, WM_APP_STATUS, static_cast<WPARAM>(p), 0);
    };
}

bool DownloadVerified(const std::wstring& url, const std::string& wantHash, std::string& data, const Progress& progress,
                      std::wstring& error) {
    if (!HttpGet(url, data, progress, error)) return false;
    std::string hash;
    if (!Sha256(data, hash) || hash != wantHash) {
        Log(L"Hash mismatch for %ls: got %hs", url.c_str(), hash.c_str());
        error = L"O arquivo baixado veio corrompido (hash diferente). Tente de novo.";
        return false;
    }
    return true;
}

bool LaunchGame(const std::wstring& gameDir, std::wstring& error) {
    const std::wstring launcher = Join(gameDir, L"ds3sc_launcher.exe");
    if (!FileExists(launcher)) {
        error = L"Não achei o ds3sc_launcher.exe (Seamless Co-op) na pasta do jogo.";
        return false;
    }
    if (!ProcessRunning(L"steam.exe")) {
        PostStatus(L"Abrindo a Steam...", 96);
        ShellExecuteW(nullptr, L"open", L"steam://open/main", nullptr, nullptr, SW_SHOWNORMAL);
        for (int i = 0; i < 60 && !ProcessRunning(L"steam.exe"); ++i) Sleep(500);
        Sleep(4000);  // let it finish logging in
    }
    PostStatus(L"Abrindo o jogo...", 100);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"open";
    sei.lpFile = launcher.c_str();
    sei.lpDirectory = gameDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        error = L"Não consegui abrir o ds3sc_launcher.exe (erro " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    return true;
}

void UpdateWorker(Options opt, bool launchAfter) {
    t_launchAfter = launchAfter;
    std::wstring error;
    PostStatus(L"Procurando atualização...", 2);

    std::string manifestText;
    if (!HttpGet(opt.source + L"manifest.txt", manifestText, nullptr, error)) {
        PostResult(Result::Error, L"Não consegui ver a versão mais nova do mod. " + error, true);
        return;
    }
    auto manifest = ParseManifest(manifestText);
    const std::wstring version = Widen(manifest["version"]);
    std::wstring filesUrl = Widen(manifest["files_url"]);
    if (opt.customSource || filesUrl.empty()) filesUrl = opt.source;
    const std::string wantDll = manifest[kModDllA];
    const std::string wantLauncher = manifest[kLauncherExeA];
    const std::string wantOverrides = manifest[kOverridesA];
    if (version.empty() || wantDll.size() != 64) {
        PostResult(Result::Error, L"A lista de arquivos do mod (manifest.txt) veio inválida.");
        return;
    }
    PostMessageW(g_wnd, WM_APP_STATUS, 0xFFFF, 0);
    Log(L"Latest version: %ls", version.c_str());

    // 1. The launcher itself
    std::string selfHash;
    if (!opt.updated && wantLauncher.size() == 64 && Sha256File(ExePath(), selfHash) && selfHash != wantLauncher) {
        PostStatus(L"Atualizando o launcher...", 5);
        std::string data;
        if (!DownloadVerified(filesUrl + kLauncherExe, wantLauncher, data, Range(5, 20), error)) {
            PostResult(Result::Error, error);
            return;
        }
        const std::wstring self = ExePath(), fresh = self + L".new", old = self + L".old";
        if (const DWORD err = WriteFileBytes(fresh, data)) {
            if (err == ERROR_ACCESS_DENIED) PostResult(Result::NeedElevation, L"");
            else PostResult(Result::Error, L"Não consegui salvar o launcher novo (erro " + std::to_wstring(err) + L").");
            return;
        }
        DeleteFileW(old.c_str());
        if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) ||
            !MoveFileExW(fresh.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            PostResult(Result::Error, L"Não consegui trocar o launcher (erro " + std::to_wstring(GetLastError()) + L").");
            return;
        }
        std::wstring cmd = Quote(self) + L" " + RestartArgs(true, launchAfter);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, DirOf(self).c_str(), &si, &pi)) {
            PostResult(Result::Error, L"Launcher atualizado, abra ele de novo.");
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        PostResult(Result::Restarted, L"");
        return;
    }

    // 2. The mod
    const std::wstring dllPath = Join(opt.gameDir, kModDll);
    std::string localHash;
    Sha256File(dllPath, localHash);
    if (localHash != wantDll) {
        if (ProcessRunning(L"DarkSoulsIII.exe")) {
            PostResult(Result::Error, L"Feche o Dark Souls III antes de atualizar o mod.");
            return;
        }
        PostStatus(L"Baixando o mod " + version + L"...", 20);
        std::string data;
        if (!DownloadVerified(filesUrl + kModDll, wantDll, data, Range(20, 85), error)) {
            PostResult(Result::Error, error, true);
            return;
        }
        const std::wstring tmp = dllPath + L".download";
        DWORD err = WriteFileBytes(tmp, data);
        if (!err && !MoveFileExW(tmp.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
        if (err) {
            DeleteFileW(tmp.c_str());
            if (err == ERROR_ACCESS_DENIED) PostResult(Result::NeedElevation, L"");
            else PostResult(Result::Error, L"Não consegui instalar o mod na pasta do jogo (erro " + std::to_wstring(err) + L").");
            return;
        }
        PostStatus(L"Mod atualizado para a versão " + version + L".", 88);
    } else {
        Log(L"O mod já está na versão mais nova (%ls).", version.c_str());
    }

    // 3. Seamless Co-op settings
    std::wstring settingsNote;
    if (wantOverrides.size() == 64) {
        const std::wstring ini = Join(opt.gameDir, L"SeamlessCoop\\ds3sc_settings.ini");
        std::string data;
        if (!FileExists(ini)) {
            settingsNote = L"Seamless Co-op não encontrado: as configurações não foram aplicadas.";
        } else if (!DownloadVerified(filesUrl + kOverrides, wantOverrides, data, nullptr, error)) {
            PostResult(Result::Error, error);
            return;
        } else {
            int changed = 0;
            if (!ApplyOverrides(ini, ParseOverrides(data), changed, error)) {
                if (GetLastError() == ERROR_ACCESS_DENIED) PostResult(Result::NeedElevation, L"");
                else PostResult(Result::Error, error);
                return;
            }
            if (changed) PostStatus(L"Configurações do Seamless Co-op atualizadas.", 92);
        }
    }
    WriteFileBytes(Join(opt.gameDir, L"ds3coopmod_version.txt"),
                   "version=" + manifest["version"] + "\r\ncommit=" + manifest["commit"] + "\r\n");

    // 4. Game version check (the mod only gives items on 1.15.2)
    uint16_t v[4] = {};
    std::wstring versionNote;
    if (ReadExeVersion(Join(opt.gameDir, L"DarkSoulsIII.exe"), v) && !(v[0] == 1 && v[1] == 15 && v[2] == 2)) {
        versionNote = L"Atenção: seu jogo é a versão " + std::to_wstring(v[0]) + L"." + std::to_wstring(v[1]) + L"." +
                      std::to_wstring(v[2]) + L", o menu de itens só funciona na 1.15.2.";
        Log(L"%ls", versionNote.c_str());
    }

    if (!launchAfter) {
        std::wstring text = L"Tudo pronto. Mod na versão mais nova (" + version + L").";
        if (!settingsNote.empty()) text += L" " + settingsNote;
        if (!versionNote.empty()) text += L" " + versionNote;
        PostResult(Result::Ready, text);
        return;
    }
    if (!LaunchGame(opt.gameDir, error)) {
        PostResult(Result::Error, error);
        return;
    }
    PostResult(Result::Launched, versionNote.empty() ? settingsNote : versionNote);
}

void PlayWorker(std::wstring gameDir) {
    t_launchAfter = true;
    std::wstring error;
    if (LaunchGame(gameDir, error)) PostResult(Result::Launched, L"");
    else PostResult(Result::Error, error);
}

// ---------------------------------------------------------------------------------------------
// Game folder
// ---------------------------------------------------------------------------------------------
bool IsGameDir(const std::wstring& dir) { return !dir.empty() && FileExists(Join(dir, L"DarkSoulsIII.exe")); }

std::wstring ConfigFile() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = Join(appData, L"DS3CoopLauncher");
        CoTaskMemFree(appData);
    } else {
        dir = DirOf(ExePath());
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return Join(dir, L"game_dir.txt");
}

bool PickFolder(HWND owner, std::wstring& out) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Escolha a pasta onde está o DarkSoulsIII.exe");
    bool ok = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return ok;
}

bool ResolveGameDir(bool forcePick) {
    if (!forcePick) {
        if (IsGameDir(g_opt.gameDir)) return true;
        const std::wstring here = DirOf(ExePath());
        if (IsGameDir(here)) {
            g_opt.gameDir = here;
            return true;
        }
        std::string saved;
        if (ReadFileBytes(ConfigFile(), saved) && IsGameDir(Widen(Trim(saved)))) {
            g_opt.gameDir = Widen(Trim(saved));
            return true;
        }
        MessageBoxW(g_wnd,
                    L"Não achei o Dark Souls III.\n\nNa próxima janela, escolha a pasta onde está o DarkSoulsIII.exe.\n"
                    L"(Dica: dá para colocar o launcher dentro dessa pasta e ele acha sozinho.)",
                    kAppName, MB_OK | MB_ICONINFORMATION);
    }
    std::wstring picked;
    while (PickFolder(g_wnd, picked)) {
        if (!IsGameDir(picked) && IsGameDir(Join(picked, L"Game"))) picked = Join(picked, L"Game");  // Steam layout
        if (IsGameDir(picked)) {
            g_opt.gameDir = picked;
            WriteFileBytes(ConfigFile(), Narrow(picked));
            return true;
        }
        MessageBoxW(g_wnd, L"Essa pasta não tem o DarkSoulsIII.exe. Escolha outra.", kAppName, MB_OK | MB_ICONWARNING);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// UI: one custom-painted window in Dark Souls III colors (GDI+, double buffered)
// ---------------------------------------------------------------------------------------------
using namespace Gdiplus;

// Palette: ash, aged gold and bonfire ember on near-black.
const Color kBgTop(255, 27, 23, 18);
const Color kBgBottom(255, 8, 7, 6);
const Color kFrame(255, 66, 54, 38);
const Color kGold(255, 201, 163, 92);
const Color kGoldLight(255, 236, 211, 150);
const Color kGoldDark(255, 150, 116, 60);
const Color kAsh(255, 214, 204, 182);
const Color kAshDim(255, 140, 130, 112);
const Color kEmber(255, 226, 122, 48);
const Color kEmberDark(255, 140, 62, 24);
const Color kError(255, 230, 104, 70);

constexpr float kWidth = 620.0f, kHeight = 350.0f;  // logical size of the client area

enum Hit { HitNone = -1, HitUpdate = 0, HitPlay = 1, HitPassword = 2, HitFolder = 3, HitVideo = 4 };
const RectF kButtonRects[3] = {{24, 236, 166, 52}, {206, 236, 208, 52}, {430, 236, 166, 52}};
const RectF kVideoLink(150, 306, 150, 22);
const RectF kFolderLink(320, 306, 170, 22);

// Same defaults the mod writes on its first run (src/video.cpp).
constexpr const char* kDefaultVideoIni =
    "; DS3 Coop Mod - opcoes de video. Cada jogador tem o seu arquivo, o launcher nao mexe aqui.\r\n"
    "; Depois de mudar, feche e abra o jogo de novo.\r\n"
    "\r\n"
    "[Video]\r\n"
    "; Limite de FPS.\r\n"
    ";   0   = usa a taxa de atualizacao do monitor (ex.: 144 num monitor de 144 Hz)\r\n"
    ";   60  = igual ao jogo original\r\n"
    ";   120, 144, 165, 240... = o valor que voce escolher (maximo 360)\r\n"
    "fps_limit = 0\r\n"
    "\r\n"
    "; Tela cheia sem borda: 1 = ligado, 0 = desligado.\r\n"
    "; Para funcionar, deixe o jogo em modo \"Janela\" nas opcoes de video do jogo,\r\n"
    "; de preferencia com a resolucao do seu monitor.\r\n"
    "borderless = 1\r\n"
    "\r\n"
    "; Corrige a corrida mais lenta acima de ~82 FPS: 1 = ligado, 0 = desligado.\r\n"
    "fix_sprint = 1\r\n";

enum class StatusKind { Normal, Success, Error };
std::wstring g_statusText = L"Iniciando...";
StatusKind g_statusKind = StatusKind::Normal;
int g_progress = 0;
int g_hover = HitNone;
int g_pressed = HitNone;
bool g_tracking = false;
ULONG_PTR g_gdiplusToken = 0;

struct Fonts {
    std::unique_ptr<FontFamily> serif;
    std::unique_ptr<FontFamily> sans;
    std::unique_ptr<Font> title, subtitle, status, button, buttonBig, small;
} g_fonts;

struct Ember {
    float x, y, vx, vy, life, maxLife, size, phase;
};
std::vector<Ember> g_embers;
unsigned g_rng = 12345;

float Rand01() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return static_cast<float>((g_rng >> 8) & 0xFFFF) / 65535.0f;
}

float Scale() { return static_cast<float>(g_dpi) / 96.0f; }

void CreateFonts() {
    auto pick = [](std::initializer_list<const wchar_t*> names) {
        for (const wchar_t* n : names) {
            auto f = std::make_unique<FontFamily>(n);
            if (f->IsAvailable()) return f;
        }
        return std::make_unique<FontFamily>(L"Times New Roman");
    };
    g_fonts.serif = pick({L"Palatino Linotype", L"Book Antiqua", L"Georgia", L"Cambria"});
    g_fonts.sans = pick({L"Segoe UI", L"Tahoma"});
    const float s = Scale();
    g_fonts.title = std::make_unique<Font>(g_fonts.serif.get(), 34.0f * s, FontStyleRegular, UnitPixel);
    g_fonts.subtitle = std::make_unique<Font>(g_fonts.serif.get(), 14.0f * s, FontStyleItalic, UnitPixel);
    g_fonts.status = std::make_unique<Font>(g_fonts.serif.get(), 15.5f * s, FontStyleRegular, UnitPixel);
    g_fonts.button = std::make_unique<Font>(g_fonts.serif.get(), 16.0f * s, FontStyleRegular, UnitPixel);
    g_fonts.buttonBig = std::make_unique<Font>(g_fonts.serif.get(), 19.0f * s, FontStyleBold, UnitPixel);
    g_fonts.small = std::make_unique<Font>(g_fonts.sans.get(), 11.5f * s, FontStyleRegular, UnitPixel);
}

RectF Scaled(const RectF& r) {
    const float s = Scale();
    return RectF(r.X * s, r.Y * s, r.Width * s, r.Height * s);
}

void RoundRect(GraphicsPath& path, const RectF& r, float radius) {
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void DrawCentered(Graphics& g, const std::wstring& text, const Font& font, const RectF& rect, const Brush& brush,
                  StringTrimming trimming = StringTrimmingEllipsisCharacter, bool singleLine = false) {
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(trimming);
    if (singleLine) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), -1, &font, rect, &fmt, &brush);
}

// A thin gold line that fades at both ends, with a small diamond in the middle.
void DrawDivider(Graphics& g, float y, float x0, float x1) {
    const float s = Scale(), cx = (x0 + x1) * 0.5f * s, yy = y * s;
    const Color clear(0, kGold.GetR(), kGold.GetG(), kGold.GetB());
    LinearGradientBrush left(PointF(x0 * s, yy), PointF(cx - 10 * s, yy), clear, kGoldDark);
    LinearGradientBrush right(PointF(cx + 10 * s, yy), PointF(x1 * s, yy), kGoldDark, clear);
    g.FillRectangle(&left, RectF(x0 * s, yy - 0.5f * s, cx - 10 * s - x0 * s, 1.0f * s));
    g.FillRectangle(&right, RectF(cx + 10 * s, yy - 0.5f * s, x1 * s - cx - 10 * s, 1.0f * s));
    PointF diamond[4] = {{cx, yy - 5 * s}, {cx + 5 * s, yy}, {cx, yy + 5 * s}, {cx - 5 * s, yy}};
    SolidBrush gold(kGold);
    g.FillPolygon(&gold, diamond, 4);
}

bool HitEnabled(int hit) {
    if (hit == HitPassword || hit == HitVideo) return true;
    return !g_busy;
}

void DrawButton(Graphics& g, int index, const wchar_t* label, bool primary) {
    const RectF r = Scaled(kButtonRects[index]);
    const bool enabled = HitEnabled(index);
    const bool hover = enabled && g_hover == index;
    const bool pressed = hover && g_pressed == index;
    const float s = Scale();

    if (primary && enabled) {  // ember glow behind the main button
        GraphicsPath glowPath;
        glowPath.AddEllipse(RectF(r.X - 30 * s, r.Y - 22 * s, r.Width + 60 * s, r.Height + 44 * s));
        PathGradientBrush glow(&glowPath);
        glow.SetCenterColor(Color(hover ? 90 : 55, 230, 120, 40));
        Color surround(0, 230, 120, 40);
        int count = 1;
        glow.SetSurroundColors(&surround, &count);
        g.FillPath(&glow, &glowPath);
    }

    GraphicsPath path;
    RoundRect(path, r, 3.0f * s);
    Color top, bottom, border, text;
    if (!enabled) {
        top = Color(255, 28, 25, 21);
        bottom = Color(255, 20, 18, 15);
        border = Color(255, 52, 45, 36);
        text = Color(255, 96, 89, 77);
    } else if (primary) {
        top = pressed ? Color(255, 70, 36, 16) : hover ? Color(255, 92, 48, 20) : Color(255, 74, 40, 18);
        bottom = pressed ? Color(255, 44, 22, 10) : Color(255, 38, 20, 10);
        border = hover ? Color(255, 244, 170, 92) : kEmber;
        text = hover ? Color(255, 255, 236, 196) : Color(255, 246, 220, 168);
    } else {
        top = pressed ? Color(255, 30, 26, 20) : hover ? Color(255, 44, 37, 27) : Color(255, 34, 29, 23);
        bottom = Color(255, 21, 18, 14);
        border = hover ? kGold : Color(255, 98, 80, 52);
        text = hover ? kGoldLight : kAsh;
    }
    LinearGradientBrush fill(PointF(0, r.Y), PointF(0, r.Y + r.Height), top, bottom);
    g.FillPath(&fill, &path);
    Pen pen(border, (primary ? 1.6f : 1.1f) * s);
    g.DrawPath(&pen, &path);
    // inner highlight line for a bevelled look
    Pen inner(Color(enabled ? 40 : 15, 255, 230, 180), 1.0f);
    g.DrawLine(&inner, PointF(r.X + 4 * s, r.Y + 1.5f * s), PointF(r.X + r.Width - 4 * s, r.Y + 1.5f * s));

    SolidBrush brush(text);
    RectF textRect = r;
    if (pressed) textRect.Y += 1.0f * s;
    DrawCentered(g, label, primary ? *g_fonts.buttonBig : *g_fonts.button, textRect, brush);
}

std::wstring InfoLine() {
    std::wstring installed;
    std::string info;
    if (ReadFileBytes(Join(g_opt.gameDir, L"ds3coopmod_version.txt"), info))
        installed = Widen(ParseManifest(info)["version"]);
    std::wstring text = L"Mod instalado: " + (installed.empty() ? std::wstring(L"nenhum") : installed);
    text += L"     Pasta do jogo: " + (g_opt.gameDir.empty() ? std::wstring(L"?") : g_opt.gameDir);
    return text;
}

void Paint(HDC target) {
    RECT rc{};
    GetClientRect(g_wnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        const float s = Scale();
        const float W = static_cast<float>(w), H = static_cast<float>(h);

        // Background: dark gradient, bonfire glow from below, rising embers.
        LinearGradientBrush bg(PointF(0, 0), PointF(0, H), kBgTop, kBgBottom);
        g.FillRectangle(&bg, 0.0f, 0.0f, W, H);
        GraphicsPath glowPath;
        glowPath.AddEllipse(RectF(W * 0.5f - 330 * s, H - 110 * s, 660 * s, 300 * s));
        PathGradientBrush glow(&glowPath);
        glow.SetCenterColor(Color(80, 210, 100, 30));
        Color surround(0, 210, 100, 30);
        int count = 1;
        glow.SetSurroundColors(&surround, &count);
        g.FillPath(&glow, &glowPath);
        for (const Ember& e : g_embers) {
            const float t = e.life / e.maxLife;
            const float alpha = std::sin(t * 3.14159f) * 255.0f;
            SolidBrush halo(Color(static_cast<BYTE>(alpha * 0.16f), 255, 130, 40));
            SolidBrush core(Color(static_cast<BYTE>(alpha), 255, 205, 120));
            const float hr = e.size * 1.7f * s, cr = e.size * 0.55f * s;
            g.FillEllipse(&halo, e.x * s - hr, e.y * s - hr, hr * 2.0f, hr * 2.0f);
            g.FillEllipse(&core, e.x * s - cr, e.y * s - cr, cr * 2.0f, cr * 2.0f);
        }

        // Frame with corner ornaments.
        Pen frame(kFrame, 1.0f * s);
        const RectF fr(10 * s, 10 * s, W - 20 * s, H - 20 * s);
        g.DrawRectangle(&frame, fr);
        Pen ornament(kGoldDark, 1.4f * s);
        const float o = 14 * s;
        g.DrawLine(&ornament, fr.X, fr.Y, fr.X + o, fr.Y);
        g.DrawLine(&ornament, fr.X, fr.Y, fr.X, fr.Y + o);
        g.DrawLine(&ornament, fr.X + fr.Width, fr.Y, fr.X + fr.Width - o, fr.Y);
        g.DrawLine(&ornament, fr.X + fr.Width, fr.Y, fr.X + fr.Width, fr.Y + o);
        g.DrawLine(&ornament, fr.X, fr.Y + fr.Height, fr.X + o, fr.Y + fr.Height);
        g.DrawLine(&ornament, fr.X, fr.Y + fr.Height, fr.X, fr.Y + fr.Height - o);
        g.DrawLine(&ornament, fr.X + fr.Width, fr.Y + fr.Height, fr.X + fr.Width - o, fr.Y + fr.Height);
        g.DrawLine(&ornament, fr.X + fr.Width, fr.Y + fr.Height, fr.X + fr.Width, fr.Y + fr.Height - o);

        // Title with a soft shadow and a gold gradient.
        const RectF titleRect = Scaled(RectF(0, 26, kWidth, 52));
        SolidBrush shadow(Color(170, 0, 0, 0));
        RectF shadowRect = titleRect;
        shadowRect.Offset(0, 2.0f * s);
        DrawCentered(g, kAppName, *g_fonts.title, shadowRect, shadow);
        LinearGradientBrush titleBrush(PointF(0, titleRect.Y), PointF(0, titleRect.Y + titleRect.Height), kGoldLight,
                                       kGoldDark);
        DrawCentered(g, kAppName, *g_fonts.title, titleRect, titleBrush);

        SolidBrush dim(kAshDim);
        DrawCentered(g, std::wstring(L"Seamless Co-op   •   Launcher ") + kLauncherVersion, *g_fonts.subtitle,
                     Scaled(RectF(0, 80, kWidth, 22)), dim);
        DrawDivider(g, 114, 70, kWidth - 70);

        // Status, progress and info.
        const Color statusColor = g_statusKind == StatusKind::Error     ? kError
                                  : g_statusKind == StatusKind::Success ? kGold
                                                                        : kAsh;
        SolidBrush statusBrush(statusColor);
        DrawCentered(g, g_statusText, *g_fonts.status, Scaled(RectF(40, 126, kWidth - 80, 50)), statusBrush,
                     StringTrimmingEllipsisWord);

        const RectF track = Scaled(RectF(120, 184, kWidth - 240, 4));
        SolidBrush trackBrush(Color(255, 40, 34, 26));
        g.FillRectangle(&trackBrush, track);
        if (g_progress > 0) {
            RectF fill = track;
            fill.Width = track.Width * static_cast<float>(std::min(g_progress, 100)) / 100.0f;
            LinearGradientBrush fillBrush(PointF(track.X, 0), PointF(track.X + track.Width, 0), kEmberDark,
                                          Color(255, 240, 170, 90));
            g.FillRectangle(&fillBrush, fill);
        }
        DrawCentered(g, InfoLine(), *g_fonts.small, Scaled(RectF(30, 198, kWidth - 60, 20)), dim,
                     StringTrimmingEllipsisPath, true);

        // Buttons.
        DrawButton(g, HitUpdate, L"Atualizar", false);
        DrawButton(g, HitPlay, L"Iniciar jogo", true);
        DrawButton(g, HitPassword, L"Senha do coop", false);

        const Color linkIdle(255, 112, 104, 90);
        SolidBrush videoLink(g_hover == HitVideo ? kGold : linkIdle);
        DrawCentered(g, L"Opções de vídeo", *g_fonts.small, Scaled(kVideoLink), videoLink);
        SolidBrush dot(Color(255, 80, 72, 60));
        DrawCentered(g, L"•", *g_fonts.small, Scaled(RectF(300, 306, 20, 22)), dot);
        SolidBrush folderLink(HitEnabled(HitFolder) && g_hover == HitFolder ? kGold : linkIdle);
        DrawCentered(g, L"Trocar pasta do jogo", *g_fonts.small, Scaled(kFolderLink), folderLink);
    }
    BitBlt(target, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void Redraw() { InvalidateRect(g_wnd, nullptr, FALSE); }

void SetStatus(const std::wstring& text, StatusKind kind) {
    g_statusText = text;
    g_statusKind = kind;
    Redraw();
}

int HitTest(LPARAM lParam) {
    const float s = Scale();
    const PointF p(static_cast<float>(static_cast<short>(LOWORD(lParam))) / s,
                   static_cast<float>(static_cast<short>(HIWORD(lParam))) / s);
    for (int i = 0; i < 3; ++i)
        if (kButtonRects[i].Contains(p)) return i;
    if (kFolderLink.Contains(p)) return HitFolder;
    if (kVideoLink.Contains(p)) return HitVideo;
    return HitNone;
}

void UpdateEmbers() {
    // Embers rise from the bottom edge and die out before reaching the buttons.
    while (g_embers.size() < 18) {
        Ember e{};
        e.x = kWidth * 0.5f + (Rand01() - 0.5f) * 540.0f;
        e.y = kHeight - 14.0f - Rand01() * 6.0f;
        e.vx = (Rand01() - 0.5f) * 0.2f;
        e.vy = -(0.25f + Rand01() * 0.45f);
        e.maxLife = 50.0f + Rand01() * 70.0f;
        e.life = Rand01() * 20.0f;
        e.size = 1.4f + Rand01() * 1.4f;
        e.phase = Rand01() * 6.28f;
        g_embers.push_back(e);
    }
    for (Ember& e : g_embers) {
        e.life += 1.0f;
        e.x += e.vx + std::sin(e.life * 0.05f + e.phase) * 0.18f;
        e.y += e.vy;
    }
    g_embers.erase(std::remove_if(g_embers.begin(), g_embers.end(),
                                  [](const Ember& e) { return e.life >= e.maxLife || e.y < 12.0f; }),
                   g_embers.end());
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------
void StartUpdate(bool launchAfter) {
    if (g_busy.exchange(true)) return;
    g_progress = 0;
    SetStatus(launchAfter ? L"Conferindo atualização antes de abrir o jogo..." : L"Procurando atualização...",
              StatusKind::Normal);
    std::thread(UpdateWorker, g_opt, launchAfter).detach();
}

void StartPlayWithoutUpdate() {
    if (g_busy.exchange(true)) return;
    SetStatus(L"Abrindo o jogo...", StatusKind::Normal);
    std::thread(PlayWorker, g_opt.gameDir).detach();
}

void OpenPasswordFile() {
    const std::wstring ini = Join(g_opt.gameDir, L"SeamlessCoop\\ds3sc_settings.ini");
    if (!FileExists(ini)) {
        MessageBoxW(g_wnd,
                    L"Não achei o arquivo do Seamless Co-op:\nSeamlessCoop\\ds3sc_settings.ini\n\n"
                    L"Confira se o Seamless Co-op está instalado na pasta do jogo.",
                    kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    // Notepad: .ini files often have no app associated.
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_wnd, L"open", L"notepad.exe", Quote(ini).c_str(), nullptr,
                                                SW_SHOWNORMAL)) <= 32)
        ShellExecuteW(g_wnd, L"open", ini.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    SetStatus(L"Abri o ds3sc_settings.ini: a senha é a linha \"cooppassword\". Todo mundo precisa usar a mesma.",
              StatusKind::Normal);
}

void OpenVideoSettings() {
    if (!IsGameDir(g_opt.gameDir)) return;
    const std::wstring ini = Join(g_opt.gameDir, L"ds3coopmod.ini");
    if (!FileExists(ini)) WriteFileBytes(ini, kDefaultVideoIni);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_wnd, L"open", L"notepad.exe", Quote(ini).c_str(), nullptr,
                                                SW_SHOWNORMAL)) <= 32)
        ShellExecuteW(g_wnd, L"open", ini.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    SetStatus(L"Abri o ds3coopmod.ini: limite de FPS e tela cheia sem borda. Vale quando o jogo abrir de novo.",
              StatusKind::Normal);
}

void OnClick(int hit) {
    if (!HitEnabled(hit)) return;
    switch (hit) {
        case HitUpdate: StartUpdate(false); break;
        case HitPlay: StartUpdate(true); break;
        case HitPassword: OpenPasswordFile(); break;
        case HitVideo: OpenVideoSettings(); break;
        case HitFolder:
            if (ResolveGameDir(true)) StartUpdate(false);
            break;
        default: break;
    }
}

void OnResult(ResultMsg* msg) {
    const ResultMsg r = *msg;
    delete msg;
    g_busy = false;
    switch (r.result) {
        case Result::Launched:
            g_progress = 100;
            SetStatus(r.text.empty() ? L"Jogo aberto. Que a chama te guie!" : L"Jogo aberto. " + r.text,
                      StatusKind::Success);
            SetTimer(g_wnd, kCloseTimer, r.text.empty() ? 2500 : 8000, nullptr);
            break;
        case Result::Ready:
            g_progress = 100;
            SetStatus(r.text, StatusKind::Success);
            if (g_opt.noLaunch) SetTimer(g_wnd, kCloseTimer, 800, nullptr);
            break;
        case Result::Restarted:
            DestroyWindow(g_wnd);
            break;
        case Result::NeedElevation: {
            SetStatus(L"Precisa de permissão de administrador para escrever na pasta do jogo...", StatusKind::Normal);
            const std::wstring self = ExePath(), args = RestartArgs(g_opt.updated, r.launchAfter);
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"runas";
            sei.lpFile = self.c_str();
            sei.lpParameters = args.c_str();
            sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei)) {
                DestroyWindow(g_wnd);
            } else {
                g_exitCode = 1;
                SetStatus(L"Sem permissão de administrador não dá para atualizar o mod nessa pasta.", StatusKind::Error);
            }
            break;
        }
        case Result::Error:
        default:
            g_progress = 0;
            g_exitCode = 1;
            SetStatus(r.text, StatusKind::Error);
            if (g_opt.noLaunch) {
                SetTimer(g_wnd, kCloseTimer, 800, nullptr);
            } else if (r.launchAfter && r.networkError && FileExists(Join(g_opt.gameDir, kModDll))) {
                if (MessageBoxW(g_wnd,
                                L"Não consegui conferir se tem atualização (sem internet ou GitHub fora do ar).\n\n"
                                L"Abrir o jogo com a versão do mod que já está instalada?",
                                kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    g_exitCode = 0;
                    StartPlayWithoutUpdate();
                }
            }
            break;
    }
    Redraw();
}

void ApplyDarkTitleBar(HWND hwnd) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(22, 19, 15), text = RGB(201, 163, 92), border = RGB(66, 54, 38);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_wnd = hwnd;
            ApplyDarkTitleBar(hwnd);
            SetTimer(hwnd, kAnimTimer, 33, nullptr);
            PostMessageW(hwnd, WM_APP_START, 0, 0);
            return 0;
        case WM_APP_START:
            if (!ResolveGameDir(false)) {
                g_exitCode = 1;
                SetStatus(L"Escolha a pasta do jogo para continuar (\"Trocar pasta do jogo\").", StatusKind::Error);
                if (g_opt.noLaunch) DestroyWindow(hwnd);
                return 0;
            }
            Log(L"Game folder: %ls", g_opt.gameDir.c_str());
            StartUpdate(g_opt.play);
            return 0;
        case WM_APP_STATUS:
            if (wParam != 0xFFFF) g_progress = static_cast<int>(wParam);
            if (lParam) {
                auto* text = reinterpret_cast<std::wstring*>(lParam);
                g_statusText = *text;
                g_statusKind = StatusKind::Normal;
                delete text;
            }
            Redraw();
            return 0;
        case WM_APP_RESULT:
            OnResult(reinterpret_cast<ResultMsg*>(lParam));
            return 0;
        case WM_TIMER:
            if (wParam == kCloseTimer) {
                DestroyWindow(hwnd);
            } else if (wParam == kAnimTimer && !IsIconic(hwnd)) {
                UpdateEmbers();
                Redraw();
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                g_tracking = TrackMouseEvent(&tme) != FALSE;
            }
            const int hit = HitTest(lParam);
            if (hit != g_hover) {
                g_hover = hit;
                Redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hover = HitNone;
            g_pressed = HitNone;
            Redraw();
            return 0;
        case WM_LBUTTONDOWN:
            g_pressed = HitTest(lParam);
            SetCapture(hwnd);
            Redraw();
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int hit = HitTest(lParam);
            const int pressed = g_pressed;
            g_pressed = HitNone;
            Redraw();
            if (hit != HitNone && hit == pressed) OnClick(hit);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                const int hit = HitTest(MAKELPARAM(p.x, p.y));
                SetCursor(LoadCursorW(nullptr, hit != HitNone && HitEnabled(hit) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) OnClick(HitPlay);
            else if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kAnimTimer);
            PostQuitMessage(g_exitCode);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ParseArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc) g_opt.gameDir = argv[++i];
        else if (a == L"--source" && i + 1 < argc) {
            g_opt.source = argv[++i];
            if (!g_opt.source.empty() && g_opt.source.back() != L'/') g_opt.source += L'/';
            g_opt.customSource = true;
        } else if (a == L"--no-launch") g_opt.noLaunch = true;
        else if (a == L"--updated") g_opt.updated = true;
        else if (a == L"--play") g_opt.play = true;
    }
    if (g_opt.noLaunch) g_opt.play = false;
    LocalFree(argv);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    ParseArgs();
    g_log = _wfopen(Join(DirOf(ExePath()), L"DS3CoopLauncher.log").c_str(), L"w, ccs=UTF-8");
    Log(L"DS3 Moraes e amigos launcher %ls, exe=%ls", kLauncherVersion, ExePath().c_str());

    // Leftover from a self-update: the old exe may still be closing.
    const std::wstring old = ExePath() + L".old";
    for (int i = 0; i < 20 && FileExists(old) && !DeleteFileW(old.c_str()); ++i) Sleep(100);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gdiplusInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
    g_dpi = static_cast<int>(GetDpiForSystem());
    CreateFonts();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"DS3MoraesLauncher";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const float s = Scale();
    RECT rc{0, 0, static_cast<LONG>(kWidth * s), static_cast<LONG>(kHeight * s)};
    AdjustWindowRect(&rc, style, FALSE);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kAppName, style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                                (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, g_opt.noLaunch ? SW_SHOWMINNOACTIVE : show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_fonts = Fonts{};
    GdiplusShutdown(g_gdiplusToken);
    if (g_log) std::fclose(g_log);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
