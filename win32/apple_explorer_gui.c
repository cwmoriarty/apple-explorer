#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/apple_explorer.h"

/* Control & Command IDs */
#define ID_BTN_OPEN_IMAGE   101
#define ID_BTN_OPEN_BUNDLE  102
#define ID_BTN_OPEN_DISK    103
#define ID_CMB_VOL_SNAP     104
#define ID_BTN_EXTRACT_SEL  105
#define ID_BTN_EXTRACT_ALL  106
#define ID_TREE_DIRS        107
#define ID_LIST_FILES       108
#define ID_STATUS_BAR       109

typedef struct {
    char     full_path[1024];
    uint64_t ino;
} tree_node_data_t;

typedef struct {
    char        name[256];
    uint64_t    ino;
    aex_ftype_t ftype;
    uint64_t    size;
    uint64_t    rsrc_size;
    int64_t     mtime;
} file_row_t;

static struct {
    HWND        hwndMain;
    HWND        hwndTree;
    HWND        hwndList;
    HWND        hwndCombo;
    HWND        hwndStatus;
    HFONT       hFont;

    char        currentTarget[MAX_PATH];
    char        currentPassword[256];
    char        currentDirPath[1024];
    aex_t      *fs;

    aex_volume_desc_t   vols[16];
    size_t              vol_count;
    aex_snapshot_desc_t snaps[64];
    size_t              snap_count;

    file_row_t *rows;
    size_t      row_count;
    size_t      row_cap;
} g_ui;

static void set_status(const char *msg) {
    if (g_ui.hwndStatus) SendMessageA(g_ui.hwndStatus, SB_SETTEXTA, 0, (LPARAM)msg);
}

static int browse_for_folder(HWND owner, const char *title, char *out_path) {
    BROWSEINFOA bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return 0;
    BOOL ok = SHGetPathFromIDListA(pidl, out_path);
    CoTaskMemFree(pidl);
    return ok ? 1 : 0;
}

/* Simple modal password prompt window for FileVault 2 / encrypted .sparsebundle / .dmg */
typedef struct {
    char out_pw[256];
    int  accepted;
    HWND hEdit;
} pw_prompt_t;

