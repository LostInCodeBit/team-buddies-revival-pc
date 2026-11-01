// update.cpp - the launcher's updates from GitHub releases (Windows).
// The check asks api.github.com for the latest release (drafts and pre-releases are never "latest"). An install
// downloads the release's SHA256SUMS.txt, its signature SHA256SUMS.txt.sig and TeamBuddies-<version>-win.zip,
// and puts the new files in place only when the signature is good for the public key compiled in
// (release_key.inc; tools/release_key.ps1 and tools/sign_release.ps1), the zip's SHA-256 is the signed one, the
// zip holds only the files a release has (tools/package_release.ps1), and its CHANGELOG.md is the version the
// tag names (an old signed release under a newer tag is refused). Files are swapped by renaming inside the game
// folder (a running exe or a loaded DLL can be renamed, though not overwritten); a step that fails puts every
// file back. The .old files and the update folder go at the next start. Settings, saves and screenshots are
// never touched. Every network step has timeouts and a size limit, and goes to GitHub over HTTPS only.
#include "update.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

#include "release_key.inc"

namespace {
const wchar_t* kRepo = L"LostInCodeBit/team-buddies-revival-pc"; // GitHub: where releases are published

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string ascii(const std::wstring& w) { return std::string(w.begin(), w.end()); } // only for checked names

// "v0.12.3" -> {0, 12, 3}; whatever follows the numbers ("-rc1") is ignored
std::vector<int> version_parts(const std::wstring& v) {
    std::vector<int> p;
    size_t i = !v.empty() && (v[0] == L'v' || v[0] == L'V') ? 1 : 0;
    while (i < v.size() && iswdigit(v[i]) && p.size() < 8) {
        int n = 0;
        while (i < v.size() && iswdigit(v[i])) n = std::min(n * 10 + (v[i++] - L'0'), 1000000);
        p.push_back(n);
        if (i < v.size() && v[i] == L'.') i++;
        else break;
    }
    return p;
}
// a tag we are willing to show and put in a URL: v1.2.3 style, short, plain characters
bool valid_tag(const std::wstring& t) {
    if (t.empty() || t.size() > 40 || version_parts(t).empty()) return false;
    for (wchar_t c : t)
        if (!(iswalnum(c) || c == L'.' || c == L'-' || c == L'+' || c == L'_')) return false;
    return true;
}
std::wstring without_v(const std::wstring& t) { return !t.empty() && (t[0] == L'v' || t[0] == L'V') ? t.substr(1) : t; }

// the string value of a key of the outermost JSON object ("" if missing); \" and \\ are unescaped, nothing else
std::string json_field(const std::string& j, const char* key) {
    int depth = 0;
    bool value = false; // after "key": at depth 1
    std::string s, last_key;
    for (size_t i = 0; i < j.size(); i++) {
        const char c = j[i];
        if (c == '"') {
            s.clear();
            for (i++; i < j.size() && j[i] != '"'; i++) {
                if (j[i] == '\\' && i + 1 < j.size()) i++;
                s += j[i];
            }
            if (depth != 1) continue;
            if (!value) last_key = s;
            else if (last_key == key) return s;
            else value = false;
        } else if (c == ':' && depth == 1) value = true;
        else if (c == ',' && depth == 1) value = false;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
    }
    return "";
}

// ---------------------------------------------------------------- HTTP
struct Host { std::wstring name; INTERNET_PORT port; bool secure; };

// test harness: TB_UPDATE_SERVER=http://127.0.0.1:<port> answers for both GitHub hosts (plain HTTP, this machine
// only); TB_UPDATE_TEST_KEY=<file> is then the public key signatures are checked with
bool test_server(Host& h) {
    const wchar_t* s = _wgetenv(L"TB_UPDATE_SERVER");
    const std::wstring pre = L"http://127.0.0.1:";
    if (!s || std::wstring(s).rfind(pre, 0) != 0) return false;
    int port = _wtoi(s + pre.size());
    if (port <= 0 || port > 65535) return false;
    h = {L"127.0.0.1", (INTERNET_PORT)port, false};
    return true;
}
Host api_host() { Host h; return test_server(h) ? h : Host{L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, true}; }
Host web_host() { Host h; return test_server(h) ? h : Host{L"github.com", INTERNET_DEFAULT_HTTPS_PORT, true}; }

// a GET (redirects followed, never from HTTPS to HTTP). The body of a 200 goes to mem (at most cap bytes) or, with
// a file, to the file and the hash as it arrives. false if the request failed or broke off; status = the HTTP
// status (0 if none)
bool http_get(const Host& h, const std::wstring& path, bool api, long long cap, DWORD& status, std::string* mem,
              HANDLE file = INVALID_HANDLE_VALUE, BCRYPT_HASH_HANDLE hash = nullptr, UpdateProgress* progress = nullptr) {
    status = 0;
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"TeamBuddiesLauncher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, h.name.c_str(), h.port, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, h.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (r) {
        WinHttpSetTimeouts(r, 10000, 10000, 10000, 30000); // resolve, connect, send, each read (ms)
        static const wchar_t api_headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        BOOL sent = api ? WinHttpSendRequest(r, api_headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                        : WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (sent && WinHttpReceiveResponse(r, nullptr)) {
            DWORD len = sizeof status;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            ok = true;
            if (status == 200) {
                DWORD size = 0;
                len = sizeof size;
                if (progress && WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &size, &len, WINHTTP_NO_HEADER_INDEX))
                    progress->total = size;
                std::vector<char> buf(65536);
                long long all = 0;
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got)) { ok = false; break; } // a timeout or a dropped connection
                    if (!got) break;
                    all += got;
                    if (all > cap) { ok = false; break; }
                    if (mem) mem->append(buf.data(), got);
                    else {
                        DWORD wrote = 0;
                        if (!WriteFile(file, buf.data(), got, &wrote, nullptr) || wrote != got) { ok = false; break; }
                        if (hash && BCryptHashData(hash, (PUCHAR)buf.data(), got, 0) != 0) { ok = false; break; }
                    }
                    if (progress) progress->done = all;
                }
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

// ---------------------------------------------------------------- signature and hashes
std::string hex(const unsigned char* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

// sig_b64: base64 of the 64-byte ECDSA P-256 signature (r, s) of data's SHA-256
bool signature_ok(const std::string& data, const std::string& sig_b64) {
    std::vector<unsigned char> key(k_release_key, k_release_key + sizeof k_release_key);
    Host h;
    if (test_server(h)) { // the harness's own key, only with its local server
        key.clear();
        if (const wchar_t* f = _wgetenv(L"TB_UPDATE_TEST_KEY")) {
            FILE* fp = _wfopen(f, L"rb");
            unsigned char b[72];
            if (fp && fread(b, 1, 72, fp) == 72) key.assign(b, b + 72);
            if (fp) fclose(fp);
        }
        if (key.empty()) return false;
    }
    std::string t;
    for (char c : sig_b64) if (!isspace((unsigned char)c)) t += c;
    BYTE sig[96];
    DWORD n = sizeof sig;
    if (t.empty() || t.size() > 128 || !CryptStringToBinaryA(t.c_str(), (DWORD)t.size(), CRYPT_STRING_BASE64, sig, &n, nullptr, nullptr) || n != 64)
        return false;
    unsigned char digest[32];
    BCRYPT_ALG_HANDLE sha = nullptr, ecdsa = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptHash(sha, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), digest, 32) == 0 &&
              BCryptOpenAlgorithmProvider(&ecdsa, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptImportKeyPair(ecdsa, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k, key.data(), (ULONG)key.size(), 0) == 0 &&
              BCryptVerifySignature(k, nullptr, digest, 32, sig, 64, 0) == 0;
    if (k) BCryptDestroyKey(k);
    if (ecdsa) BCryptCloseAlgorithmProvider(ecdsa, 0);
    if (sha) BCryptCloseAlgorithmProvider(sha, 0);
    return ok;
}

// the SHA-256 (lowercase hex) SHA256SUMS.txt lists for name ("hash  name" lines), "" if none
std::string listed_hash(const std::string& sums, const std::string& name) {
    size_t p = 0;
    while (p < sums.size()) {
        size_t e = sums.find('\n', p);
        std::string ln = sums.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? sums.size() : e + 1;
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.size() != 66 + name.size() || ln.compare(64, 2, "  ") != 0 || ln.compare(66, std::string::npos, name) != 0) continue;
        std::string h = ln.substr(0, 64);
        for (char& c : h) {
            c = (char)tolower((unsigned char)c);
            if (!isxdigit((unsigned char)c)) return "";
        }
        return h;
    }
    return "";
}

// ---------------------------------------------------------------- files
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// deletes path and everything under it; a link (reparse point) is removed itself, never followed
bool remove_tree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path.c_str()) != 0;
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) remove_tree(path + L"\\" + fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

// a file a release may hold (tools/package_release.ps1): the programs, the docs, licenses\<name>
bool allowed(const std::wstring& rel) {
    static const wchar_t* top[] = {L"TeamBuddies.exe", L"TeamBuddiesLauncher.exe", L"SDL3.dll", L"CHANGELOG.md", L"LICENSE", L"PLAYING.md", L"README.txt"};
    for (const wchar_t* t : top)
        if (_wcsicmp(rel.c_str(), t) == 0) return true;
    if (rel.size() <= 9 || _wcsnicmp(rel.c_str(), L"licenses\\", 9) != 0 || rel.size() > 9 + 64) return false;
    for (size_t i = 9; i < rel.size(); i++)
        if (!(iswalnum(rel[i]) || rel[i] == L'.' || rel[i] == L'-' || rel[i] == L'_')) return false;
    return true;
}

// the files under root + rel ("licenses\SDL3.txt"); false if there is anything but plain files and folders
bool list_files(const std::wstring& root, const std::wstring& rel, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return true; // empty
    bool ok = true;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ok = false;
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = rel.size() < 64 && list_files(root, rel + n + L"\\", out);
        else out.push_back(rel + n);
        if (out.size() > 256) ok = false;
    } while (ok && FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}

// the newest released version in a CHANGELOG.md ("## 0.3.0 - 2026-11-30"), "" if none
std::wstring changelog_version(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return L"";
    char line[512];
    std::wstring v;
    while (v.empty() && fgets(line, sizeof line, f))
        if (!strncmp(line, "## ", 3) && isdigit((unsigned char)line[3])) {
            size_t n = 3;
            while (line[n] && !isspace((unsigned char)line[n])) n++;
            v = widen_utf8(std::string(line + 3, n - 3));
        }
    fclose(f);
    return v;
}

// unpacks zip into dest with Windows' own tar.exe (Windows 10 1803 and later; it refuses absolute paths and "..")
bool unzip(const std::wstring& zip, const std::wstring& dest, std::wstring& err) {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys, n) + L"\\tar.exe";
    if (!n || !exists(tar)) { err = L"Windows has no tar.exe to unpack it"; return false; }
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dest + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(tar.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dest.c_str(), &si, &pi)) {
        err = L"couldn't start tar.exe";
        return false;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) err = L"couldn't unpack the package";
    return code == 0;
}

