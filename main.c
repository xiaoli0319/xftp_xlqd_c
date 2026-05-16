#include <ncurses.h>
#include <langinfo.h>
#include <locale.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <errno.h>
#include <time.h>

// ─── Config ───
#define CFG_DIR ".config/xftp_xlqd"
#define EXT_COLS 6

// ─── File Entry ───
typedef struct { char name[256]; int is_dir; off_t size; char perm[12]; } Entry;
typedef struct { Entry *e; int n, cap, sel, scr; char path[1024]; } Entries;

static Entries ent[2];  // 0=local, 1=remote
static int act = 0;      // active pane

// ─── SFTP Session ───
static LIBSSH2_SESSION *ssh = NULL;
static LIBSSH2_SFTP *sftp = NULL;
static int sock = -1;

// ─── Connection Config ───
static char cfg_host[128] = "", cfg_user[64] = "", cfg_pass[128] = "";
static int cfg_port = 22;

// ─── Path helpers ───
static void path_join(char *dst, const char *a, const char *b) {
    int la = strlen(a);
    if (la && a[la-1] == '/') snprintf(dst, 1024, "%s%s", a, b);
    else snprintf(dst, 1024, "%s/%s", a, b);
}

// ─── Config persistence ───
static void cfg_save(void) {
    struct passwd *pw = getpwuid(getuid());
    char d[512]; snprintf(d, 512, "%s/%s", pw->pw_dir, CFG_DIR);
    mkdir(d, 0755);
    char p[576]; snprintf(p, 576, "%s/config", d);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "host=%s\nuser=%s\nport=%d\n", cfg_host, cfg_user, cfg_port);
    fclose(f);
}
static void cfg_load(void) {
    struct passwd *pw = getpwuid(getuid());
    char p[576]; snprintf(p, 576, "%s/%s/config", pw->pw_dir, CFG_DIR);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, 256, f)) {
        char k[64], v[192];
        if (sscanf(line, "%63[^=]=%191s", k, v) >= 2) {
            if (!strcmp(k,"host")) strncpy(cfg_host, v, 127);
            else if (!strcmp(k,"user")) strncpy(cfg_user, v, 63);
            else if (!strcmp(k,"port")) cfg_port = atoi(v);
        }
    }
    fclose(f);
}

// ─── Entries helpers ───
static void ent_free(int side) {
    free(ent[side].e); ent[side].e = NULL;
    ent[side].n = ent[side].cap = ent[side].sel = ent[side].scr = 0;
}
static void ent_add(int side, const char *name, int is_dir, off_t size, const char *perm) {
    Entries *en = &ent[side];
    if (en->n >= en->cap) {
        en->cap = en->cap ? en->cap*2 : 256;
        en->e = realloc(en->e, en->cap * sizeof(Entry));
    }
    Entry *e = &en->e[en->n++];
    strncpy(e->name, name, 255); e->is_dir = is_dir; e->size = size;
    if (perm) strncpy(e->perm, perm, 11); else e->perm[0] = 0;
}
static void ent_sort(int side) {
    int n = ent[side].n;
    Entry *a = ent[side].e;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcasecmp(a[j-1].name, a[j].name) > 0; j--) {
            Entry t = a[j]; a[j] = a[j-1]; a[j-1] = t;
        }
}

// ─── Local FS ───
static int local_read(const char *path) {
    int side = 0;
    ent_free(side);
    strncpy(ent[side].path, path, 1023);
    DIR *d = opendir(path); if (!d) return -1;
    struct dirent *de; struct stat st;
    char fp[1024];
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name,".")) continue;
        path_join(fp, path, de->d_name);
        int is_dir = 0;
        if (stat(fp, &st) == 0) is_dir = S_ISDIR(st.st_mode);
        ent_add(side, de->d_name, is_dir, is_dir ? 0 : st.st_size, NULL);
    }
    closedir(d);
    ent_sort(side);
    return 0;
}
static int local_cd(const char *sub) {
    char np[1024];
    if (!strcmp(sub,"..")) {
        char *s = strrchr(ent[0].path, '/');
        if (s && s != ent[0].path) { *s = 0; return local_read(ent[0].path); }
    } else { path_join(np, ent[0].path, sub); return local_read(np); }
    return 0;
}

