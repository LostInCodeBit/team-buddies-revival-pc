// files.h - the few file-system calls the saves, the ini and the disc search need, on Windows and POSIX.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifndef PATH_SEP
#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif
#endif

inline bool path_exists(const std::string& p) { // a file or a folder
#ifdef _WIN32
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

inline bool file_exists(const std::string& p) { // a file, not a folder
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

inline void make_dirs(const std::string& dir) { // every missing folder of the path
#ifdef _WIN32
    for (size_t i = 3; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '\\' || dir[i] == '/') CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
#else
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
#endif
}

inline bool file_delete(const std::string& p) { return std::remove(p.c_str()) == 0; }

enum { MOVE_DURABLE = 1, MOVE_ANY_VOLUME = 2 };
// renames `from` over `to` (replacing it). MOVE_DURABLE: on disk before it returns (temp file + rename saves);
// MOVE_ANY_VOLUME: copies when the two are on different drives
inline bool file_move(const std::string& from, const std::string& to, int flags = 0) {
#ifdef _WIN32
    DWORD f = MOVEFILE_REPLACE_EXISTING | (flags & MOVE_DURABLE ? MOVEFILE_WRITE_THROUGH : 0) | (flags & MOVE_ANY_VOLUME ? MOVEFILE_COPY_ALLOWED : 0);
    return MoveFileExA(from.c_str(), to.c_str(), f) != 0;
#else
    auto sync_path = [](const std::string& p, int mode) { int fd = open(p.c_str(), mode); if (fd >= 0) { fsync(fd); close(fd); } };
    if (flags & MOVE_DURABLE) sync_path(from, O_RDONLY);
    if (rename(from.c_str(), to.c_str()) != 0) {
        if (errno != EXDEV || !(flags & MOVE_ANY_VOLUME)) return false;
        FILE* in = fopen(from.c_str(), "rb");
        if (!in) return false;
        std::string tmp = to + ".copy";
        FILE* out = fopen(tmp.c_str(), "wb");
        bool ok = out != nullptr;
        char buf[65536];
        for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
        fclose(in);
        if (out && fclose(out) != 0) ok = false;
        if (!ok || rename(tmp.c_str(), to.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
        std::remove(from.c_str());
    }
    if (flags & MOVE_DURABLE) {
        size_t s = to.find_last_of('/');
        sync_path(s == std::string::npos ? "." : to.substr(0, s + 1), O_RDONLY | O_DIRECTORY);
    }
    return true;
#endif
}

// names of the files (not folders) in dir whose name ends in ext, any case (".bin", ".cue")
inline std::vector<std::string> list_files(const std::string& dir, const char* ext) {
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*" + ext).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (!d) return out;
    const size_t el = strlen(ext);
    while (dirent* e = readdir(d)) {
        size_t n = strlen(e->d_name);
        if (n <= el || strcasecmp(e->d_name + n - el, ext) != 0) continue;
        if (file_exists(dir + "/" + e->d_name)) out.push_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end()); // readdir has no order; Windows lists NTFS folders sorted
#endif
    return out;
}