// moves the staged files over the installed ones (the old ones to <name>.old); on any failure every file is put
// back as it was
bool swap_in(const std::wstring& dir, const std::wstring& staging, const std::vector<std::wstring>& files, std::wstring& err) {
    struct Done { std::wstring target, staged; bool had; };
    std::vector<Done> done;
    auto undo = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            MoveFileExW(it->target.c_str(), it->staged.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (it->had) MoveFileExW((it->target + L".old").c_str(), it->target.c_str(), 0);
        }
    };
    for (const std::wstring& rel : files) {
        const std::wstring target = dir + rel, staged = staging + rel, old = target + L".old";
        size_t sub = rel.find(L'\\');
        if (sub != std::wstring::npos) CreateDirectoryW((dir + rel.substr(0, sub)).c_str(), nullptr);
        const bool had = exists(target);
        if (had && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            undo();
            err = L"couldn't replace " + rel + L" (is it open?)";
            return false;
        }
        if (!MoveFileExW(staged.c_str(), target.c_str(), 0)) {
            if (had) MoveFileExW(old.c_str(), target.c_str(), 0);
            undo();
            err = L"couldn't replace " + rel;
            return false;
        }
        done.push_back({target, staged, had});
    }
    return true;
}
} // namespace

// ---------------------------------------------------------------- the interface
bool update_newer(const std::wstring& a, const std::wstring& b) {
    std::vector<int> x = version_parts(a), y = version_parts(b);
    size_t n = std::max(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    return x > y;
}

std::wstring update_release_page() { return std::wstring(L"https://github.com/") + kRepo + L"/releases/latest"; }

bool update_latest(std::wstring& tag, std::wstring& err) {
    tag.clear();
    std::string body;
    DWORD status = 0;
    if (!http_get(api_host(), std::wstring(L"/repos/") + kRepo + L"/releases/latest", true, 1 << 20, status, &body)) {
        err = L"couldn't reach GitHub";
        return false;
    }
    if (status == 404) return true; // nothing released yet
    if (status != 200) {
        err = status == 403 || status == 429 ? L"GitHub is limiting requests, try again later" : L"GitHub answered " + std::to_wstring(status);
        return false;
    }
    std::wstring t = widen_utf8(json_field(body, "tag_name"));
    if (!valid_tag(t)) {
        err = L"GitHub's answer named no version";
        return false;
    }
    tag = t;
    return true;
}

bool update_install(const std::wstring& dir, const std::wstring& tag, UpdateProgress& progress, std::wstring& err) {
    progress.phase = UPH_DOWNLOAD;
    progress.done = progress.total = 0;
    if (!valid_tag(tag)) { err = L"the release's name is not a version"; return false; }
    const std::wstring ver = without_v(tag), work = dir + L"update\\", staging = work + L"files\\";
    const std::string zip_name = "TeamBuddies-" + ascii(ver) + "-win.zip"; // valid_tag: plain ASCII
    const std::wstring zip = work + widen_utf8(zip_name);
    remove_tree(dir + L"update");
    if (!CreateDirectoryW(work.c_str(), nullptr)) { err = L"can't write to the game folder"; return false; }

    // the signed list of hashes first: nothing big is fetched for a release that isn't ours
    const Host web = web_host();
    const std::wstring base = std::wstring(L"/") + kRepo + L"/releases/download/" + tag + L"/";
    std::string sums, sig;
    DWORD status = 0;
    if (!http_get(web, base + L"SHA256SUMS.txt", false, 64 << 10, status, &sums) || status != 200) {
        err = status == 404 ? L"the release has no SHA256SUMS.txt" : L"couldn't download the release";
        return false;
    }
    if (!http_get(web, base + L"SHA256SUMS.txt.sig", false, 4 << 10, status, &sig) || status != 200) {
        err = status == 404 ? L"the release is not signed" : L"couldn't download the release";
        return false;
    }
    if (!signature_ok(sums, sig)) { err = L"the release's signature is not valid"; return false; }
    const std::string want = listed_hash(sums, zip_name);
    if (want.empty()) { err = L"the release has no Windows package"; return false; }
    update_log(L"signature ok, %S %S", zip_name.c_str(), want.c_str());

    // the package, hashed as it arrives
    HANDLE f = CreateFileW((zip + L".part").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"can't write to the game folder"; return false; }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool fetched = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                   BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                   http_get(web, base + widen_utf8(zip_name), false, 512ll << 20, status, nullptr, f, hash, &progress) && status == 200;
    unsigned char digest[32] = {};
    const bool hashed = hash && BCryptFinishHash(hash, digest, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!fetched) { err = L"couldn't download the release"; return false; }
    if (!hashed || hex(digest, 32) != want) { err = L"the download is damaged (not the signed file)"; return false; }
    if (!MoveFileExW((zip + L".part").c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING)) { err = L"can't write to the game folder"; return false; }

    // unpack it, and look at every file before anything is replaced
    progress.phase = UPH_CHECK;
    if (!CreateDirectoryW(staging.c_str(), nullptr)) { err = L"can't write to the game folder"; return false; }
    if (!unzip(zip, work + L"files", err)) return false;
    std::vector<std::wstring> files;
    if (!list_files(staging, L"", files)) { err = L"the package holds something other than files"; return false; }
    for (const std::wstring& rel : files)
        if (!allowed(rel)) { err = L"the package holds an unexpected file: " + rel; return false; }
    for (const wchar_t* need : {L"TeamBuddies.exe", L"TeamBuddiesLauncher.exe", L"CHANGELOG.md"})
        if (std::none_of(files.begin(), files.end(), [&](const std::wstring& r) { return _wcsicmp(r.c_str(), need) == 0; })) {
            err = std::wstring(L"the package has no ") + need;
            return false;
        }
    const std::wstring packaged = changelog_version(staging + L"CHANGELOG.md");
    if (packaged != ver) { err = L"the package is version " + (packaged.empty() ? L"?" : packaged) + L", not " + ver; return false; }

    progress.phase = UPH_REPLACE;
    if (!swap_in(dir, staging, files, err)) return false;
    update_log(L"installed %s (%d files)", ver.c_str(), (int)files.size());
    return true;
}

bool update_cleanup(const std::wstring& dir) {
    bool clean = true;
    for (const wchar_t* sub : {L"", L"licenses\\"}) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + sub + L"*.old").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            // only <a release file>.old (the pattern also matches some longer names through their 8.3 alias)
            std::wstring n = fd.cFileName;
            if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || n.size() <= 4 ||
                _wcsicmp(n.c_str() + n.size() - 4, L".old") != 0 || !allowed(sub + n.substr(0, n.size() - 4)))
                continue;
            if (!DeleteFileW((dir + sub + n).c_str())) clean = false;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (!remove_tree(dir + L"update")) clean = false;
    return clean;
}

void update_log(const wchar_t* fmt, ...) {
    const wchar_t* path = _wgetenv(L"TB_UPDATE_LOG");
    if (!path) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (FILE* f = _wfopen(path, L"a, ccs=UTF-8")) {
        fwprintf(f, L"[%lu] %s\n", GetCurrentProcessId(), buf);
        fclose(f);
    }
}