// ─── Remote SFTP ───
static int sftp_connect(void) {
    struct hostent *he = gethostbyname(cfg_host); if (!he) return -1;
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(cfg_port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    sock = socket(AF_INET, SOCK_STREAM, 0); if (sock < 0) return -1;
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); sock=-1; return -1; }
    ssh = libssh2_session_init(); if (!ssh) { close(sock); sock=-1; return -1; }
    libssh2_session_set_blocking(ssh, 1);
    if (libssh2_session_handshake(ssh, sock)) { libssh2_session_free(ssh); ssh=NULL; close(sock); sock=-1; return -1; }
    if (libssh2_userauth_password(ssh, cfg_user, cfg_pass))
    { libssh2_session_free(ssh); ssh=NULL; close(sock); sock=-1; return -1; }
    sftp = libssh2_sftp_init(ssh); if (!sftp) { libssh2_session_free(ssh); ssh=NULL; close(sock); sock=-1; return -1; }
    return 0;
}
static void sftp_disconnect(void) {
    if (sftp) { libssh2_sftp_shutdown(sftp); sftp=NULL; }
    if (ssh) { libssh2_session_disconnect(ssh, "bye"); libssh2_session_free(ssh); ssh=NULL; }
    if (sock>=0) { close(sock); sock=-1; }
}
static int remote_read(const char *path) {
    int side = 1;
    ent_free(side);
    if (!sftp) return -1;
    strncpy(ent[side].path, path, 1023);
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_opendir(sftp, path);
    if (!h) return -1;
    char name[512]; LIBSSH2_SFTP_ATTRIBUTES attr;
    while (libssh2_sftp_readdir(h, name, 512, &attr) > 0) {
        if (!strcmp(name,".") || !strcmp(name,"..")) continue;
        int is_dir = LIBSSH2_SFTP_S_ISDIR(attr.permissions);
        char perm[12] = "";
        if (attr.permissions) {
            perm[0] = LIBSSH2_SFTP_S_ISDIR(attr.permissions)?'d':'-';
            perm[1] = attr.permissions & 0400?'r':'-';
            perm[2] = attr.permissions & 0200?'w':'-';
            perm[3] = attr.permissions & 0100?'x':'-';
            perm[4] = attr.permissions & 0040?'r':'-';
            perm[5] = attr.permissions & 0020?'w':'-';
            perm[6] = attr.permissions & 0010?'x':'-';
            perm[7] = attr.permissions & 0004?'r':'-';
            perm[8] = attr.permissions & 0002?'w':'-';
            perm[9] = attr.permissions & 0001?'x':'-';
            perm[10] = 0;
        }
        ent_add(side, name, is_dir, is_dir ? 0 : attr.filesize, perm);
    }
    libssh2_sftp_closedir(h);
    ent_sort(side);
    return 0;
}
static int remote_cd(const char *sub) {
    char np[1024];
    if (!strcmp(sub,"..")) {
        char *s = strrchr(ent[1].path, '/');
        if (s && s != ent[1].path) { *s = 0; return remote_read(ent[1].path); }
    } else { path_join(np, ent[1].path, sub); return remote_read(np); }
    return 0;
}

// ─── Transfer ───
static int xfer_download(const char *rpath, const char *lpath) {
    LIBSSH2_SFTP_HANDLE *rf = libssh2_sftp_open(sftp, rpath, LIBSSH2_FXF_READ, 0);
    if (!rf) return -1;
    FILE *lf = fopen(lpath, "wb"); if (!lf) { libssh2_sftp_close(rf); return -1; }
    char buf[16384]; int n;
    while ((n = libssh2_sftp_read(rf, buf, 16384)) > 0) fwrite(buf, 1, n, lf);
    fclose(lf); libssh2_sftp_close(rf);
    return 0;
}
static int xfer_upload(const char *lpath, const char *rpath) {
    FILE *lf = fopen(lpath, "rb"); if (!lf) return -1;
    LIBSSH2_SFTP_HANDLE *rf = libssh2_sftp_open(sftp, rpath,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR);
    if (!rf) { fclose(lf); return -1; }
    char buf[16384]; int n;
    while ((n = fread(buf, 1, 16384, lf)) > 0) libssh2_sftp_write(rf, buf, n);
    fclose(lf); libssh2_sftp_close(rf);
    return 0;
}