static LRESULT CALLBACK PwWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    pw_prompt_t *st = (pw_prompt_t *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
            st = (pw_prompt_t *)cs->lpCreateParams;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            CreateWindowExA(0, "STATIC", "Enter FileVault 2 or Disk Image Passphrase:",
                            WS_CHILD | WS_VISIBLE, 14, 14, 330, 20, hwnd, NULL, cs->hInstance, NULL);
            st->hEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                            WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP,
                            14, 38, 330, 24, hwnd, (HMENU)1, cs->hInstance, NULL);
            CreateWindowExA(0, "BUTTON", "Unlock",
                            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                            176, 74, 80, 26, hwnd, (HMENU)IDOK, cs->hInstance, NULL);
            CreateWindowExA(0, "BUTTON", "Cancel",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            264, 74, 80, 26, hwnd, (HMENU)IDCANCEL, cs->hInstance, NULL);
            SetFocus(st->hEdit);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK && st) {
                GetWindowTextA(st->hEdit, st->out_pw, sizeof(st->out_pw));
                st->accepted = 1;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int prompt_password(HWND parent, char *out_pw, size_t cap) {
    static int reg = 0;
    if (!reg) {
        WNDCLASSA wc = { 0 };
        wc.lpfnWndProc = PwWndProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "AEXPasswordDialog";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        reg = 1;
    }
    pw_prompt_t st;
    memset(&st, 0, sizeof(st));
    EnableWindow(parent, FALSE);
    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME, "AEXPasswordDialog", "Unlock Encrypted Apple Volume",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 366, 142, parent, NULL, GetModuleHandleA(NULL), &st
    );
    MSG msg;
    while (IsWindow(dlg) && GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageA(dlg, WM_COMMAND, IDOK, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    if (st.accepted) {
        snprintf(out_pw, cap, "%s", st.out_pw);
        return 1;
    }
    return 0;
}

static void format_size(uint64_t bytes, char *out, size_t cap) {
    if (bytes >= (1ULL << 30)) snprintf(out, cap, "%.2f GB", (double)bytes / 1073741824.0);
    else if (bytes >= (1ULL << 20)) snprintf(out, cap, "%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= 1024ULL) snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    else snprintf(out, cap, "%llu B", (unsigned long long)bytes);
}

static int list_dir_cb(void *user_ctx, const char *name, uint64_t ino, aex_ftype_t ftype) {
    (void)user_ctx;
    if (g_ui.row_count == g_ui.row_cap) {
        size_t ncap = g_ui.row_cap ? g_ui.row_cap * 2U : 128U;
        file_row_t *nr = (file_row_t *)realloc(g_ui.rows, ncap * sizeof(file_row_t));
        if (!nr) return 1;
        g_ui.rows = nr;
        g_ui.row_cap = ncap;
    }
    file_row_t *r = &g_ui.rows[g_ui.row_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->ino = ino;
    r->ftype = ftype;
    aex_attr_t attr;
    if (aex_getattr(g_ui.fs, ino, &attr) == 0) {
        r->size = attr.size;
        r->rsrc_size = attr.rsrc_size;
        r->mtime = attr.mtime_sec;
    }
    return 0;
}

static void populate_listview(const char *dir_path) {
    SendMessageA(g_ui.hwndList, LVM_DELETEALLITEMS, 0, 0);
    g_ui.row_count = 0;
    if (!g_ui.fs) return;

    uint64_t ino = 0;
    if (aex_lookup_path(g_ui.fs, dir_path, &ino) != 0) return;
    snprintf(g_ui.currentDirPath, sizeof(g_ui.currentDirPath), "%s", dir_path);
    (void)aex_readdir(g_ui.fs, ino, list_dir_cb, NULL);

    for (size_t i = 0; i < g_ui.row_count; i++) {
        LVITEMA lvi;
        memset(&lvi, 0, sizeof(lvi));
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = (int)i;
        lvi.pszText = g_ui.rows[i].name;
        lvi.lParam = (LPARAM)i;
        int idx = (int)SendMessageA(g_ui.hwndList, LVM_INSERTITEMA, 0, (LPARAM)&lvi);

        const char *tstr = (g_ui.rows[i].ftype == AEX_FT_DIR) ? "Folder"
                         : (g_ui.rows[i].ftype == AEX_FT_SYMLINK) ? "Symlink" : "File";
        LVITEMA sub = { 0 };
        sub.iSubItem = 1;
        sub.pszText = (LPSTR)tstr;
        SendMessageA(g_ui.hwndList, LVM_SETITEMTEXTA, (WPARAM)idx, (LPARAM)&sub);

        char sz_buf[64] = "-";
        if (g_ui.rows[i].ftype == AEX_FT_REG) format_size(g_ui.rows[i].size, sz_buf, sizeof(sz_buf));
        sub.iSubItem = 2;
        sub.pszText = sz_buf;
        SendMessageA(g_ui.hwndList, LVM_SETITEMTEXTA, (WPARAM)idx, (LPARAM)&sub);

        char rsrc_buf[64] = "-";
        if (g_ui.rows[i].rsrc_size > 0) format_size(g_ui.rows[i].rsrc_size, rsrc_buf, sizeof(rsrc_buf));
        sub.iSubItem = 3;
        sub.pszText = rsrc_buf;
        SendMessageA(g_ui.hwndList, LVM_SETITEMTEXTA, (WPARAM)idx, (LPARAM)&sub);
    }

    char st[512];
    snprintf(st, sizeof(st), "%s (%s)  |  %s  |  %zu items",
             aex_volume_name(g_ui.fs),
             (aex_fs_kind(g_ui.fs) == AEX_FS_APFS) ? "APFS" : (aex_fs_kind(g_ui.fs) == AEX_FS_HFSPLUS) ? "HFS+" : "Classic HFS",
             dir_path, g_ui.row_count);
    set_status(st);
}

typedef struct {
    HWND        hwndTree;
    HTREEITEM   parentItem;
    const char *parentPath;
} tree_fill_t;

static int tree_subdir_cb(void *user_ctx, const char *name, uint64_t ino, aex_ftype_t ftype) {
    if (ftype != AEX_FT_DIR) return 0;
    tree_fill_t *tf = (tree_fill_t *)user_ctx;
    tree_node_data_t *nd = (tree_node_data_t *)calloc(1, sizeof(tree_node_data_t));
    if (!nd) return 1;
    nd->ino = ino;
    if (strcmp(tf->parentPath, "/") == 0) snprintf(nd->full_path, sizeof(nd->full_path), "/%s", name);
    else snprintf(nd->full_path, sizeof(nd->full_path), "%s/%s", tf->parentPath, name);

    TVINSERTSTRUCTA tvi;
    memset(&tvi, 0, sizeof(tvi));
    tvi.hParent = tf->parentItem;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    tvi.item.pszText = (LPSTR)name;
    tvi.item.lParam = (LPARAM)nd;
    tvi.item.cChildren = 1;
    SendMessageA(tf->hwndTree, TVM_INSERTITEMA, 0, (LPARAM)&tvi);
    return 0;
}

static void rebuild_tree(void) {
    SendMessageA(g_ui.hwndTree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    if (!g_ui.fs) return;

    tree_node_data_t *root_nd = (tree_node_data_t *)calloc(1, sizeof(tree_node_data_t));
    snprintf(root_nd->full_path, sizeof(root_nd->full_path), "/");
    root_nd->ino = aex_root_ino(g_ui.fs);

    TVINSERTSTRUCTA tvi;
    memset(&tvi, 0, sizeof(tvi));
    tvi.hParent = TVI_ROOT;
    tvi.hInsertAfter = TVI_FIRST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    tvi.item.pszText = (LPSTR)aex_volume_name(g_ui.fs);
    tvi.item.lParam = (LPARAM)root_nd;
    tvi.item.cChildren = 1;
    HTREEITEM hRoot = (HTREEITEM)SendMessageA(g_ui.hwndTree, TVM_INSERTITEMA, 0, (LPARAM)&tvi);

    tree_fill_t tf = { g_ui.hwndTree, hRoot, "/" };
    (void)aex_readdir(g_ui.fs, root_nd->ino, tree_subdir_cb, &tf);
    SendMessageA(g_ui.hwndTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)hRoot);
    populate_listview("/");
}

static void open_target_with_opts(const char *path, int vol_idx, const char *snap_name, int refresh_combo) {
    if (g_ui.fs) {
        aex_close(g_ui.fs);
        g_ui.fs = NULL;
    }
    snprintf(g_ui.currentTarget, sizeof(g_ui.currentTarget), "%s", path);

    aex_open_options_t opts = { -1, vol_idx, snap_name, g_ui.currentPassword[0] ? g_ui.currentPassword : NULL };
    int rc = aex_open_path(path, &opts, &g_ui.fs);
    if (rc == -EACCES) {
        if (prompt_password(g_ui.hwndMain, g_ui.currentPassword, sizeof(g_ui.currentPassword))) {
            opts.password = g_ui.currentPassword;
            rc = aex_open_path(path, &opts, &g_ui.fs);
        }
    }
    if (rc != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Could not open Apple filesystem on:\n%s\n\nReason: %s", path, aex_strerror(rc));
        MessageBoxA(g_ui.hwndMain, msg, "Apple Explorer", MB_ICONWARNING | MB_OK);
        set_status("Ready");
        return;
    }

    if (refresh_combo) {
        SendMessageA(g_ui.hwndCombo, CB_RESETCONTENT, 0, 0);
        g_ui.vol_count = 0;
        (void)aex_list_volumes_path(path, opts.password, g_ui.vols, 16U, &g_ui.vol_count);
        for (size_t i = 0; i < g_ui.vol_count && i < 16U; i++) {
            char entry[256];
            snprintf(entry, sizeof(entry), "Volume [%u]: %s (%s)", g_ui.vols[i].index, g_ui.vols[i].name, g_ui.vols[i].role);
            SendMessageA(g_ui.hwndCombo, CB_ADDSTRING, 0, (LPARAM)entry);
        }
        g_ui.snap_count = 0;
        (void)aex_list_snapshots(g_ui.fs, g_ui.snaps, 64U, &g_ui.snap_count);
        for (size_t s = 0; s < g_ui.snap_count && s < 64U; s++) {
            char entry[320];
            snprintf(entry, sizeof(entry), "Snapshot: %s", g_ui.snaps[s].name);
            SendMessageA(g_ui.hwndCombo, CB_ADDSTRING, 0, (LPARAM)entry);
        }
        SendMessageA(g_ui.hwndCombo, CB_SETCURSEL, 0, 0);
    }

    rebuild_tree();
}

static void scan_and_open_physical_drive(void) {
    for (int d = 0; d < 16; d++) {
        char dev[64];
        snprintf(dev, sizeof(dev), "\\\\.\\PhysicalDrive%d", d);
        aex_probe_info_t info;
        if (aex_probe_path(dev, NULL, &info) == 0 && info.primary_fs != AEX_FS_UNKNOWN) {
            char ask[256];
            snprintf(ask, sizeof(ask), "Found Apple volume \"%s\" on %s.\nOpen this drive read-only?",
                     info.primary_label[0] ? info.primary_label : "Untitled", dev);
            if (MessageBoxA(g_ui.hwndMain, ask, "Apple Physical Drive Detected", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_ui.currentPassword[0] = '\0';
                open_target_with_opts(dev, -1, NULL, 1);
                return;
            }
        }
    }
    MessageBoxA(g_ui.hwndMain,
                "No HFS, HFS+, or APFS partitions were detected on PhysicalDrive0..15.\n"
                "Note: Reading raw physical drives on Windows requires running AppleExplorer.exe as Administrator.",
                "Apple Explorer — Physical Drive Scan", MB_ICONINFORMATION | MB_OK);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE hInst = ((LPCREATESTRUCTA)lp)->hInstance;
            g_ui.hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            struct { int id; const char *label; int x; int w; } btns[] = {
                { ID_BTN_OPEN_IMAGE,  "Open .dmg / Image...",       8,   140 },
                { ID_BTN_OPEN_BUNDLE, "Open .sparsebundle...",      154, 148 },
                { ID_BTN_OPEN_DISK,   "Open Physical Drive...",     308, 148 },
                { ID_BTN_EXTRACT_SEL, "Extract Selected...",        730, 130 },
                { ID_BTN_EXTRACT_ALL, "Extract Volume...",          866, 120 },
            };
            for (size_t i = 0; i < sizeof(btns)/sizeof(btns[0]); i++) {
                HWND b = CreateWindowExA(0, "BUTTON", btns[i].label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         btns[i].x, 6, btns[i].w, 26, hwnd, (HMENU)(INT_PTR)btns[i].id, hInst, NULL);
                SendMessageA(b, WM_SETFONT, (WPARAM)g_ui.hFont, TRUE);
            }

            g_ui.hwndCombo = CreateWindowExA(0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                464, 7, 258, 240, hwnd, (HMENU)ID_CMB_VOL_SNAP, hInst, NULL);
            SendMessageA(g_ui.hwndCombo, WM_SETFONT, (WPARAM)g_ui.hFont, TRUE);

            g_ui.hwndTree = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TREEVIEWA, "",
                WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                0, 38, 280, 500, hwnd, (HMENU)ID_TREE_DIRS, hInst, NULL);

            g_ui.hwndList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                284, 38, 700, 500, hwnd, (HMENU)ID_LIST_FILES, hInst, NULL);
            SendMessageA(g_ui.hwndList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            const char *cols[] = { "Name", "Type", "Size", "Resource Fork" };
            const int widths[] = { 340, 100, 120, 110 };
            for (int c = 0; c < 4; c++) {
                LVCOLUMNA col = { 0 };
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                col.pszText = (LPSTR)cols[c];
                col.cx = widths[c];
                col.iSubItem = c;
                SendMessageA(g_ui.hwndList, LVM_INSERTCOLUMNA, c, (LPARAM)&col);
            }

            g_ui.hwndStatus = CreateWindowExA(0, STATUSCLASSNAMEA,
                "Ready — Open a .dmg, .sparsebundle, .backupbundle, or physical HFS/HFS+/APFS drive",
                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd, (HMENU)ID_STATUS_BAR, hInst, NULL);
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        }

        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            SendMessageA(g_ui.hwndStatus, WM_SIZE, 0, 0);
            RECT sr; GetWindowRect(g_ui.hwndStatus, &sr);
            int sh = sr.bottom - sr.top;
            int body_h = (h > 38 + sh) ? (h - 38 - sh) : 100;
            int tree_w = (w > 600) ? 290 : (w / 3);
            MoveWindow(g_ui.hwndTree, 4, 38, tree_w, body_h, TRUE);
            MoveWindow(g_ui.hwndList, tree_w + 8, 38, w - tree_w - 12, body_h, TRUE);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wp;
            char dropped[MAX_PATH] = "";
            if (DragQueryFileA(hDrop, 0, dropped, sizeof(dropped))) {
                g_ui.currentPassword[0] = '\0';
                open_target_with_opts(dropped, -1, NULL, 1);
            }
            DragFinish(hDrop);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_BTN_OPEN_IMAGE) {
                char file[MAX_PATH] = "";
                OPENFILENAMEA ofn = { 0 };
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "Apple Disk Images (*.dmg;*.sparseimage;*.img;*.raw)\0*.dmg;*.sparseimage;*.img;*.raw;*.cdr\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = sizeof(file);
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) {
                    g_ui.currentPassword[0] = '\0';
                    open_target_with_opts(file, -1, NULL, 1);
                }
            } else if (id == ID_BTN_OPEN_BUNDLE) {
                char dir[MAX_PATH] = "";
                if (browse_for_folder(hwnd, "Select a .sparsebundle or .backupbundle folder:", dir)) {
                    g_ui.currentPassword[0] = '\0';
                    open_target_with_opts(dir, -1, NULL, 1);
                }
            } else if (id == ID_BTN_OPEN_DISK) {
                scan_and_open_physical_drive();
            } else if (id == ID_CMB_VOL_SNAP && HIWORD(wp) == CBN_SELCHANGE && g_ui.currentTarget[0]) {
                int sel = (int)SendMessageA(g_ui.hwndCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    if ((size_t)sel < g_ui.vol_count) {
                        open_target_with_opts(g_ui.currentTarget, (int)sel, NULL, 0);
                    } else {
                        size_t sidx = (size_t)sel - g_ui.vol_count;
                        if (sidx < g_ui.snap_count) {
                            open_target_with_opts(g_ui.currentTarget, -1, g_ui.snaps[sidx].name, 0);
                        }
                    }
                }
            } else if (id == ID_BTN_EXTRACT_ALL && g_ui.fs) {
                char out_dir[MAX_PATH] = "";
                if (browse_for_folder(hwnd, "Choose destination folder to extract volume:", out_dir)) {
                    set_status("Extracting entire volume...");
                    int rc = aex_extract_path(g_ui.fs, "/", out_dir);
                    if (rc == 0) MessageBoxA(hwnd, "Volume extraction completed.", "Apple Explorer", MB_ICONINFORMATION);
                    else MessageBoxA(hwnd, aex_strerror(rc), "Extraction Error", MB_ICONERROR);
                    populate_listview(g_ui.currentDirPath);
                }
            } else if (id == ID_BTN_EXTRACT_SEL && g_ui.fs) {
                int sel = (int)SendMessageA(g_ui.hwndList, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
                if (sel < 0 || (size_t)sel >= g_ui.row_count) {
                    MessageBoxA(hwnd, "Select a file or folder in the right pane first.", "Apple Explorer", MB_ICONINFORMATION);
                } else {
                    char out_dir[MAX_PATH] = "";
                    if (browse_for_folder(hwnd, "Choose destination folder:", out_dir)) {
                        char src[1400], dst[1400];
                        if (strcmp(g_ui.currentDirPath, "/") == 0) snprintf(src, sizeof(src), "/%s", g_ui.rows[sel].name);
                        else snprintf(src, sizeof(src), "%s/%s", g_ui.currentDirPath, g_ui.rows[sel].name);
                        snprintf(dst, sizeof(dst), "%s\\%s", out_dir, g_ui.rows[sel].name);
                        int rc = aex_extract_path(g_ui.fs, src, dst);
                        if (rc == 0) MessageBoxA(hwnd, "Extraction completed.", "Apple Explorer", MB_ICONINFORMATION);
                        else MessageBoxA(hwnd, aex_strerror(rc), "Extraction Error", MB_ICONERROR);
                    }
                }
            }
            return 0;
        }

        case WM_NOTIFY: {
            LPNMHDR nm = (LPNMHDR)lp;
            if (nm->idFrom == ID_TREE_DIRS && nm->code == TVN_ITEMEXPANDINGA) {
                LPNMTREEVIEWA pnmtv = (LPNMTREEVIEWA)lp;
                if (pnmtv->action == TVE_EXPAND && g_ui.fs) {
                    HTREEITEM hItem = pnmtv->itemNew.hItem;
                    if (!SendMessageA(g_ui.hwndTree, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)hItem)) {
                        tree_node_data_t *nd = (tree_node_data_t *)pnmtv->itemNew.lParam;
                        if (nd) {
                            tree_fill_t tf = { g_ui.hwndTree, hItem, nd->full_path };
                            (void)aex_readdir(g_ui.fs, nd->ino, tree_subdir_cb, &tf);
                        }
                    }
                }
            } else if (nm->idFrom == ID_TREE_DIRS && nm->code == TVN_SELCHANGEDA) {
                LPNMTREEVIEWA pnmtv = (LPNMTREEVIEWA)lp;
                tree_node_data_t *nd = (tree_node_data_t *)pnmtv->itemNew.lParam;
                if (nd) populate_listview(nd->full_path);
            } else if (nm->idFrom == ID_LIST_FILES && nm->code == NM_DBLCLK) {
                int sel = (int)SendMessageA(g_ui.hwndList, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
                if (sel >= 0 && (size_t)sel < g_ui.row_count && g_ui.rows[sel].ftype == AEX_FT_DIR) {
                    char next_dir[1400];
                    if (strcmp(g_ui.currentDirPath, "/") == 0) snprintf(next_dir, sizeof(next_dir), "/%s", g_ui.rows[sel].name);
                    else snprintf(next_dir, sizeof(next_dir), "%s/%s", g_ui.currentDirPath, g_ui.rows[sel].name);
                    populate_listview(next_dir);
                }
            }
            return 0;
        }

        case WM_DESTROY:
            if (g_ui.fs) { aex_close(g_ui.fs); g_ui.fs = NULL; }
            free(g_ui.rows);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShowCmd) {
    (void)hPrev;
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AppleExplorerMainWnd";
    RegisterClassA(&wc);

    g_ui.hwndMain = CreateWindowExA(
        0, "AppleExplorerMainWnd",
        "Apple Explorer v0.1.0 — Portable HFS, HFS+, APFS, FileVault 2, and Time Machine Reader",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1010, 620, NULL, NULL, hInst, NULL
    );
    ShowWindow(g_ui.hwndMain, nShowCmd);

    if (lpCmdLine && lpCmdLine[0]) {
        /* Strip quotes if path passed via drag-drop onto AppleExplorer.exe */
        char arg[MAX_PATH];
        size_t p = 0;
        for (size_t i = 0; lpCmdLine[i] && p + 1 < sizeof(arg); i++) {
            if (lpCmdLine[i] != '"') arg[p++] = lpCmdLine[i];
        }
        arg[p] = '\0';
        if (arg[0]) open_target_with_opts(arg, -1, NULL, 1);
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    CoUninitialize();
    return 0;
}
#endif
