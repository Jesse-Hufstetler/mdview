/* setup - installer/uninstaller for mdview, runs on Windows 95 through Windows 11
 *
 *   setup.exe              interactive install
 *   setup.exe /S           silent install (associates .md files)
 *   uninstall.exe /uninstall [/S]     (installed copy of this exe; also run from Add/Remove Programs)
 *
 * The viewer is embedded as resource 2 (type PAYLOAD). Win9x installs machine-wide to
 * Program Files; NT installs per-user under %LOCALAPPDATA%\Programs, so no admin prompt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>

#define APP "mdview"
#include "version.h"
#define VERSION VER_STR
#define PROGID "mdview.markdown"
#define UNKEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\mdview"
#define CLASSES "Software\\Classes\\"

static int silent;
static HKEY root;
static char instdir[MAX_PATH], self[MAX_PATH];

/* ---- shell link COM interfaces (not declared by Open Watcom's headers) ---- */

static const GUID guid_ShellLink  = {0x00021401, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID guid_IShellLink = {0x000214EE, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID guid_IPersistFile = {0x0000010B, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

typedef struct {
    HRESULT (__stdcall *QueryInterface)(void *, const GUID *, void **);
    ULONG   (__stdcall *AddRef)(void *);
    ULONG   (__stdcall *Release)(void *);
    HRESULT (__stdcall *GetPath)(void *, char *, int, void *, DWORD);
    HRESULT (__stdcall *GetIDList)(void *, void **);
    HRESULT (__stdcall *SetIDList)(void *, void *);
    HRESULT (__stdcall *GetDescription)(void *, char *, int);
    HRESULT (__stdcall *SetDescription)(void *, const char *);
    HRESULT (__stdcall *GetWorkingDirectory)(void *, char *, int);
    HRESULT (__stdcall *SetWorkingDirectory)(void *, const char *);
    HRESULT (__stdcall *GetArguments)(void *, char *, int);
    HRESULT (__stdcall *SetArguments)(void *, const char *);
    HRESULT (__stdcall *GetHotkey)(void *, WORD *);
    HRESULT (__stdcall *SetHotkey)(void *, WORD);
    HRESULT (__stdcall *GetShowCmd)(void *, int *);
    HRESULT (__stdcall *SetShowCmd)(void *, int);
    HRESULT (__stdcall *GetIconLocation)(void *, char *, int, int *);
    HRESULT (__stdcall *SetIconLocation)(void *, const char *, int);
    HRESULT (__stdcall *SetRelativePath)(void *, const char *, DWORD);
    HRESULT (__stdcall *Resolve)(void *, HWND, DWORD);
    HRESULT (__stdcall *SetPath)(void *, const char *);
} ShellLinkVtbl;
typedef struct { ShellLinkVtbl *lpVtbl; } ShellLink;

typedef struct {
    HRESULT (__stdcall *QueryInterface)(void *, const GUID *, void **);
    ULONG   (__stdcall *AddRef)(void *);
    ULONG   (__stdcall *Release)(void *);
    HRESULT (__stdcall *GetClassID)(void *, GUID *);
    HRESULT (__stdcall *IsDirty)(void *);
    HRESULT (__stdcall *Load)(void *, const WCHAR *, DWORD);
    HRESULT (__stdcall *Save)(void *, const WCHAR *, BOOL);
    HRESULT (__stdcall *SaveCompleted)(void *, const WCHAR *);
    HRESULT (__stdcall *GetCurFile)(void *, WCHAR **);
} PersistFileVtbl;
typedef struct { PersistFileVtbl *lpVtbl; } PersistFile;

static int make_link(const char *lnk, const char *target, const char *args, const char *desc)
{
    ShellLink *sl;
    PersistFile *pf;
    WCHAR wide[MAX_PATH];
    int ok = 0;

    if (CoCreateInstance(&guid_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                         &guid_IShellLink, (void **)&sl) != S_OK)
        return 0;
    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetDescription(sl, desc);
    sl->lpVtbl->SetWorkingDirectory(sl, instdir);
    if (args) sl->lpVtbl->SetArguments(sl, args);
    if (sl->lpVtbl->QueryInterface(sl, &guid_IPersistFile, (void **)&pf) == S_OK) {
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, wide, MAX_PATH);
        ok = pf->lpVtbl->Save(pf, wide, TRUE) == S_OK;
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

static int programs_dir(char *out)
{
    LPITEMIDLIST pidl;
    int ok = 0;
    if (SHGetSpecialFolderLocation(NULL, CSIDL_PROGRAMS, &pidl) == S_OK) {
        ok = SHGetPathFromIDList(pidl, out);
        CoTaskMemFree(pidl);
    }
    return ok;
}

/* ---- small helpers ---- */

static void say(const char *text, UINT icon)
{
    if (!silent) MessageBox(NULL, text, APP " setup", MB_OK | icon);
}

static void mkdirs(const char *path)
{
    char p[MAX_PATH];
    char *s;
    strcpy(p, path);
    for (s = p + 3; *s; s++) {
        if (*s == '\\') {
            *s = 0;
            CreateDirectory(p, NULL);
            *s = '\\';
        }
    }
    CreateDirectory(p, NULL);
}

static void reg_str(const char *key, const char *name, const char *val)
{
    HKEY k;
    if (RegCreateKeyEx(root, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueEx(k, name, 0, REG_SZ, (const BYTE *)val, strlen(val) + 1);
        RegCloseKey(k);
    }
}

static void reg_dword(const char *key, const char *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyEx(root, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueEx(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

/* the DisplayVersion of an existing install under the current root; returns 1 if there is one */
static int installed_version(char *out, DWORD size)
{
    HKEY k;
    DWORD len = size, type = 0;
    int ok = 0;
    out[0] = 0;
    if (RegOpenKeyEx(root, UNKEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        ok = RegQueryValueEx(k, "DisplayVersion", NULL, &type, (BYTE *)out, &len) == ERROR_SUCCESS &&
             type == REG_SZ && out[0];
        RegCloseKey(k);
    }
    return ok;
}

/* compare dotted versions such as 2026.10.31.1552 (older ones like 1.2 count as 1.2.0.0) */
static int version_cmp(const char *a, const char *b)
{
    int x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0}, i;
    sscanf(a, "%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3]);
    sscanf(b, "%d.%d.%d.%d", &y[0], &y[1], &y[2], &y[3]);
    for (i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static int extract_payload(const char *dest)
{
    HRSRC r = FindResource(NULL, MAKEINTRESOURCE(2), "PAYLOAD");
    HGLOBAL g;
    DWORD n, wrote;
    HANDLE f;
    int ok;

    if (!r) return 0;
    g = LoadResource(NULL, r);
    n = SizeofResource(NULL, r);
    f = CreateFile(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(f, LockResource(g), n, &wrote, NULL) && wrote == n;
    CloseHandle(f);
    return ok;
}

static void set_assoc(const char *ext, const char *exe)
{
    char key[200], cmd[MAX_PATH + 8], icon[MAX_PATH + 8];

    sprintf(key, CLASSES "%s", ext);
    reg_str(key, NULL, PROGID);
    sprintf(key, CLASSES "%s\\OpenWithProgids", ext);
    reg_str(key, PROGID, "");

    sprintf(icon, "%s,0", exe);
    sprintf(cmd, "\"%s\" \"%%1\"", exe);
    reg_str(CLASSES PROGID, NULL, "Markdown Document");
    reg_str(CLASSES PROGID "\\DefaultIcon", NULL, icon);
    reg_str(CLASSES PROGID "\\shell\\open\\command", NULL, cmd);
}

static void clear_assoc(HKEY r, const char *ext)
{
    HKEY k;
    char key[200], val[100];
    DWORD len = sizeof(val), type;

    sprintf(key, CLASSES "%s", ext);
    if (RegOpenKeyEx(r, key, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
        if (RegQueryValueEx(k, NULL, NULL, &type, (BYTE *)val, &len) == ERROR_SUCCESS &&
            strcmp(val, PROGID) == 0)
            RegDeleteValue(k, "");
        RegCloseKey(k);
    }
    sprintf(key, CLASSES "%s\\OpenWithProgids", ext);
    if (RegOpenKeyEx(r, key, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
        RegDeleteValue(k, PROGID);
        RegCloseKey(k);
    }
}

/* ---- install ---- */

static void choose_mode(void)
{
    const char *la = getenv("LOCALAPPDATA");
    int is9x = (GetVersion() & 0x80000000UL) != 0;

    if (!is9x && la && *la) {
        root = HKEY_CURRENT_USER;
        sprintf(instdir, "%s\\Programs\\" APP, la);
    } else {
        char pf[MAX_PATH] = "C:\\Program Files";
        DWORD len = sizeof(pf);
        HKEY k;
        root = HKEY_LOCAL_MACHINE;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion",
                         0, KEY_READ, &k) == ERROR_SUCCESS) {
            RegQueryValueEx(k, "ProgramFilesDir", NULL, NULL, (BYTE *)pf, &len);
            RegCloseKey(k);
        }
        sprintf(instdir, "%s\\" APP, pf);
    }
}

static int do_install(void)
{
    char exe[MAX_PATH], unexe[MAX_PATH], prog[MAX_PATH], dir[MAX_PATH], lnk[MAX_PATH];
    char q[MAX_PATH + 16];
    int assoc = 1;
    char msg[MAX_PATH + 400];

    choose_mode();
    if (!silent) {
        char prev[64], note[160] = "";
        if (installed_version(prev, sizeof(prev))) {
            int cmp = version_cmp(prev, VERSION);
            if (cmp < 0)       sprintf(note, "\n\nInstalled version: %s (will be upgraded)", prev);
            else if (cmp == 0) sprintf(note, "\n\nInstalled version: %s (same version, will be reinstalled)", prev);
            else               sprintf(note, "\n\nInstalled version: %s (newer than this setup, will be replaced)", prev);
        }
        sprintf(msg, APP " " VERSION " will be installed to:\n\n%s%s\n\nContinue?", instdir, note);
        if (MessageBox(NULL, msg, APP " setup", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
        assoc = MessageBox(NULL, "Open Markdown files (.md) with mdview?", APP " setup",
                           MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    mkdirs(instdir);
    sprintf(exe, "%s\\mdview.exe", instdir);
    sprintf(unexe, "%s\\uninstall.exe", instdir);
    if (!extract_payload(exe)) {
        say("Couldn't write mdview.exe. If mdview is running, close it and try again.",
            MB_ICONERROR);
        return 1;
    }
    CopyFile(self, unexe, FALSE);

    CoInitialize(NULL);
    if (programs_dir(prog)) {
        sprintf(dir, "%s\\" APP, prog);
        CreateDirectory(dir, NULL);
        sprintf(lnk, "%s\\mdview.lnk", dir);
        make_link(lnk, exe, NULL, "Markdown viewer");
        sprintf(lnk, "%s\\Uninstall mdview.lnk", dir);
        make_link(lnk, unexe, "/uninstall", "Remove mdview");
    }
    CoUninitialize();

    reg_str(UNKEY, "DisplayName", APP);
    reg_str(UNKEY, "DisplayVersion", VERSION);
    reg_str(UNKEY, "InstallLocation", instdir);
    sprintf(q, "\"%s\" /uninstall", unexe);
    reg_str(UNKEY, "UninstallString", q);
    sprintf(q, "%s,0", exe);
    reg_str(UNKEY, "DisplayIcon", q);
    reg_dword(UNKEY, "NoModify", 1);
    reg_dword(UNKEY, "NoRepair", 1);

    if (assoc) {
        set_assoc(".md", exe);
        set_assoc(".markdown", exe);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    if (!silent) {
        if (MessageBox(NULL, "mdview is installed.\n\nRun it now?", APP " setup",
                       MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecute(NULL, "open", exe, NULL, instdir, SW_SHOWNORMAL);
    }
    return 0;
}

/* ---- uninstall (two phases, so the running exe isn't the one being deleted) ---- */

static void remove_all(const char *dir)
{
    char path[MAX_PATH], prog[MAX_PATH];
    HKEY roots[2];
    int i;

    sprintf(path, "%s\\mdview.exe", dir);
    DeleteFile(path);
    sprintf(path, "%s\\uninstall.exe", dir);
    DeleteFile(path);
    RemoveDirectory(dir);

    CoInitialize(NULL);
    if (programs_dir(prog)) {
        sprintf(path, "%s\\" APP "\\mdview.lnk", prog);
        DeleteFile(path);
        sprintf(path, "%s\\" APP "\\Uninstall mdview.lnk", prog);
        DeleteFile(path);
        sprintf(path, "%s\\" APP, prog);
        RemoveDirectory(path);
    }
    CoUninitialize();

    roots[0] = HKEY_CURRENT_USER;
    roots[1] = HKEY_LOCAL_MACHINE;
    for (i = 0; i < 2; i++) {
        clear_assoc(roots[i], ".md");
        clear_assoc(roots[i], ".markdown");
        RegDeleteKey(roots[i], CLASSES PROGID "\\shell\\open\\command");
        RegDeleteKey(roots[i], CLASSES PROGID "\\shell\\open");
        RegDeleteKey(roots[i], CLASSES PROGID "\\shell");
        RegDeleteKey(roots[i], CLASSES PROGID "\\DefaultIcon");
        RegDeleteKey(roots[i], CLASSES PROGID);
        RegDeleteKey(roots[i], UNKEY);
        RegDeleteKey(roots[i], "Software\\mdview");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static int do_uninstall1(void)
{
    char dir[MAX_PATH], tmp[MAX_PATH], copy[MAX_PATH], cl[MAX_PATH * 2 + 40];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    char *slash;

    if (!silent && MessageBox(NULL, "Remove mdview from this computer?", APP " setup",
                              MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;

    strcpy(dir, self);
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;

    GetTempPath(sizeof(tmp), tmp);
    sprintf(copy, "%smdview-remove.exe", tmp);
    if (!CopyFile(self, copy, FALSE)) {
        say("Couldn't prepare the uninstaller.", MB_ICONERROR);
        return 1;
    }
    sprintf(cl, "\"%s\" /uninstall2 \"%s\"%s", copy, dir, silent ? " /S" : "");
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcess(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        say("Couldn't start the uninstaller.", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static int do_uninstall2(const char *dir)
{
    char leftover[MAX_PATH];

    Sleep(1500);                    /* let the first-phase process exit */
    remove_all(dir);
    sprintf(leftover, "%s\\mdview.exe", dir);
    if (GetFileAttributes(leftover) != 0xFFFFFFFFUL) {
        char msg[MAX_PATH + 120];
        sprintf(msg, "mdview was removed, except for mdview.exe, which is still running.\n"
                     "Close it, then delete this folder:\n\n%s", dir);
        say(msg, MB_ICONWARNING);
    } else {
        say("mdview was removed.", MB_ICONINFORMATION);
    }

    /* this temp copy can't delete itself: have the OS do it at next boot */
    if (!(GetVersion() & 0x80000000UL)) {
        MoveFileEx(self, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    } else {
        char ini[MAX_PATH], shortself[MAX_PATH];
        GetWindowsDirectory(ini, sizeof(ini));
        strcat(ini, "\\wininit.ini");
        if (GetShortPathName(self, shortself, sizeof(shortself)))
            WritePrivateProfileString("Rename", "NUL", shortself, ini);
    }
    return 0;
}

/* ---- command line ---- */

static int next_token(const char **p, char *out, int max)
{
    int n = 0, quoted = 0;
    while (**p == ' ') (*p)++;
    if (!**p) return 0;
    while (**p && (quoted || **p != ' ')) {
        if (**p == '"') quoted = !quoted;
        else if (n < max - 1) out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    const char *p = cmd;
    char tok[MAX_PATH], dir[MAX_PATH] = "";
    int mode = 0;                   /* 0 install, 1 uninstall, 2 uninstall phase 2 */

    GetModuleFileName(NULL, self, sizeof(self));
    while (next_token(&p, tok, sizeof(tok))) {
        if (!stricmp(tok, "/S")) silent = 1;
        else if (!stricmp(tok, "/uninstall")) mode = 1;
        else if (!stricmp(tok, "/uninstall2")) {
            mode = 2;
            next_token(&p, dir, sizeof(dir));
        }
    }
    if (mode == 1) return do_uninstall1();
    if (mode == 2) return do_uninstall2(dir);
    return do_install();
}