// ─── NCURSES UI ───
static WINDOW *w_pane[2], *w_status;

static void ui_init(void) {
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    use_default_colors();
    start_color();
    init_pair(1, COLOR_CYAN, -1);   // active pane border/title
    init_pair(2, COLOR_WHITE, COLOR_BLUE);   // active pane content (keep for selection bar)
    init_pair(3, COLOR_WHITE, -1);  // inactive pane
    init_pair(4, COLOR_YELLOW, -1); // directory name
    init_pair(5, COLOR_RED, -1);    // status error
    init_pair(6, COLOR_GREEN, -1);  // status ok
    init_pair(7, COLOR_BLACK, COLOR_WHITE);  // selected item (keep highlight)
    // Create pane windows once
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int pw = (cols - 3) / 2;
    w_pane[0] = newwin(rows-4, pw, 2, 0);
    w_pane[1] = newwin(rows-4, pw, 2, pw+3);
    w_status = newwin(1, cols, rows-1, 0);
}
static void ui_end(void) { endwin(); }

static void pane_draw(int side) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int pw = (cols - 3) / 2, ph = rows - 4;
    int px = side ? pw + 3 : 0;
    // Recreate windows on terminal resize
    if (w_pane[side]) {
        int wy, wx; getbegyx(w_pane[side], wy, wx);
        if (wx != px || wy != 2 || getmaxy(w_pane[side]) != ph || getmaxx(w_pane[side]) != pw) {
            delwin(w_pane[side]);
            w_pane[side] = newwin(ph, pw, 2, px);
        }
    }
    WINDOW *w = w_pane[side];
    int hi = side == act;
    werase(w);
    wattron(w, COLOR_PAIR(hi ? 1 : 3));
    box(w, 0, 0);
    wattroff(w, COLOR_PAIR(hi ? 1 : 3));

    // Title
    const char *label = side ? "  Remote  " : "  Local  ";
    wattron(w, COLOR_PAIR(hi ? 1 : 3));
    mvwprintw(w, 0, 2, "%s", label);
    wattroff(w, COLOR_PAIR(hi ? 1 : 3));

    // Path
    mvwprintw(w, 1, 1, "%-*.*s", pw-2, pw-2, ent[side].path);

    // Files
    Entries *en = &ent[side];
    int mh = ph - 3;
    if (en->sel < en->scr) en->scr = en->sel;
    if (en->sel >= en->scr + mh) en->scr = en->sel - mh + 1;

    for (int i = 0; i < mh && i + en->scr < en->n; i++) {
        Entry *e = &en->e[i + en->scr];
        int y = i + 2;
        int sel = (i + en->scr == en->sel);
        if (sel) wattron(w, COLOR_PAIR(7));
        else if (e->is_dir) wattron(w, COLOR_PAIR(4));

        char size_str[16] = "";
        if (!e->is_dir) {
            if (e->size < 1024) snprintf(size_str, 16, "%ld", (long)e->size);
            else if (e->size < 1024*1024) snprintf(size_str, 16, "%.1fK", e->size/1024.0);
            else snprintf(size_str, 16, "%.1fM", e->size/(1024.0*1024));
        }
        if (e->perm[0]) mvwprintw(w, y, 1, "%s ", e->perm);
        int pad = pw - 2 - (e->perm[0] ? 12 : 0) - (size_str[0] ? (int)strlen(size_str)+1 : 0);
        if (pad < 0) pad = 0;
        const char *suffix = e->is_dir ? "/" : "";
        char disp[256]; snprintf(disp, 256, "%s%s", e->name, suffix);
        mvwprintw(w, y, 1 + (e->perm[0] ? 11 : 0), "%-*s %s", pad, disp, size_str);

        if (sel) wattroff(w, COLOR_PAIR(7));
        else if (e->is_dir) wattroff(w, COLOR_PAIR(4));
    }
    wrefresh(w);
}
static void status_draw(const char *msg, int is_err) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    if (!w_status) {
        w_status = newwin(1, cols, rows-1, 0);
    }
    werase(w_status);
    if (is_err) wattron(w_status, COLOR_PAIR(5));
    else wattron(w_status, COLOR_PAIR(6));
    wprintw(w_status, "  %-*s", cols-3, msg ? msg : "");
    wattroff(w_status, COLOR_PAIR(is_err ? 5 : 6));
    wrefresh(w_status);
}

