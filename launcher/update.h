// update.h - the launcher's updates from GitHub releases (update.cpp): the check, and the signed download and
// install. Both block: the launcher runs them on a worker thread.
#pragma once
#include <atomic>
#include <string>

// the latest release's tag ("v0.3.0"; "" if nothing is released yet); false with the reason in err if GitHub
// could not be asked
bool update_latest(std::wstring& tag, std::wstring& err);
// version a newer than b? ("v0.12.3" vs "0.12.2"; numbers only, a suffix such as "-rc1" is ignored)
bool update_newer(const std::wstring& a, const std::wstring& b);

// what an install is doing (UpdatePhase), and the download's bytes
struct UpdateProgress {
    std::atomic<int> phase{0};
    std::atomic<long long> done{0}, total{0};
};
enum UpdatePhase { UPH_DOWNLOAD, UPH_CHECK, UPH_REPLACE };
// downloads, checks and installs release `tag` over the files in dir (ending in a backslash): true when the new
// files are in place (start the new launcher); false with the reason in err, and every file as it was
bool update_install(const std::wstring& dir, const std::wstring& tag, UpdateProgress& progress, std::wstring& err);
// at start: deletes what an install left behind (the .old files, the update folder); false while some of it
// is still in use (the old launcher still closing)
bool update_cleanup(const std::wstring& dir);
// the release page, for installing by hand
std::wstring update_release_page();

// test harness (TB_UPDATE_LOG=<file>): one line per step
void update_log(const wchar_t* fmt, ...);
