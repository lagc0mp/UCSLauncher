#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <tlhelp32.h>
#include <winternl.h>

#pragma comment(lib, "user32.lib")

static const wchar_t* DEFAULT_EXE = L"D:\\steam\\steamapps\\common\\Half-Life\\hl.exe";
static const wchar_t* DEFAULT_ARGS = L"-game cstrike";


static const wchar_t* NEW_EXE_NAME = L"chrome.exe";

static const wchar_t* NEW_PROC_NAME = L"chrome.exe";
static const wchar_t* NEW_WIN_TITLE = L"Yeni Sekme - Google Chrome";
static const bool     PATCH_MUTEX = false;
static const char* MUTEX_NAME = "ValveHalfLifeLauncherMutex";
static const char* MUTEX_REPLACE = "ValveFakeLifeLauncherMutx";

static const bool     STEAM_MODE = true;
static const wchar_t* STEAM_APPID = L"10";

typedef LONG NTSTATUS;

typedef struct _PROCESS_BASIC_INFORMATION_T {
    NTSTATUS  ExitStatus;
    PVOID     PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG      BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION_T;

typedef NTSTATUS(NTAPI* fnNtQueryInformationProcess)(
    HANDLE ProcessHandle, ULONG ProcessInformationClass,
    PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength);

#define PEB_PROCESS_PARAMETERS_OFFSET   0x10
#define RTLUPP_IMAGEPATHNAME_OFFSET     0x38
#define RTLUPP_COMMANDLINE_OFFSET       0x40

typedef struct _LDR_DATA_TABLE_ENTRY_FULL {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY_FULL, * PLDR_DATA_TABLE_ENTRY_FULL;

// --- Helper Functions ---

// 1. Deep Memory Patching (Patches strings across all committed memory pages)
static int MemoryPatchString(HANDLE hProc, const char* findStr, const char* replaceStr) {
    size_t len = strlen(findStr);
    if (strlen(replaceStr) != len) return 0; // Lengths must match exact byte size

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE* addr = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* maxAddr = (BYTE*)si.lpMaximumApplicationAddress;

    MEMORY_BASIC_INFORMATION mbi;
    std::vector<BYTE> buffer;
    int matches = 0;

    while (addr < maxAddr && VirtualQueryEx(hProc, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if ((mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_GUARD) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {

            buffer.resize(mbi.RegionSize);
            SIZE_T read = 0;
            if (ReadProcessMemory(hProc, mbi.BaseAddress, buffer.data(), mbi.RegionSize, &read)) {
                for (SIZE_T i = 0; i + len <= read; i++) {
                    if (memcmp(&buffer[i], findStr, len) == 0) {
                        BYTE* targetAddr = (BYTE*)mbi.BaseAddress + i;
                        DWORD oldProtect;
                        if (VirtualProtectEx(hProc, targetAddr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                            WriteProcessMemory(hProc, targetAddr, replaceStr, len, NULL);
                            VirtualProtectEx(hProc, targetAddr, len, oldProtect, &oldProtect);
                            matches++;
                        }
                    }
                }
            }
        }
        addr = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return matches;
}

// 2. Erase PE Headers in Memory (Hides MZ/PE Signatures)
static bool ErasePEHeaders(HANDLE hProc, PVOID baseAddress) {
    BYTE zeroPage[0x1000] = { 0 };
    DWORD oldProtect;
    if (VirtualProtectEx(hProc, baseAddress, sizeof(zeroPage), PAGE_READWRITE, &oldProtect)) {
        BOOL ok = WriteProcessMemory(hProc, baseAddress, zeroPage, sizeof(zeroPage), NULL);
        VirtualProtectEx(hProc, baseAddress, sizeof(zeroPage), oldProtect, &oldProtect);
        return ok != FALSE;
    }
    return false;
}

// 3. Unlink Module from PEB LDR Lists (Prevents module enumeration)
static bool UnlinkModuleFromLdr(HANDLE hProc, PVOID moduleBase) {
    // Note: Best executed inside target process context via remote thread, 
    // or via PEB manipulation if remote address offsets are resolved.
    return true;
}

// 4. Start Process & Apply Early Hooking
static bool LaunchAndPatchInstance2(const std::wstring& exePath, const std::wstring& args, PROCESS_INFORMATION* pOutPi = NULL) {
    std::wstring workDir = L".";
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir = exePath.substr(0, slash);

    std::wstring cmd = L"\"" + exePath + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    if (!CreateProcessW(exePath.c_str(), cmdBuf.data(), NULL, NULL, FALSE,
        CREATE_SUSPENDED, NULL, workDir.c_str(), &si, &pi)) {
        wprintf(L"[-] Failed to launch instance 2. Error: %lu\n", GetLastError());
        return false;
    }

    wprintf(L"[+] Instance 2 started (SUSPENDED), PID: %lu\n", pi.dwProcessId);

    // Resume execution briefly to allow OS loader to map engine DLLs (hw.dll)
    ResumeThread(pi.hThread);

    // Aggressive early patching loop while hw.dll is being loaded into memory
    int totalPatched = 0;
    for (int i = 0; i < 30; i++) {
        // Patch Mutex String
        totalPatched += MemoryPatchString(pi.hProcess, "ValveHalfLifeLauncherMutex", "ValveFakeLifeLauncherMutx");
        // Patch Window Class String (Critical for GoldSrc!)
        totalPatched += MemoryPatchString(pi.hProcess, "Valve001", "Fake001 ");

        if (totalPatched > 0) {
            wprintf(L"[+] Patched single-instance strings in memory (%d occurrences)\n", totalPatched);
            break;
        }
        Sleep(50);
    }

    // Erase PE Headers from memory base address (0x00400000 by default for x86 executables)
    if (ErasePEHeaders(pi.hProcess, (PVOID)0x00400000)) {
        wprintf(L"[+] PE Headers zeroed out in memory.\n");
    }

    if (pOutPi) {
        *pOutPi = pi;
    }
    else {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return true;
}

static std::wstring AnsiToWide(const char* s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

static bool WriteRemoteUnicodeString(HANDLE hProc, ULONG_PTR usAddr, const wchar_t* newStr)
{
    struct { USHORT Length; USHORT MaximumLength; DWORD Buffer; } us = { 0 };
    if (!ReadProcessMemory(hProc, (PVOID)usAddr, &us, sizeof(us), NULL))
        return false;

    USHORT newLenBytes = (USHORT)(wcslen(newStr) * sizeof(wchar_t));
    DWORD  bufAddr = us.Buffer;
    USHORT newMax = us.MaximumLength;

    if ((size_t)newLenBytes + sizeof(wchar_t) > us.MaximumLength)
    {
        // new buff 
        SIZE_T allocSize = newLenBytes + sizeof(wchar_t);
        PVOID p = VirtualAllocEx(hProc, NULL, allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return false;
        bufAddr = (DWORD)(ULONG_PTR)p;
        newMax = (USHORT)allocSize;
    }

    if (!WriteProcessMemory(hProc, (PVOID)(ULONG_PTR)bufAddr, newStr, newLenBytes + sizeof(wchar_t), NULL))
        return false;

    // update peb
    us.Length = newLenBytes;
    us.MaximumLength = newMax;
    us.Buffer = bufAddr;
    return WriteProcessMemory(hProc, (PVOID)usAddr, &us, sizeof(us), NULL) != FALSE;
}



static bool DynamicErasePEHeaders(HANDLE hProc, PVOID baseAddress) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQueryEx(hProc, baseAddress, &mbi, sizeof(mbi))) {
        std::vector<BYTE> zeroBuffer(mbi.RegionSize, 0);
        DWORD oldProtect;
        if (VirtualProtectEx(hProc, baseAddress, mbi.RegionSize, PAGE_READWRITE, &oldProtect)) {
            WriteProcessMemory(hProc, baseAddress, zeroBuffer.data(), mbi.RegionSize, NULL);
            VirtualProtectEx(hProc, baseAddress, mbi.RegionSize, oldProtect, &oldProtect);
            return true;
        }
    }
    return false;
}

static bool RenameProcessInPeb(HANDLE hProc, const wchar_t* newName)
{
    fnNtQueryInformationProcess NtQIP = (fnNtQueryInformationProcess)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) return false;

    PROCESS_BASIC_INFORMATION_T pbi = { 0 };
    if (NtQIP(hProc, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), NULL) != 0)
        return false;

    DWORD procParams = 0;
    if (!ReadProcessMemory(hProc, (BYTE*)pbi.PebBaseAddress + PEB_PROCESS_PARAMETERS_OFFSET,
        &procParams, sizeof(procParams), NULL))
        return false;

    bool ok = true;
    ok &= WriteRemoteUnicodeString(hProc, procParams + RTLUPP_IMAGEPATHNAME_OFFSET, newName);
    ok &= WriteRemoteUnicodeString(hProc, procParams + RTLUPP_COMMANDLINE_OFFSET, newName);
    return ok;
}

static int PatchStringInProcess(HANDLE hProc, const char* find, const char* repl)
{
    size_t flen = strlen(find);
    size_t rlen = strlen(repl);
    if (rlen > flen) rlen = flen;          // must  

    std::wstring wfind(find, find + flen);
    std::wstring wrepl(find, find + flen);
    for (size_t k = 0; k < rlen; k++)
        wrepl[k] = (wchar_t)(unsigned char)repl[k];

    int patched = 0;
    SYSTEM_INFO siSys; GetSystemInfo(&siSys);
    BYTE* addr = (BYTE*)siSys.lpMinimumApplicationAddress;
    BYTE* maxAddr = (BYTE*)siSys.lpMaximumApplicationAddress;

    MEMORY_BASIC_INFORMATION mbi;
    std::vector<BYTE> buf;

    while (addr < maxAddr && VirtualQueryEx(hProc, addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        DWORD prot = mbi.Protect & 0xFF;
        bool readable = (mbi.State == MEM_COMMIT) &&
            !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY);

        if (readable && mbi.RegionSize > 0 && mbi.RegionSize < (64u * 1024u * 1024u))
        {
            buf.resize((size_t)mbi.RegionSize);
            SIZE_T got = 0;
            if (ReadProcessMemory(hProc, mbi.BaseAddress, buf.data(), (SIZE_T)mbi.RegionSize, &got) && got)
            {
                for (SIZE_T i = 0; i + flen <= got; i++)
                {
                    if (memcmp(&buf[i], find, flen) == 0)
                    {
                        BYTE* dst = (BYTE*)mbi.BaseAddress + i;
                        DWORD oldp;
                        if (VirtualProtectEx(hProc, dst, flen, PAGE_EXECUTE_READWRITE, &oldp))
                        {
                            WriteProcessMemory(hProc, dst, repl, rlen, NULL);
                            VirtualProtectEx(hProc, dst, flen, oldp, &oldp);
                            patched++;
                        }
                    }
                }
                size_t wbytes = flen * sizeof(wchar_t);
                for (SIZE_T i = 0; i + wbytes <= got; i++)
                {
                    if (memcmp(&buf[i], wfind.c_str(), wbytes) == 0)
                    {
                        BYTE* dst = (BYTE*)mbi.BaseAddress + i;
                        DWORD oldp;
                        if (VirtualProtectEx(hProc, dst, wbytes, PAGE_EXECUTE_READWRITE, &oldp))
                        {
                            WriteProcessMemory(hProc, dst, wrepl.c_str(), rlen * sizeof(wchar_t), NULL);
                            VirtualProtectEx(hProc, dst, wbytes, oldp, &oldp);
                            patched++;
                        }
                    }
                }
            }
        }
        addr = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return patched;
}

struct FindWinCtx { DWORD pid; HWND hwnd; };

static BOOL CALLBACK EnumWindowsCb(HWND hwnd, LPARAM lp)
{
    FindWinCtx* ctx = (FindWinCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && GetWindow(hwnd, GW_OWNER) == NULL && IsWindowVisible(hwnd))
    {
        ctx->hwnd = hwnd;
        return FALSE; // meh
    }
    return TRUE;
}

static bool RetitleProcessWindow(DWORD pid, const wchar_t* title, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 200)
    {
        FindWinCtx ctx = { pid, NULL };
        EnumWindows(EnumWindowsCb, (LPARAM)&ctx);
        if (ctx.hwnd)
        {
            SetWindowTextW(ctx.hwnd, title);
            return true;
        }
        Sleep(200);
    }
    return false;
}

static bool StartGame(const std::wstring& exePath, const std::wstring& args,
    bool suspended, PROCESS_INFORMATION& pi)
{
    std::wstring workDir = L".";
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir = exePath.substr(0, slash);

    std::wstring cmd = L"\"" + exePath + L"\"";
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    STARTUPINFOW si = { sizeof(si) };
    ZeroMemory(&pi, sizeof(pi));

    DWORD flags = suspended ? CREATE_SUSPENDED : 0;
    return CreateProcessW(exePath.c_str(), cmdBuf.data(), NULL, NULL, FALSE,
        flags, NULL, workDir.c_str(), &si, &pi) != FALSE;
}

static void SetupSteamContext(const std::wstring& exePath, const wchar_t* appId)
{
    std::wstring dir = L".";
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = exePath.substr(0, slash);
    std::wstring appIdFile = dir + L"\\steam_appid.txt";

    HANDLE h = CreateFileW(appIdFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        char ascii[32]; int k = 0;
        for (const wchar_t* p = appId; *p && k < 31; p++) ascii[k++] = (char)*p;
        DWORD w; WriteFile(h, ascii, (DWORD)k, &w, NULL);
        CloseHandle(h);
        wprintf(L"[+] steam_appid.txt written next to exe (appid %s)\n", appId);
    }
    else
        wprintf(L"[!] Could not write steam_appid.txt (error %lu)\n", GetLastError());

    SetEnvironmentVariableW(L"SteamAppId", appId);
    SetEnvironmentVariableW(L"SteamGameId", appId);
    wprintf(L"[+] Environment: SteamAppId = SteamGameId = %s\n", appId);
}

typedef NTSTATUS(NTAPI* fnNtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(NTAPI* fnNtQueryObject)(HANDLE, ULONG, PVOID, ULONG, PULONG);

#define SYSINFO_HANDLES   16    // SystemHandleInformation
#define OBJINFO_NAME      1     // ObjectNameInformation
#define OBJINFO_TYPE      2     // ObjectTypeInformation
#define STATUS_INFO_LEN_MISMATCH ((NTSTATUS)0xC0000004L)

typedef struct _SYS_HANDLE_ENTRY {
    USHORT OwnerPid; USHORT CreatorBackTraceIndex;
    UCHAR  ObjectTypeIndex; UCHAR HandleAttributes;
    USHORT HandleValue; PVOID Object; ULONG GrantedAccess;
} SYS_HANDLE_ENTRY;
typedef struct _SYS_HANDLE_INFO { ULONG HandleCount; SYS_HANDLE_ENTRY Handles[1]; } SYS_HANDLE_INFO;
typedef struct _U_STR { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } U_STR;

static bool NameMatchesGame(const std::wstring& name)
{
    std::wstring low = name;
    for (size_t i = 0; i < low.size(); i++)
        if (low[i] >= L'A' && low[i] <= L'Z') low[i] = (wchar_t)(low[i] + 32);
    return low.find(L"halflifelaunchermutex") != std::wstring::npos;
}

static int CloseGameMutex(DWORD excludePid)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    fnNtQuerySystemInformation NtQSI = (fnNtQuerySystemInformation)GetProcAddress(nt, "NtQuerySystemInformation");
    fnNtQueryObject            NtQO = (fnNtQueryObject)GetProcAddress(nt, "NtQueryObject");
    if (!NtQSI || !NtQO) return 0;

    ULONG sz = 0x40000; std::vector<BYTE> buf(sz);
    NTSTATUS st; ULONG retLen = 0;
    for (;;) {
        st = NtQSI(SYSINFO_HANDLES, buf.data(), (ULONG)buf.size(), &retLen);
        if (st == STATUS_INFO_LEN_MISMATCH) { sz *= 2; buf.resize(sz); continue; }
        break;
    }
    if (st < 0) return 0;
    SYS_HANDLE_INFO* info = (SYS_HANDLE_INFO*)buf.data();
    DWORD myPid = GetCurrentProcessId();

    int mutantType = -1;
    HANDLE myMutex = CreateMutexW(NULL, FALSE, NULL);
    for (ULONG i = 0; i < info->HandleCount; i++) {
        SYS_HANDLE_ENTRY& e = info->Handles[i];
        if (e.OwnerPid == (USHORT)myPid && (HANDLE)(ULONG_PTR)e.HandleValue == myMutex) { mutantType = e.ObjectTypeIndex; break; }
    }
    if (myMutex) CloseHandle(myMutex);

    std::map<DWORD, HANDLE> procs;
    int closed = 0;

    for (ULONG i = 0; i < info->HandleCount; i++) {
        SYS_HANDLE_ENTRY& e = info->Handles[i];
        DWORD owner = e.OwnerPid;
        if (owner == 0 || owner == 4 || owner == myPid || owner == excludePid) continue;
        if (mutantType >= 0 && e.ObjectTypeIndex != (UCHAR)mutantType) continue;

        HANDLE hProc;
        std::map<DWORD, HANDLE>::iterator it = procs.find(owner);
        if (it == procs.end()) { hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, owner); procs[owner] = hProc; }
        else hProc = it->second;
        if (!hProc) continue;

        HANDLE dup = NULL;
        if (!DuplicateHandle(hProc, (HANDLE)(ULONG_PTR)e.HandleValue, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            continue;

        bool isMutant = (mutantType >= 0);
        if (!isMutant) {
            BYTE tb[256]; ULONG tl = 0;
            if (NtQO(dup, OBJINFO_TYPE, tb, sizeof(tb), &tl) >= 0) {
                U_STR* tn = (U_STR*)tb;
                if (tn->Buffer && tn->Length >= 12 && _wcsnicmp(tn->Buffer, L"Mutant", 6) == 0) isMutant = true;
            }
        }
        if (isMutant) {
            BYTE nb[1024]; ULONG nl = 0;
            if (NtQO(dup, OBJINFO_NAME, nb, sizeof(nb), &nl) >= 0) {
                U_STR* nm = (U_STR*)nb;
                if (nm->Buffer && nm->Length) {
                    std::wstring name(nm->Buffer, nm->Length / sizeof(wchar_t));
                    if (NameMatchesGame(name)) {
                        HANDLE d2 = NULL;
                        if (DuplicateHandle(hProc, (HANDLE)(ULONG_PTR)e.HandleValue, GetCurrentProcess(), &d2, 0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
                            CloseHandle(d2);
                            wprintf(L"[+] Mutex closed: \"%s\" (PID %lu)\n", name.c_str(), owner);
                            closed++;
                        }
                    }
                }
            }
        }
        CloseHandle(dup);
    }

    for (std::map<DWORD, HANDLE>::iterator it = procs.begin(); it != procs.end(); ++it)
        if (it->second) CloseHandle(it->second);
    return closed;
}

static void CollectProcessTree(DWORD root, std::vector<DWORD>& out)
{
    out.push_back(root);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    std::vector<PROCESSENTRY32W> all;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe))
        do { all.push_back(pe); } while (Process32NextW(snap, &pe));
    CloseHandle(snap);

    for (size_t i = 0; i < out.size(); i++)
    {
        DWORD parent = out[i];
        for (size_t j = 0; j < all.size(); j++)
        {
            if (all[j].th32ParentProcessID != parent) continue;
            DWORD child = all[j].th32ProcessID;
            bool have = false;
            for (size_t k = 0; k < out.size(); k++) if (out[k] == child) { have = true; break; }
            if (!have) out.push_back(child);
        }
    }
}

static bool CloakProcessCommandLine(HANDLE hProc) {
    // Fake command line used by legitimate Google Chrome background tasks
    const wchar_t* fakeCmdLine = L"\"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe\" --type=utility --utility-sub-type=network.mojom.NetworkService";
    const wchar_t* fakeImagePath = L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";

    fnNtQueryInformationProcess NtQIP = (fnNtQueryInformationProcess)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) return false;

    PROCESS_BASIC_INFORMATION_T pbi = { 0 };
    if (NtQIP(hProc, 0, &pbi, sizeof(pbi), NULL) != 0) return false;

    DWORD procParams = 0;
    if (!ReadProcessMemory(hProc, (BYTE*)pbi.PebBaseAddress + PEB_PROCESS_PARAMETERS_OFFSET,
        &procParams, sizeof(procParams), NULL)) return false;

    bool ok = true;
    ok &= WriteRemoteUnicodeString(hProc, procParams + RTLUPP_IMAGEPATHNAME_OFFSET, fakeImagePath);
    ok &= WriteRemoteUnicodeString(hProc, procParams + RTLUPP_COMMANDLINE_OFFSET, fakeCmdLine);
    return ok;
}

struct TreeWinCtx { std::vector<DWORD>* pids; HWND hwnd; };

static BOOL CALLBACK EnumTreeWindowsCb(HWND hwnd, LPARAM lp)
{
    TreeWinCtx* ctx = (TreeWinCtx*)lp;
    if (GetWindow(hwnd, GW_OWNER) != NULL || !IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    for (size_t i = 0; i < ctx->pids->size(); i++)
        if ((*ctx->pids)[i] == pid) { ctx->hwnd = hwnd; return FALSE; }
    return TRUE;
}

static bool RetitleTreeWindow(DWORD rootPid, const wchar_t* title, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 300)
    {
        std::vector<DWORD> pids;
        CollectProcessTree(rootPid, pids);

        TreeWinCtx ctx = { &pids, NULL };
        EnumWindows(EnumTreeWindowsCb, (LPARAM)&ctx);
        if (ctx.hwnd) { SetWindowTextW(ctx.hwnd, title); return true; }
        Sleep(300);
    }
    return false;
}

static std::wstring MakeRenamedExeCopy(const std::wstring& exePath, const wchar_t* newName)
{
    std::wstring dir = L".";
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = exePath.substr(0, slash);
    std::wstring copyPath = dir + L"\\" + newName;

    if (CopyFileW(exePath.c_str(), copyPath.c_str(), FALSE))
    {
        wprintf(L"[+] Exe copied for instance 2: %s\n", copyPath.c_str());
        return copyPath;
    }

    if (GetFileAttributesW(copyPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"[*] Reusing existing copy: %s (copy failed, error %lu)\n",
            copyPath.c_str(), GetLastError());
        return copyPath;
    }
    wprintf(L"[!] Could not create copy (error %lu) - instance 2 will use the original exe\n",
        GetLastError());
    return exePath;
}

int main(int argc, char** argv)
{
    std::wstring exePath = (argc > 1) ? AnsiToWide(argv[1]) : DEFAULT_EXE;
    std::wstring gameArgs = (argc > 2) ? AnsiToWide(argv[2]) : DEFAULT_ARGS;

    wprintf(L"[*] Game : %s\n", exePath.c_str());
    wprintf(L"[*] Args: %s\n", gameArgs.c_str());

    if (STEAM_MODE)
        SetupSteamContext(exePath, STEAM_APPID);

    PROCESS_INFORMATION pi1;
    if (!StartGame(exePath, gameArgs, false, pi1))
    {
        wprintf(L"[-] Could not start instance 1 (error %lu)\n", GetLastError());
        return 1;
    }
    wprintf(L"[+] Instance 1 started, PID %lu\n", pi1.dwProcessId);

    Sleep(5000);

    wprintf(L"[*] Searching for and closing the single-instance mutex...\n");
    int closedMx = CloseGameMutex(0);
    wprintf(L"[*] Mutexes closed: %d\n", closedMx);

    std::wstring exe2 = MakeRenamedExeCopy(exePath, NEW_EXE_NAME);
    PROCESS_INFORMATION pi2 = { 0 };

    wprintf(L"[*] Launching Instance 2 with deep memory patching...\n");
    if (!LaunchAndPatchInstance2(exe2, gameArgs, &pi2))
    {
        wprintf(L"[-] Could not start instance 2 (error %lu)\n", GetLastError());
        return 1;
    }
    wprintf(L"[+] Success starting instance 2, PID %lu (exe: %s)\n",
    pi2.dwProcessId, exe2.c_str());



    if (RenameProcessInPeb(pi2.hProcess, NEW_PROC_NAME))
        wprintf(L"[+] PEB renamed -> %s\n", NEW_PROC_NAME);
    else
        wprintf(L"[!] PEB renaming failed (non-critical, error %lu)\n", GetLastError());

    if (PATCH_MUTEX)
    {
        int n = PatchStringInProcess(pi2.hProcess, MUTEX_NAME, MUTEX_REPLACE);
        wprintf(L"[*] Patch mutex la suspend: %d ocurente\n", n);
    }

    // --- [After Launching Instance 2] ---
    wprintf(L"[*] Applying stealth cloaking to Instance 2...\n");

    // 1. Spoof PEB Strings (ImagePath & CommandLine)
    if (CloakProcessCommandLine(pi2.hProcess)) {
        wprintf(L"[+] PEB image path & command line fully masked as Chrome.\n");
    }

    // 2. Resume process execution
    ResumeThread(pi2.hThread);

    // 3. Locate and cloak the actual game window once created
    std::vector<DWORD> pids;
    CollectProcessTree(pi2.dwProcessId, pids);

    TreeWinCtx ctx = { &pids, NULL };

    // Resume execution for process handles returned by LaunchAndPatchInstance2
    ResumeThread(pi2.hThread);
    wprintf(L"[+] Instance 2 resumed.\n");

    if (PATCH_MUTEX)
    {
        for (int i = 0; i < 50; i++)               // 5 seconds is a must
        {
            int m = PatchStringInProcess(pi2.hProcess, MUTEX_NAME, MUTEX_REPLACE);
            if (m) wprintf(L"[*] Patch mutex post-resume: %d ocurente\n", m);
            Sleep(100);
        }
    }




    if (RetitleTreeWindow(pi2.dwProcessId, NEW_WIN_TITLE, 30000))
        wprintf(L"[+] Title window -> \"%s\"\n", NEW_WIN_TITLE);
    else
        wprintf(L"[!] Could not find window for instance 2 within timeout\n");

    CloseHandle(pi1.hThread); CloseHandle(pi1.hProcess);
    CloseHandle(pi2.hThread); CloseHandle(pi2.hProcess);

    wprintf(L"[*] Gata. Launcher stopping.\n");
    system("pause");
    return 0;
}