static void refresh_all(const char *msg, int is_err) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    move(0, 0); clrtoeol(); attron(A_BOLD);
    mvprintw(0, (cols-12)/2, "  xFTP xl_qd  ");
    attroff(A_BOLD);
    mvprintw(rows-2, 0, "  Tab=切换  Shift+方向=进目录  Enter=进入  F5=传输  F7=新建  F8=删除  F10=退出");
    wnoutrefresh(stdscr);
    pane_draw(0); pane_draw(1);
    status_draw(msg, is_err);
}

// ─── Dialogs ───
static int confirm_dialog(const char *msg) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int h = 5, w = 66, x = (cols-w)/2, y = (rows-h)/2;
    WINDOW *d = newwin(h, w, y, x);
    wattron(d, COLOR_PAIR(1)); box(d, 0, 0); wattroff(d, COLOR_PAIR(1));
    mvwprintw(d, 1, 2, "%-*s", w-4, msg);
    mvwprintw(d, 3, 2, "  Y=确认  其他键=取消  ");
    wrefresh(d);
    int ch = getch();
    delwin(d); refresh_all(NULL, 0);
    return (ch == 'y' || ch == 'Y');
}

static void input_dialog(const char *prompt, char *buf, int len) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int h = 5, w = 66, x = (cols-w)/2, y = (rows-h)/2;
    WINDOW *d = newwin(h, w, y, x);
    wattron(d, COLOR_PAIR(1)); box(d, 0, 0); wattroff(d, COLOR_PAIR(1));
    mvwprintw(d, 1, 2, "%s", prompt);
    echo(); curs_set(1);
    mvwgetnstr(d, 3, 2, buf, len-1);
    noecho(); curs_set(0);
    delwin(d); refresh_all(NULL, 0);
}

// ─── Operations ───
static void op_transfer(void) {
    Entries *src = &ent[act];
    if (src->sel < 0 || src->sel >= src->n) return;
    Entry *e = &src->e[src->sel];
    if (e->is_dir) return;

    if (act == 1) {  // remote → local
        char lp[1024]; path_join(lp, ent[0].path, e->name);
        char msg[256]; snprintf(msg,256,"下载 %s 到本地？", e->name);
        if (!confirm_dialog(msg)) return;
        if (xfer_download(e->name, lp) == 0) {
            local_read(ent[0].path);
            refresh_all("下载完成", 0);
        } else refresh_all("下载失败", 1);
    } else {  // local → remote
        char rp[1024]; path_join(rp, ent[1].path, e->name);
        char msg[256]; snprintf(msg,256,"上传 %s 到远程？", e->name);
        if (!confirm_dialog(msg)) return;
        if (xfer_upload(e->name, rp) == 0) {
            remote_read(ent[1].path);
            refresh_all("上传完成", 0);
        } else refresh_all("上传失败", 1);
    }
}

static void op_delete(void) {
    Entries *en = &ent[act];
    if (en->sel < 0 || en->sel >= en->n) return;
    Entry *e = &en->e[en->sel];
    char msg[256]; snprintf(msg,256,"确认删除 %s%s？", e->name, e->is_dir?"/":"");
    if (!confirm_dialog(msg)) return;

    int ok = 0;
    if (act == 0) {  // local
        if (e->is_dir) ok = (rmdir(e->name) == 0);
        else ok = (unlink(e->name) == 0);
        if (ok) local_read(ent[0].path);
    } else {  // remote
        if (sftp) {
            if (e->is_dir) ok = (libssh2_sftp_rmdir(sftp, e->name) == 0);
            else ok = (libssh2_sftp_unlink(sftp, e->name) == 0);
            if (ok) remote_read(ent[1].path);
        }
    }
    refresh_all(ok ? "删除完成" : "删除失败", !ok);
}

static void op_mkdir(void) {
    char name[128] = "";
    input_dialog("输入新目录名称：", name, 128);
    if (!name[0]) return;

    int ok = 0;
    if (act == 0) {  // local
        ok = (mkdir(name, 0755) == 0);
        if (ok) local_read(ent[0].path);
    } else {  // remote
        if (sftp) {
            ok = (libssh2_sftp_mkdir(sftp, name,
                LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP) == 0);
            if (ok) remote_read(ent[1].path);
        }
    }
    refresh_all(ok ? "目录已创建" : "创建失败", !ok);
}

