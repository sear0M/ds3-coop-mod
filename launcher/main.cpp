// DS3 Coop Launcher
//
// Keeps every player on the same mod version:
//   1. downloads manifest.txt from the latest GitHub release,
//   2. updates itself, dinput8.dll and the Seamless Co-op settings when their SHA-256 differs,
//   3. opens Steam if needed and starts the game through ds3sc_launcher.exe.
//
// Command line (mostly for testing):
//   --game "<dir>"    game folder (the one with DarkSoulsIII.exe)
//   --source "<url>"  where manifest.txt is (default: latest GitHub release)
//   --no-launch       update only, do not start the game, close by itself (exit code 0/1)
//   --updated         internal: set after a self-update restart

#include <windows.h>

#include <bcrypt.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace {

constexpr wchar_t kAppName[] = L"DS3 Coop Mod";
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

enum Control { IdRetry = 101, IdPlay = 102, IdFolder = 103 };
enum class Result { Launched, Ready, Error, NeedElevation, Restarted };

struct Options {
    std::wstring gameDir;
    std::wstring source = kDefaultSource;
    bool customSource = false;
    bool noLaunch = false;
    bool updated = false;
};

Options g_opt;
HWND g_wnd = nullptr, g_title = nullptr, g_status = nullptr, g_progress = nullptr, g_version = nullptr;
HWND g_btnRetry = nullptr, g_btnPlay = nullptr, g_btnFolder = nullptr;
HFONT g_font = nullptr, g_titleFont = nullptr, g_smallFont = nullptr;
int g_dpi = 96;
int g_exitCode = 0;
std::atomic<bool> g_busy{false};
FILE* g_log = nullptr;

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
int S(int v) { return MulDiv(v, g_dpi, 96); }

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
std::wstring RestartArgs(bool updated) {
    std::wstring args = L"--game " + Quote(g_opt.gameDir);
    if (g_opt.customSource) args += L" --source " + Quote(g_opt.source);
    if (g_opt.noLaunch) args += L" --no-launch";
    if (updated) args += L" --updated";
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

void PostResult(Result result, const std::wstring& text) {
    Log(L"Result %d: %ls", static_cast<int>(result), text.c_str());
    PostMessageW(g_wnd, WM_APP_RESULT, static_cast<WPARAM>(result), reinterpret_cast<LPARAM>(new std::wstring(text)));
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

void UpdateWorker(Options opt) {
    std::wstring error;
    PostStatus(L"Procurando atualização...", 2);

    std::string manifestText;
    if (!HttpGet(opt.source + L"manifest.txt", manifestText, nullptr, error)) {
        PostResult(Result::Error, L"Não consegui ver a versão mais nova do mod.\n" + error);
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
        std::wstring cmd = Quote(self) + L" " + RestartArgs(true);
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
            PostResult(Result::Error, error);
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
        PostStatus(L"O mod já está na versão mais nova (" + version + L").", 88);
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

    if (opt.noLaunch) {
        PostResult(Result::Ready, L"Tudo atualizado (" + version + L"). " + settingsNote + versionNote);
        return;
    }
    if (!LaunchGame(opt.gameDir, error)) {
        PostResult(Result::Error, error);
        return;
    }
    PostResult(Result::Launched, versionNote.empty() ? settingsNote : versionNote);
}

void PlayWorker(std::wstring gameDir) {
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
// UI
// ---------------------------------------------------------------------------------------------
void ShowButtons(bool show) {
    const int cmd = show ? SW_SHOW : SW_HIDE;
    ShowWindow(g_btnRetry, cmd);
    ShowWindow(g_btnFolder, cmd);
    const bool canPlay = show && IsGameDir(g_opt.gameDir) && FileExists(Join(g_opt.gameDir, kModDll)) &&
                         FileExists(Join(g_opt.gameDir, L"ds3sc_launcher.exe"));
    ShowWindow(g_btnPlay, canPlay ? SW_SHOW : SW_HIDE);
}

void SetStatusText(const std::wstring& text) { SetWindowTextW(g_status, text.c_str()); }

void UpdateVersionLabel() {
    std::string info;
    std::wstring text = L"Pasta do jogo: " + (g_opt.gameDir.empty() ? std::wstring(L"?") : g_opt.gameDir);
    if (ReadFileBytes(Join(g_opt.gameDir, L"ds3coopmod_version.txt"), info)) {
        const std::wstring version = Widen(ParseManifest(info)["version"]);
        if (!version.empty()) text = L"Mod instalado: " + version + L"   |   " + text;
    }
    SetWindowTextW(g_version, text.c_str());
}

void StartUpdate() {
    if (g_busy.exchange(true)) return;
    ShowButtons(false);
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    UpdateVersionLabel();
    std::thread(UpdateWorker, g_opt).detach();
}

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

void CreateFonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    g_font = CreateFontIndirectW(&lf);
    LOGFONTW small = lf;
    small.lfHeight = lf.lfHeight * 85 / 100;
    g_smallFont = CreateFontIndirectW(&small);
    LOGFONTW title = lf;
    title.lfHeight = lf.lfHeight * 17 / 10;
    title.lfWeight = FW_BOLD;
    g_titleFont = CreateFontIndirectW(&title);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_wnd = hwnd;
            g_title = MakeControl(L"STATIC", kAppName, SS_LEFT, 20, 14, 440, 34, 0, g_titleFont);
            g_status = MakeControl(L"STATIC", L"Iniciando...", SS_LEFT, 20, 54, 440, 46, 0, g_font);
            g_progress = MakeControl(PROGRESS_CLASSW, L"", 0, 20, 104, 440, 16, 0, g_font);
            SendMessageW(g_progress, PBM_SETRANGE32, 0, 100);
            g_version = MakeControl(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 20, 126, 440, 20, 0, g_smallFont);
            g_btnRetry = MakeControl(L"BUTTON", L"Tentar de novo", BS_PUSHBUTTON, 20, 154, 130, 30, IdRetry, g_font);
            g_btnPlay = MakeControl(L"BUTTON", L"Jogar sem atualizar", BS_PUSHBUTTON, 160, 154, 160, 30, IdPlay, g_font);
            g_btnFolder = MakeControl(L"BUTTON", L"Trocar pasta", BS_PUSHBUTTON, 330, 154, 130, 30, IdFolder, g_font);
            ShowButtons(false);
            PostMessageW(hwnd, WM_APP_START, 0, 0);
            return 0;
        }
        case WM_APP_START:
            if (!ResolveGameDir(false)) {
                SetStatusText(L"Escolha a pasta do jogo para continuar.");
                ShowButtons(true);
                ShowWindow(g_btnRetry, SW_HIDE);
                g_exitCode = 1;
                if (g_opt.noLaunch) DestroyWindow(hwnd);
                return 0;
            }
            Log(L"Game folder: %ls", g_opt.gameDir.c_str());
            StartUpdate();
            return 0;
        case WM_APP_STATUS: {
            if (wParam != 0xFFFF) SendMessageW(g_progress, PBM_SETPOS, wParam, 0);
            if (lParam) {
                auto* text = reinterpret_cast<std::wstring*>(lParam);
                SetStatusText(*text);
                delete text;
            }
            return 0;
        }
        case WM_APP_RESULT: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            const std::wstring message = text ? *text : std::wstring();
            delete text;
            g_busy = false;
            UpdateVersionLabel();
            switch (static_cast<Result>(wParam)) {
                case Result::Launched:
                    SetStatusText(message.empty() ? L"Jogo aberto. Bom jogo!" : L"Jogo aberto. " + message);
                    SendMessageW(g_progress, PBM_SETPOS, 100, 0);
                    SetTimer(hwnd, kCloseTimer, message.empty() ? 2500 : 8000, nullptr);
                    break;
                case Result::Ready:
                    SetStatusText(message);
                    SendMessageW(g_progress, PBM_SETPOS, 100, 0);
                    SetTimer(hwnd, kCloseTimer, 800, nullptr);
                    break;
                case Result::Restarted:
                    DestroyWindow(hwnd);
                    break;
                case Result::NeedElevation: {
                    SetStatusText(L"Precisa de permissão de administrador para escrever na pasta do jogo...");
                    const std::wstring self = ExePath(), args = RestartArgs(g_opt.updated);
                    SHELLEXECUTEINFOW sei{};
                    sei.cbSize = sizeof(sei);
                    sei.lpVerb = L"runas";
                    sei.lpFile = self.c_str();
                    sei.lpParameters = args.c_str();
                    sei.nShow = SW_SHOWNORMAL;
                    if (ShellExecuteExW(&sei)) {
                        DestroyWindow(hwnd);
                    } else {
                        SetStatusText(L"Sem permissão de administrador não dá para atualizar o mod nessa pasta.");
                        g_exitCode = 1;
                        ShowButtons(true);
                    }
                    break;
                }
                case Result::Error:
                default:
                    SetStatusText(message);
                    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
                    g_exitCode = 1;
                    ShowButtons(true);
                    if (g_opt.noLaunch) SetTimer(hwnd, kCloseTimer, 800, nullptr);
                    break;
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IdRetry:
                    g_exitCode = 0;
                    StartUpdate();
                    break;
                case IdPlay:
                    if (!g_busy.exchange(true)) {
                        ShowButtons(false);
                        std::thread(PlayWorker, g_opt.gameDir).detach();
                    }
                    break;
                case IdFolder:
                    if (!g_busy && ResolveGameDir(true)) {
                        g_exitCode = 0;
                        StartUpdate();
                    }
                    break;
            }
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            if (reinterpret_cast<HWND>(lParam) == g_version) SetTextColor(dc, RGB(110, 110, 110));
            if (reinterpret_cast<HWND>(lParam) == g_title) SetTextColor(dc, RGB(120, 70, 20));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_TIMER:
            if (wParam == kCloseTimer) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
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
    }
    LocalFree(argv);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    ParseArgs();
    g_log = _wfopen(Join(DirOf(ExePath()), L"DS3CoopLauncher.log").c_str(), L"w, ccs=UTF-8");
    Log(L"DS3 Coop Launcher, exe=%ls", ExePath().c_str());

    // Leftover from a self-update: the old exe may still be closing.
    const std::wstring old = ExePath() + L".old";
    for (int i = 0; i < 20 && FileExists(old) && !DeleteFileW(old.c_str()); ++i) Sleep(100);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_dpi = static_cast<int>(GetDpiForSystem());
    CreateFonts();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"DS3CoopLauncher";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, S(480), S(200)};
    AdjustWindowRect(&rc, style, FALSE);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DS3 Coop Mod - Launcher", style,
                                (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, g_opt.noLaunch ? SW_SHOWMINNOACTIVE : show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_log) std::fclose(g_log);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
