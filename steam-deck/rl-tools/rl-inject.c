/* rl-inject.exe - loads a DLL into a running process, the same way BakkesMod.exe does it
 * (LoadLibraryW on a remote thread), but without BakkesMod.exe's update-server check. BakkesMod.exe will not
 * inject when it cannot reach its update server, so bakkes-helper.sh uses this when there is no internet.
 *
 * The mod itself also asks a server whether it matches the game; without internet it shows a
 * "Could not verify RL version, do you wish to inject anyway?" box inside the game, which cannot be reached in
 * Gaming Mode. bakkes-helper.sh has already done that check against the saved server answer before running
 * this, so the box is answered Yes here.
 * Last resort only: in Gaming Mode (gamescope) the game's picture is gone after the box, answered or not (black
 * screen, game running underneath). psynet-offline.py therefore answers the mod's version request locally so
 * that the box does not come up at all; bakkes-update.py check refuses the injection when it could not.
 *
 * Usage:  rl-inject.exe <process.exe> <full path of dll>
 * Exit:   0 injected, 2 already injected, 3 process not running, 4 cannot open process, 5 injection failed
 *
 * Build (any machine with zig):
 *   zig cc -target x86_64-windows-gnu -O2 -municode -o rl-inject.exe rl-inject.c
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

struct InjectionParameters { int version; int injectionMode; };   /* same block BakkesMod.exe publishes */

static const wchar_t *base_name(const wchar_t *path)
{
    const wchar_t *b = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static DWORD find_process(const wchar_t *name)
{
    PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static int has_module(DWORD pid, const wchar_t *module)
{
    MODULEENTRY32W me = { .dwSize = sizeof(me) };
    int found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, module) == 0) { found = 1; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

struct prompt { DWORD pid; HWND dialog; };

static BOOL CALLBACK find_verify_text(HWND child, LPARAM found)
{
    wchar_t text[512];
    if (GetWindowTextW(child, text, 512) && wcsstr(text, L"Could not verify RL version")) *(int *)found = 1;
    return TRUE;
}

static BOOL CALLBACK find_prompt(HWND win, LPARAM arg)
{
    struct prompt *p = (struct prompt *)arg;
    DWORD pid = 0;
    wchar_t cls[32];
    int found = 0;
    GetWindowThreadProcessId(win, &pid);
    if (pid != p->pid || !GetClassNameW(win, cls, 32) || wcscmp(cls, L"#32770") != 0) return TRUE;
    EnumChildWindows(win, find_verify_text, (LPARAM)&found);
    if (!found) return TRUE;
    p->dialog = win;
    return FALSE;
}

/* Answers the mod's "Could not verify RL version ... inject anyway?" box with Yes; 1 if it was there. */
static int answer_version_prompt(DWORD pid, int seconds)
{
    for (int i = 0; i < seconds * 4; i++) {
        struct prompt p = { pid, NULL };
        EnumWindows(find_prompt, (LPARAM)&p);
        if (p.dialog) {
            HWND yes = GetDlgItem(p.dialog, IDYES);
            PostMessageW(p.dialog, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), (LPARAM)yes);
            for (int j = 0; j < 20 && IsWindow(p.dialog); j++) Sleep(100);
            printf(IsWindow(p.dialog) ? "version prompt did not close\n" : "answered the mod's version prompt with Yes\n");
            fflush(stdout);
            return 1;
        }
        Sleep(250);
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) {
        printf("usage: rl-inject.exe <process.exe> <full path of dll>\n");
        return 1;
    }
    const wchar_t *dll = argv[2], *dll_name = base_name(argv[2]);
    DWORD pid = find_process(argv[1]);
    if (!pid) { printf("%ls is not running\n", argv[1]); return 3; }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { printf("dll not found: %ls\n", dll); return 5; }
    if (has_module(pid, dll_name)) {
        printf("%ls is already loaded in pid %lu\n", dll_name, pid);
        fflush(stdout);
        answer_version_prompt(pid, 3);
        return 2;
    }

    /* BakkesMod.exe publishes this before injecting; the mod is said to ignore it now, kept for parity. */
    struct InjectionParameters ip = { 32, 0 };
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(ip), L"BakkesModInjectionParameters");
    void *view = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ip)) : NULL;
    if (view) memcpy(view, &ip, sizeof(ip));

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { printf("OpenProcess(%lu) failed, error %lu\n", pid, GetLastError()); return 4; }
    SIZE_T len = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(proc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dll, len, NULL)) {
        printf("could not write into pid %lu, error %lu\n", pid, GetLastError());
        return 5;
    }
    LPTHREAD_START_ROUTINE load = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE thread = CreateRemoteThread(proc, NULL, 0, load, remote, 0, NULL);
    if (!thread) { printf("CreateRemoteThread failed, error %lu\n", GetLastError()); return 5; }
    WaitForSingleObject(thread, 60000);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (!has_module(pid, dll_name)) { printf("%ls did not load in pid %lu\n", dll_name, pid); return 5; }
    printf("injected %ls into pid %lu\n", dll_name, pid);
    fflush(stdout);
    /* Online the mod verifies the version itself and no prompt appears. Waiting here also keeps the
       parameter block alive while the mod starts up. */
    if (answer_version_prompt(pid, 30)) Sleep(8000);
    else printf("no version prompt from the mod\n");
    return 0;
}