// ─── Connection dialog ───
static int connect_dialog(void) {
    cfg_load();
    input_dialog("主机地址（hostname 或 IP）：", cfg_host, 127);
    if (!cfg_host[0]) return -1;
    input_dialog("用户名：", cfg_user, 63);
    if (!cfg_user[0]) return -1;
    char port_buf[16]; snprintf(port_buf,16,"%d",cfg_port);
    input_dialog("端口（默认22）：", port_buf, 15);
    if (port_buf[0]) cfg_port = atoi(port_buf);
    input_dialog("密码：", cfg_pass, 127);
    if (!cfg_pass[0]) return -1;
    cfg_save();
    return 0;
}

// ─── Main ───
int main(void) {
    setlocale(LC_ALL, "");
    libssh2_init(0);

    ui_init();
    refresh_all("正在连接...", 0);

    if (connect_dialog() < 0) { ui_end(); libssh2_exit(); return 1; }

    refresh_all("正在连接 SFTP...", 0);
    if (sftp_connect() < 0) {
        refresh_all("连接失败！", 1);
        napms(2000);
        ui_end(); libssh2_exit(); return 1;
    }
    cfg_save();

    // Initial directory listing
    char cwd[1024]; getcwd(cwd, 1024);
    local_read(cwd);
    remote_read("/");

    refresh_all("已连接", 0);

    // ─── Main Loop ───
    while (1) {
        refresh_all(NULL, 0);
        int ch = getch();
        Entries *en = &ent[act];

        switch (ch) {
            case 9:  // Tab
            case KEY_BTAB:
                act = !act;
                break;

            case KEY_UP:
                if (en->sel > 0) en->sel--;
                break;

            case KEY_DOWN:
                if (en->sel < en->n - 1) en->sel++;
                break;

            case KEY_SLEFT:  // Shift+← 进本地目录
                {
                Entries *le = &ent[0];
                if (le->n > 0 && le->sel >= 0 && le->sel < le->n && le->e[le->sel].is_dir) {
                    char buf[256]; strcpy(buf, le->e[le->sel].name);
                    local_cd(buf);
                    le->sel = 0; le->scr = 0;
                }
                }
                break;

            case KEY_SRIGHT:  // Shift+→ 进远程目录
                {
                Entries *re = &ent[1];
                if (re->n > 0 && re->sel >= 0 && re->sel < re->n && re->e[re->sel].is_dir) {
                    char buf[256]; strcpy(buf, re->e[re->sel].name);
                    remote_cd(buf);
                    re->sel = 0; re->scr = 0;
                }
                }
                break;

            case KEY_PPAGE:  // PageUp
                {
                int ph;
                getmaxyx(stdscr, ph, ph);
                ph -= 4;
                en->sel -= ph; if (en->sel < 0) en->sel = 0;
                }
                break;

            case KEY_NPAGE:  // PageDown
                {
                int ph;
                getmaxyx(stdscr, ph, ph);
                ph -= 4;
                en->sel += ph; if (en->sel >= en->n) en->sel = en->n - 1;
                }
                break;

            case KEY_HOME:
                en->sel = 0; en->scr = 0;
                break;

            case KEY_END:
                en->sel = en->n - 1;
                break;

            case '\n':
            case KEY_ENTER:
                if (en->n == 0) break;
                Entry *e = &en->e[en->sel];
                if (e->is_dir) {
                    if (act == 0) local_cd(e->name);
                    else remote_cd(e->name);
                    en->sel = 0; en->scr = 0;
                }
                break;

            case KEY_F(5):
                op_transfer();
                break;

            case KEY_F(7):
                op_mkdir();
                break;

            case KEY_F(8):
                op_delete();
                break;

            case 'q':
            case 'Q':
            case KEY_F(10):
                if (confirm_dialog("确认退出？")) goto done;
                break;

            case KEY_RESIZE:
                break;

            default:
                break;
        }
    }

done:
    sftp_disconnect();
    ent_free(0); ent_free(1);
    ui_end(); libssh2_exit();
    return 0;
}
