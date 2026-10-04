// Save WebDAV v1.0  -  10 個資料夾 -> 7z -> WebDAV, 保留最新 5 份
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cmath>
#include <map>
#include <deque>
#include <cstdarg>
#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
using namespace std;

static const char* CFG_DIR = "sdmc:/config/Save_WebDAV/";
static const char* CFG_FILE = "sdmc:/config/Save_WebDAV/config.txt";
static const char* TMP_FILE = "sdmc:/config/Save_WebDAV/tmp.7z";
static const char* BEFORE_FILE = "sdmc:/config/Save_WebDAV/before_restore.7z";
static const char* TEST_FILE = "sdmc:/config/Save_WebDAV/test.tmp";
static const char* LOG_FILE = "sdmc:/config/Save_WebDAV/log.txt";
static const size_t KEEP = 5;

struct Cfg { string url, user, pass, name; vector<string> dirs; } cfg;
static PadState pad;
static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static int progLast = -1;
static bool pgActive = false, pgOk = false, pgErr = false;
static int pgPct = 0, pgBase = 0, pgSpan = 100, totalFiles = 0, restoredCount = 0, archTotal = 0;
static string pgTitle, pgStage;
static vector<string> pgList;
static void setPct(int p);
static long long totalBytes = 0;
static bool gCancel = false;   // 按住 B 取消
static bool cancelReq(bool force = false) {
    static int c = 0;
    if (gCancel) return true;
    if (force || (++c & 7) == 0) { padUpdate(&pad); if (padGetButtons(&pad) & HidNpadButton_B) gCancel = true; }
    return gCancel;
}
static string esc(const string& n) { char* e = curl_easy_escape(NULL, n.c_str(), 0); string r = e ? e : n; if (e) curl_free(e); return r; }
static int prog(void*, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    if (cancelReq(true)) return 1;
    curl_off_t t = dt ? dt : ut, n = dt ? dn : un;
    if (t > 0) { int p = (int)(n * 100 / t); if (p != progLast) { progLast = p; setPct(pgBase + p * pgSpan / 100); } }
    return 0;
}

// ---------- 小工具 ----------
static void mkdirs(const string& p) {
    size_t i = p.find(":/");
    i = (i == string::npos) ? 0 : i + 2;
    while ((i = p.find('/', i)) != string::npos) { mkdir(p.substr(0, i).c_str(), 0777); i++; }
}

static string normDir(string d) {
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    if (d.rfind("sdmc:/", 0) != 0) {
        if (!d.empty() && d[0] == '/') d = "sdmc:" + d; else d = "sdmc:/" + d;
    }
    return d;
}

// ---------- 除錯記錄 (閃退時可看停在哪一步) ----------
static void dbg(const char* fmt, ...) {
    static bool made = false;
    if (!made) { mkdirs(string(CFG_DIR)); made = true; }
    FILE* f = fopen(LOG_FILE, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// ---------- 設定檔 ----------
static void loadCfg() {
    FILE* f = fopen(CFG_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t e = s.find('=');
        if (e == string::npos) continue;
        string k = s.substr(0, e), v = s.substr(e + 1);
        if (k == "url") cfg.url = v;
        else if (k == "user") cfg.user = v;
        else if (k == "pass") cfg.pass = v;
        else if (k == "name") cfg.name = v;
        else if (k.rfind("dir", 0) == 0 && !v.empty() && cfg.dirs.size() < 20) cfg.dirs.push_back(v);
    }
    fclose(f);
}
static void saveCfg() {
    mkdirs(string(CFG_DIR));
    FILE* f = fopen(CFG_FILE, "w");
    if (!f) return;
    fprintf(f, "name=%s\nurl=%s\nuser=%s\npass=%s\n", cfg.name.c_str(), cfg.url.c_str(), cfg.user.c_str(), cfg.pass.c_str());
    for (size_t i = 0; i < cfg.dirs.size(); i++) fprintf(f, "dir%zu=%s\n", i + 1, cfg.dirs[i].c_str());
    fclose(f);
}

// ---------- 軟體鍵盤 ----------
static bool kbd(const char* header, string& val, bool pw = false) {
    SwkbdConfig k;
    if (R_FAILED(swkbdCreate(&k, 0))) return false;
    swkbdConfigMakePresetDefault(&k);
    swkbdConfigSetHeaderText(&k, header);
    swkbdConfigSetInitialText(&k, val.c_str());
    swkbdConfigSetStringLenMax(&k, 200);
    if (pw) swkbdConfigSetPasswordFlag(&k, true);   // 輸入時顯示 ****
    char out[1024] = {0};
    Result rc = swkbdShow(&k, out, sizeof out);
    swkbdClose(&k);
    if (R_SUCCEEDED(rc)) { val = out; return true; }
    return false;
}

// ---------- WebDAV ----------
static string baseUrl() {
    string u = cfg.url;
    if (!u.empty() && u.back() != '/') u += '/';
    return u;
}
static size_t cbStr(void* p, size_t s, size_t n, void* u) { ((string*)u)->append((char*)p, s * n); return s * n; }
static size_t cbW(void* p, size_t s, size_t n, void* u) { return fwrite(p, s, n, (FILE*)u); }
static size_t cbR(void* p, size_t s, size_t n, void* u) { return fread(p, s, n, (FILE*)u); }

static long lastHttp = 0;
static CURLcode lastRc = CURLE_OK;

static string describeErr() {   // 把錯誤代碼翻成中文
    switch (lastRc) {
        case CURLE_OK: break;
        case CURLE_UNSUPPORTED_PROTOCOL: case CURLE_URL_MALFORMAT: return "網址格式錯誤，請檢查 WebDAV 位址";
        case CURLE_COULDNT_RESOLVE_HOST: return "找不到伺服器，請檢查網址或網路連線";
        case CURLE_COULDNT_CONNECT: return "無法連線到伺服器，請檢查網路或網址";
        case CURLE_OPERATION_TIMEDOUT: return "連線逾時，網路太慢或已中斷";
        case CURLE_SSL_CONNECT_ERROR: case CURLE_PEER_FAILED_VERIFICATION: return "HTTPS 連線失敗";
        case CURLE_ABORTED_BY_CALLBACK: return "已取消";
        case CURLE_SEND_ERROR: case CURLE_RECV_ERROR: case CURLE_GOT_NOTHING: return "網路中斷或伺服器沒有回應";
        case CURLE_WRITE_ERROR: return "寫入 SD 卡失敗，空間可能不足";
        default: return "網路錯誤 (代碼 " + to_string((int)lastRc) + ")";
    }
    switch (lastHttp) {
        case 401: return "帳號或密碼錯誤 (HTTP 401)";
        case 403: return "沒有權限，請確認帳號權限 (HTTP 403)";
        case 404: return "WebDAV 資料夾或檔案不存在，請檢查位址 (HTTP 404)";
        case 405: return "伺服器不接受這個操作，位址可能不是 WebDAV 資料夾 (HTTP 405)";
        case 409: return "上層資料夾不存在 (HTTP 409)";
        case 423: return "檔案被鎖定 (HTTP 423)";
        case 507: return "伺服器空間不足 (HTTP 507)";
        default: break;
    }
    if (lastHttp >= 500) return "伺服器發生錯誤 (HTTP " + to_string(lastHttp) + ")";
    return "伺服器回應異常 (HTTP " + to_string(lastHttp) + ")";
}

static CURL* mk(const string& url) {
    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    string up = cfg.user + ":" + cfg.pass;
    curl_easy_setopt(c, CURLOPT_USERPWD, up.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);   // Switch 沒有內建憑證庫
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // 40 秒內速度低於 1KB/s 就當作斷線
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 40L);
    return c;
}
static bool run(CURL* c) {
    lastRc = curl_easy_perform(c);
    lastHttp = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &lastHttp);
    return lastRc == CURLE_OK && lastHttp >= 200 && lastHttp < 300;
}

static bool davPropfind(int depth, string* body) {
    CURL* c = mk(baseUrl());
    string sink;
    curl_slist* h = curl_slist_append(NULL, depth ? "Depth: 1" : "Depth: 0");
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cbStr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body ? body : &sink);
    bool ok = run(c);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    return ok;
}
static bool davExists() { return davPropfind(0, NULL); }

static bool davList(vector<string>& names, const string& prefix) {
    names.clear();
    string body;
    if (!davPropfind(1, &body)) { logf_("讀取遠端失敗: %s", describeErr().c_str()); return false; }
    string low = body;
    for (auto& ch : low) ch = tolower((unsigned char)ch);
    size_t pos = 0;
    while ((pos = low.find("href>", pos)) != string::npos) {
        pos += 5;
        size_t e = body.find('<', pos);
        if (e == string::npos) break;
        string href = body.substr(pos, e - pos);
        while (!href.empty() && href.back() == '/') href.pop_back();
        size_t sl = href.rfind('/');
        string nm = (sl == string::npos) ? href : href.substr(sl + 1);
        char* un = curl_easy_unescape(NULL, nm.c_str(), 0, NULL);
        if (un) { nm = un; curl_free(un); }
        // 檔名格式: Save_名稱_YYYYMMDD_HHMMSS.7z  (.tmp 是上傳中的暫存檔，不列入)
        if (nm.rfind(prefix, 0) == 0 && nm.size() > 18 && nm.substr(nm.size() - 3) == ".7z"
            && find(names.begin(), names.end(), nm) == names.end())
            names.push_back(nm);
        pos = e;
    }
    sort(names.begin(), names.end(), [](const string& x, const string& y) {
        string dx = x.substr(x.size() - 18, 15), dy = y.substr(y.size() - 18, 15);
        return dx != dy ? dx > dy : x > y; });   // 新 -> 舊
    return true;
}

static bool davPut(const string& name, const char* file) {
    FILE* f = fopen(file, "rb");
    if (!f) { lastRc = CURLE_READ_ERROR; lastHttp = 0; return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    CURL* c = mk(baseUrl() + esc(name));
    curl_slist* h = curl_slist_append(NULL, "Expect:");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, prog); progLast = -1;
    curl_easy_setopt(c, CURLOPT_READFUNCTION, cbR);
    curl_easy_setopt(c, CURLOPT_READDATA, f);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)sz);
    string sink;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cbStr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    bool ok = run(c);
    curl_slist_free_all(h); curl_easy_cleanup(c); fclose(f);
    return ok;
}

static bool davDel(const string& name) {
    CURL* c = mk(baseUrl() + esc(name));
    string sink;
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cbStr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    bool ok = run(c);
    curl_easy_cleanup(c);
    return ok;
}

static bool davMove(const string& from, const string& to) {   // 上傳完才改成正式檔名
    CURL* c = mk(baseUrl() + esc(from));
    string dest = "Destination: " + baseUrl() + esc(to);
    curl_slist* h = curl_slist_append(NULL, dest.c_str());
    h = curl_slist_append(h, "Overwrite: T");
    string sink;
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "MOVE");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cbStr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    bool ok = run(c);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    return ok;
}

static long long davSize(const string& name) {   // 取得遠端檔案大小，-1 = 不知道
    CURL* c = mk(baseUrl() + esc(name));
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    bool ok = run(c);
    curl_off_t len = -1;
    if (ok) curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    curl_easy_cleanup(c);
    return ok ? (long long)len : -1;
}

static bool davGet(const string& name, const char* file) {
    FILE* f = fopen(file, "wb");
    if (!f) { lastRc = CURLE_WRITE_ERROR; lastHttp = 0; return false; }
    CURL* c = mk(baseUrl() + esc(name));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cbW);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, prog); progLast = -1;
    bool ok = run(c);
    curl_easy_cleanup(c); fclose(f);
    if (!ok) remove(file);
    return ok;
}

static bool davMkcolAbs(const string& url) {
    CURL* c = mk(url);
    string sink;
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "MKCOL");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cbStr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    bool ok = run(c);
    curl_easy_cleanup(c);
    return ok;
}
static bool davEnsureDir() {   // 逐層建立 WebDAV 位址裡的資料夾
    string u = baseUrl();
    size_t p = u.find("://");
    if (p == string::npos) return false;
    size_t i = u.find('/', p + 3);
    if (i == string::npos) return davExists();
    i++;
    while (i < u.size()) {
        size_t e = u.find('/', i);
        if (e == string::npos) break;
        davMkcolAbs(u.substr(0, e + 1));   // 已存在會回 405，不用理會
        i = e + 1;
    }
    return davExists();
}

// ---------- 7z 打包 / 解壓 ----------
static int countFiles(const string& dir) {
    int n = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        string f = dir + "/" + de->d_name;
        struct stat st;
        if (stat(f.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) n += countFiles(f); else { n++; totalBytes += (long long)st.st_size; }
    }
    closedir(d);
    return n;
}
static string packErr;
static void cleanTmp() {   // libarchive 的 7z 需要暫存檔，Switch 沒有 /tmp，所以指定到 SD 卡
    DIR* d = opendir(CFG_DIR);
    if (!d) return;
    struct dirent* e; vector<string> v;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "libarchive_", 11)) v.push_back(string(CFG_DIR) + e->d_name);
    closedir(d);
    for (auto& f : v) remove(f.c_str());
}
static int packedCount = 0;
static vector<char> ioBuf(1 << 16);

static void addFile(struct archive* a, const string& full, const struct stat& st) {
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) { logf_("\n略過(無法讀取): %s\n", full.c_str()); return; }
    struct archive_entry* e = archive_entry_new();
    archive_entry_set_pathname(e, full.substr(6).c_str());   // 去掉 "sdmc:/"
    archive_entry_set_size(e, st.st_size);
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_mtime(e, st.st_mtime, 0);
    if (archive_write_header(a, e) < ARCHIVE_WARN) {
        packErr = archive_error_string(a) ? archive_error_string(a) : "write header";
        archive_entry_free(e); fclose(f); return;
    }
    size_t n;
    while ((n = fread(ioBuf.data(), 1, ioBuf.size(), f)) > 0) {
        if (cancelReq()) { packErr = "已取消"; break; }
        if (archive_write_data(a, ioBuf.data(), n) < 0) { packErr = archive_error_string(a) ? archive_error_string(a) : "write data"; break; }
    }
    archive_entry_free(e);
    fclose(f);
    ++packedCount;
    if (totalFiles > 0) { int p = pgBase + (int)((long long)packedCount * pgSpan / totalFiles); if (p > pgPct) setPct(p); }
}

static void walk(struct archive* a, const string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) { logf_("\n找不到資料夾: %s\n", dir.c_str()); return; }
    struct dirent* de;
    while ((de = readdir(d))) {
        if (!packErr.empty()) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        string full = dir + "/" + de->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            struct archive_entry* e = archive_entry_new();
            archive_entry_set_pathname(e, (full.substr(6) + "/").c_str());
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_perm(e, 0755);
            if (archive_write_header(a, e) < ARCHIVE_WARN) packErr = archive_error_string(a) ? archive_error_string(a) : "write dir";
            archive_entry_free(e);
            walk(a, full);
        } else addFile(a, full, st);
    }
    closedir(d);
}

static bool verify7zFull(const char* file, int expectFiles) {   // 完整讀過一遍，確認沒壞
    struct archive* a = archive_read_new();
    archive_read_support_format_7zip(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, file, 1 << 16) != ARCHIVE_OK) {
        logf_("驗證失敗: %s", archive_error_string(a) ? archive_error_string(a) : "無法開啟");
        archive_read_free(a); return false;
    }
    struct archive_entry* e; int files = 0, hr; bool ok = true;
    while ((hr = archive_read_next_header(a, &e)) == ARCHIVE_OK || hr == ARCHIVE_WARN) {
        if (cancelReq()) { logf_("已取消"); ok = false; break; }
        string nm = archive_entry_pathname(e);
        if (archive_entry_filetype(e) != AE_IFREG) continue;
        ssize_t r;
        while ((r = archive_read_data(a, ioBuf.data(), ioBuf.size())) > 0) { if (cancelReq()) break; }
        if (r < 0) { logf_("驗證失敗: %s", archive_error_string(a) ? archive_error_string(a) : "資料損壞"); ok = false; break; }
        if (nm != "Save_WebDAV_folders.txt") {
            files++;
            if (expectFiles > 0) { int p = pgBase + (int)((long long)files * pgSpan / expectFiles); if (p > pgPct) setPct(p); }
        }
    }
    if (ok && hr != ARCHIVE_EOF) { logf_("驗證失敗: 讀取中斷"); ok = false; }
    if (ok && files != expectFiles) { logf_("驗證失敗: 檔案數不符 (%d / %d)", files, expectFiles); ok = false; }
    archive_read_free(a);
    return ok;
}

static bool packAll() {
    mkdirs(string(CFG_DIR));
    setenv("TMPDIR", CFG_DIR, 1);
    cleanTmp(); packErr.clear();
    struct archive* a = archive_write_new();
    archive_write_set_format_7zip(a);
    archive_write_set_format_option(a, "7zip", "compression", "deflate");  // 省記憶體
    if (archive_write_open_filename(a, TMP_FILE) != ARCHIVE_OK) {
        logf_("無法建立暫存檔\n"); archive_write_free(a); return false;
    }
    {   // 把「共享資料夾清單」一起放進 7z，還原時自動加入
        string man = "#files=" + to_string(totalFiles) + "\n";
        for (auto& d0 : cfg.dirs) man += normDir(d0) + "\n";
        struct archive_entry* me = archive_entry_new();
        archive_entry_set_pathname(me, "Save_WebDAV_folders.txt");
        archive_entry_set_size(me, man.size());
        archive_entry_set_filetype(me, AE_IFREG);
        archive_entry_set_perm(me, 0644);
        archive_entry_set_mtime(me, time(NULL), 0);
        if (archive_write_header(a, me) < ARCHIVE_WARN) packErr = archive_error_string(a) ? archive_error_string(a) : "manifest";
        else archive_write_data(a, man.data(), man.size());
        archive_entry_free(me);
    }
    packedCount = 0;
    for (size_t i = 0; i < cfg.dirs.size(); i++) {
        string d = normDir(cfg.dirs[i]);
        walk(a, d);
    }
    if (archive_write_close(a) < ARCHIVE_WARN && packErr.empty())
        packErr = archive_error_string(a) ? archive_error_string(a) : "close";
    archive_write_free(a);
    cleanTmp();
    if (!packErr.empty()) { logf_("打包失敗: %s", packErr.c_str()); remove(TMP_FILE); return false; }
    if (packedCount == 0) { logf_("資料夾內沒有檔案，已取消"); remove(TMP_FILE); return false; }
    struct stat st;
    if (stat(TMP_FILE, &st) != 0 || st.st_size <= 0) {
        logf_("7z 檔案是空的，已取消"); remove(TMP_FILE); return false;
    }
    return true;
}

static bool safeName(const string& n) {
    if (n.empty() || n[0] == '/') return false;
    if (n.find(':') != string::npos) return false;
    size_t i = 0;
    while (i <= n.size()) {
        size_t e = n.find('/', i);
        if (e == string::npos) e = n.size();
        if (n.substr(i, e - i) == "..") return false;
        i = e + 1;
    }
    return true;
}

static int skippedCount = 0, failCount = 0;
static bool isUnder(const string& path, const string& dir) {
    return path == dir || path.rfind(dir + "/", 0) == 0;
}

static bool unpackAll(const char* file) {
    struct archive* a = archive_read_new();
    archive_read_support_format_7zip(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, file, 1 << 16) != ARCHIVE_OK) {
        logf_("無法開啟 7z: %s", archive_error_string(a)); archive_read_free(a); return false;
    }
    struct archive_entry* e;
    int n = 0, hr;
    archTotal = 0; restoredCount = 0; skippedCount = 0; failCount = 0;
    struct stat fst; long long fsz = (stat(file, &fst) == 0) ? (long long)fst.st_size : 1;
    long long b0 = -1;
    bool cancelled = false;
    while ((hr = archive_read_next_header(a, &e)) == ARCHIVE_OK || hr == ARCHIVE_WARN) {
        if (cancelReq()) { cancelled = true; break; }
        if (b0 < 0) b0 = (long long)archive_filter_bytes(a, 0);
        string name = archive_entry_pathname(e);
        if (!safeName(name)) { logf_("略過不安全路徑: %s", name.c_str()); skippedCount++; continue; }
        if (name == "Save_WebDAV_folders.txt") {   // 清單檔: 不寫到 SD 卡，直接加入共享資料夾
            string content; ssize_t r2;
            while ((r2 = archive_read_data(a, ioBuf.data(), ioBuf.size())) > 0) content.append(ioBuf.data(), r2);
            size_t i2 = 0; int added = 0;
            while (i2 < content.size()) {
                size_t e2 = content.find('\n', i2); if (e2 == string::npos) e2 = content.size();
                string ln = content.substr(i2, e2 - i2); i2 = e2 + 1;
                while (!ln.empty() && ln.back() == '\r') ln.pop_back();
                if (ln.empty()) continue;
                if (ln[0] == '#') { if (ln.rfind("#files=", 0) == 0) archTotal = atoi(ln.c_str() + 7); continue; }
                ln = normDir(ln);
                if (cfg.dirs.size() < 20 && find(cfg.dirs.begin(), cfg.dirs.end(), ln) == cfg.dirs.end()) { cfg.dirs.push_back(ln); added++; }
            }
            if (added) { saveCfg(); logf_("已自動加入 %d 個共享資料夾", added); }
            continue;
        }
        string target = "sdmc:/" + name;
        if (!cfg.dirs.empty()) {   // 只允許寫回共享資料夾內，避免覆蓋到其他地方
            bool allowed = false;
            for (auto& d : cfg.dirs) if (isUnder(target, normDir(d))) { allowed = true; break; }
            if (!allowed) { skippedCount++; continue; }
        }
        if (archive_entry_filetype(e) == AE_IFDIR) { mkdirs(target + "/"); mkdir(target.c_str(), 0777); continue; }
        mkdirs(target);
        FILE* f = fopen(target.c_str(), "wb");
        if (!f) { failCount++; logf_("無法寫入: %s", target.c_str()); continue; }
        ssize_t r; bool bad = false;
        while ((r = archive_read_data(a, ioBuf.data(), ioBuf.size())) > 0) {
            if (fwrite(ioBuf.data(), 1, r, f) != (size_t)r) { bad = true; break; }   // 寫入失敗(多半是空間不足)
            if (archTotal <= 0) {   // 舊備份沒有檔案數: 改用已讀取的資料量估算
                long long denom = max(1LL, fsz - b0);
                int p = pgBase + (int)(max(0LL, (long long)archive_filter_bytes(a, 0) - b0) * pgSpan / denom);
                if (p > pgPct) setPct(p);
            }
            if (cancelReq()) { cancelled = true; break; }
        }
        if (!bad && !cancelled && r < 0) { bad = true; logf_("解壓錯誤: %s", archive_error_string(a) ? archive_error_string(a) : "資料損壞"); }
        fclose(f);
        if (cancelled) break;
        if (bad) { failCount++; logf_("寫入失敗: %s", target.c_str()); continue; }
        n++; restoredCount = n;
        if (archTotal > 0) { int p = pgBase + (int)((long long)n * pgSpan / archTotal); if (p > pgPct) setPct(p); }
    }
    bool readErr = (!cancelled && hr != ARCHIVE_EOF);
    if (readErr) logf_("7z 讀取中斷: %s", archive_error_string(a) ? archive_error_string(a) : "檔案可能損壞");
    archive_read_free(a);
    if (cancelled) { logf_("已取消，部分檔案可能已經還原"); return false; }
    if (skippedCount > 0) logf_("已略過 %d 個不在共享資料夾內的檔案", skippedCount);
    if (failCount > 0) { logf_("有 %d 個檔案還原失敗", failCount); return false; }
    if (readErr) return false;
    if (n == 0) { logf_("這份備份裡沒有可還原的檔案"); return false; }
    return true;
}

// ================= 畫面 (SDL2, 觸控 + 手把) =================
static SDL_Window* win;
static SDL_Renderer* ren;
static PlFontData fdata;
static map<int, TTF_Font*> fonts;
struct Col { Uint8 r, g, b; };
static const Col C_BG = {16, 20, 30}, C_BAR = {22, 28, 42}, C_CARD = {30, 37, 54}, C_SEL = {38, 92, 170},
                 C_WHITE = {240, 244, 250}, C_GRAY = {150, 160, 182}, C_ACC = {64, 196, 160},
                 C_BTN = {28, 76, 78}, C_DARK = {10, 24, 24}, C_MARK = {170, 182, 205};

static TTF_Font* F(int sz) {
    auto it = fonts.find(sz);
    if (it != fonts.end()) return it->second;
    TTF_Font* f = TTF_OpenFontRW(SDL_RWFromMem(fdata.address, fdata.size), 0, sz);
    fonts[sz] = f;
    return f;
}
static map<string, pair<SDL_Texture*, pair<int, int>>> tcache;

static string fitStr(TTF_Font* f, string s, int maxw) {
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s.c_str(), &w, &h);
    if (w <= maxw) return s;
    while (!s.empty()) {
        while (!s.empty()) { unsigned char ch = s.back(); s.pop_back(); if ((ch & 0xC0) != 0x80) break; }
        string t = s + "...";
        TTF_SizeUTF8(f, t.c_str(), &w, &h);
        if (w <= maxw) return t;
    }
    return "";
}
// align: 0 左, 1 置中(x=中心), 2 靠右(x=右緣)
static int txt(string s, int sz, int x, int y, Col c, int al = 0, int maxw = 0) {
    if (s.empty()) return 0;
    TTF_Font* f = F(sz);
    if (!f) return 0;
    if (maxw) s = fitStr(f, s, maxw);
    if (s.empty()) return 0;
    char key[32]; snprintf(key, sizeof key, "%d|%02x%02x%02x|", sz, c.r, c.g, c.b);
    string k = string(key) + s;
    auto it = tcache.find(k);
    if (it == tcache.end()) {
        if (tcache.size() > 400) { for (auto& p : tcache) SDL_DestroyTexture(p.second.first); tcache.clear(); }
        SDL_Surface* sf = TTF_RenderUTF8_Blended(f, s.c_str(), SDL_Color{c.r, c.g, c.b, 255});
        if (!sf) return 0;
        SDL_Texture* t = SDL_CreateTextureFromSurface(ren, sf);
        it = tcache.emplace(k, make_pair(t, make_pair(sf->w, sf->h))).first;
        SDL_FreeSurface(sf);
    }
    int w = it->second.second.first, h = it->second.second.second;
    int dx = al == 1 ? x - w / 2 : al == 2 ? x - w : x;
    SDL_Rect r{dx, y, w, h};
    SDL_RenderCopy(ren, it->second.first, NULL, &r);
    return w;
}
static void rect(int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}
// 上下橫條 + 右下角署名
static void chrome(const string& title, const string& hint, bool back) {
    rect(0, 0, 1280, 86, C_BAR); rect(0, 84, 1280, 2, C_ACC);
    txt(title, 36, 48, 18, C_WHITE);
    if (back) { rect(1080, 18, 152, 50, C_CARD); txt("B  返回", 26, 1156, 26, C_WHITE, 1); }
    rect(0, 650, 1280, 70, C_BAR);
    txt(hint, 22, 48, 678, C_GRAY, 0, 900);
    txt("ted5789", 24, 1236, 678, C_MARK, 2);
}

// kind: 0 一般(左名稱/右數值) 1 大按鈕 2 資料夾(名稱+路徑) 3 小標題 4 訊息文字
struct Row { string a, b, c; int kind; bool ok; int h; };
static Row R(int kind, const string& a, const string& b = "", const string& c = "") {
    Row r; r.a = a; r.b = b; r.c = c; r.kind = kind;
    r.ok = (kind != 3 && kind != 4);
    r.h = kind == 1 ? 92 : kind == 2 ? 92 : kind == 3 ? 44 : kind == 4 ? 52 : 72;
    return r;
}

// 回傳: 被選的列 / -1 = B 或點返回 / -9 = +
static int menu(const string& title, const string& hint, vector<Row>& rows, int& sel, bool back) {
    const int VT = 98, VB = 642, VH = VB - VT;
    if (sel < 0 || sel >= (int)rows.size() || !rows[sel].ok) {
        sel = 0; while (sel < (int)rows.size() && !rows[sel].ok) sel++;
        if (sel >= (int)rows.size()) sel = 0;
    }
    vector<int> ys; int total = 0;
    for (auto& r : rows) { ys.push_back(total); total += r.h + 10; }
    int maxS = max(0, total - VH);
    float scroll = 0, target = 0;
    auto ensure = [&]() {   // 自動定位: 讓選取列一定在畫面內
        float top = ys[sel] - 10, bot = ys[sel] + rows[sel].h + 10;
        if (top < target) target = top; else if (bot > target + VH) target = bot - VH;
        target = max(0.f, min((float)maxS, target));
    };
    ensure(); scroll = target;
    bool touching = false, moved = false; int sx = 0, sy = 0, lx = 0, ly = 0, hold = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 dn = padGetButtonsDown(&pad), hd = padGetButtons(&pad);
        if (dn & HidNpadButton_Plus) return -9;
        if (dn & HidNpadButton_B) return -1;
        if ((dn & HidNpadButton_A) && rows[sel].ok) return sel;
        int mv = 0;
        if (dn & HidNpadButton_AnyUp) { mv = -1; hold = 0; }
        else if (dn & HidNpadButton_AnyDown) { mv = 1; hold = 0; }
        else if (hd & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) {
            if (++hold > 22 && hold % 5 == 0) mv = (hd & HidNpadButton_AnyUp) ? -1 : 1;
        } else hold = 0;
        if (mv) {
            int n = sel + mv;
            while (n >= 0 && n < (int)rows.size() && !rows[n].ok) n += mv;
            if (n >= 0 && n < (int)rows.size()) { sel = n; ensure(); }
        }
        HidTouchScreenState ts = {0};
        hidGetTouchScreenStates(&ts, 1);
        if (ts.count > 0) {
            int tx = ts.touches[0].x, ty = ts.touches[0].y;
            if (!touching) { touching = true; moved = false; sx = tx; sy = ty; }
            else {
                if (abs(tx - sx) > 14 || abs(ty - sy) > 14) moved = true;
                if (moved) { scroll -= (ty - ly); scroll = max(0.f, min((float)maxS, scroll)); target = scroll; }
            }
            lx = tx; ly = ty;
        } else if (touching) {
            touching = false;
            if (!moved) {
                if (back && ly < 86 && lx > 1060) return -1;
                if (ly >= VT && ly < VB)
                    for (size_t i = 0; i < rows.size(); i++) {
                        float y = VT + ys[i] - scroll;
                        if (ly >= y && ly < y + rows[i].h && rows[i].ok) { sel = (int)i; return (int)i; }
                    }
            }
        }
        scroll += (target - scroll) * 0.3f;
        if (fabsf(target - scroll) < 0.5f) scroll = target;

        rect(0, 0, 1280, 720, C_BG);
        for (size_t i = 0; i < rows.size(); i++) {
            const Row& r = rows[i];
            int y = (int)(VT + ys[i] - scroll);
            if (y + r.h < VT - 10 || y > VB) continue;
            bool s = ((int)i == sel);
            if (r.kind == 3) { txt(r.a, 22, 52, y + 10, C_GRAY, 0, 1100); continue; }
            if (r.kind == 4) { txt(r.a, 28, 52, y + 6, C_WHITE, 0, 1170); continue; }
            if (s) rect(36, y - 4, 1208, r.h + 8, C_ACC);
            Col bg = r.kind == 1 ? (s ? C_ACC : C_BTN) : (s ? C_SEL : C_CARD);
            rect(40, y, 1200, r.h, bg);
            if (r.kind == 1) txt(r.a, 36, 640, y + (r.h - 50) / 2, s ? C_DARK : C_WHITE, 1);
            else if (r.kind == 2) {
                txt(r.a, 30, 64, y + 8, C_WHITE, 0, 900);
                txt(r.b, 20, 64, y + r.h - 32, s ? C_WHITE : C_GRAY, 0, 1100);
                if (!r.c.empty()) txt(r.c, 22, 1216, y + r.h / 2 - 16, C_GRAY, 2);
            } else {
                txt(r.a, 28, 64, y + (r.h - 40) / 2, C_WHITE, 0, r.c.empty() ? 1100 : 520);
                if (!r.c.empty()) txt(r.c, 26, 1216, y + (r.h - 38) / 2, s ? C_WHITE : C_GRAY, 2, 620);
            }
        }
        chrome(title, hint, back);
        SDL_RenderPresent(ren);
    }
    return -9;
}

static bool confirm(const string& msg, const string& yes) {
    vector<Row> r;
    size_t i = 0;
    while (i <= msg.size()) {
        size_t e = msg.find('\n', i); if (e == string::npos) e = msg.size();
        r.push_back(R(4, msg.substr(i, e - i))); i = e + 1;
    }
    r.push_back(R(1, yes)); int yi = (int)r.size() - 1;
    r.push_back(R(0, "取消")); int sel = (int)r.size() - 1;   // 預設停在「取消」比較安全
    return menu("請確認", "A 確定　B 取消　可觸控", r, sel, true) == yi;
}

// ---------- 進度記錄畫面 ----------
static deque<string> logs;
static string logHint = "處理中，請勿關閉程式　按住 B 可取消";
static const Col C_ERR = {232, 126, 92};

static void pgStart(const string& title, const string& stage) {
    gCancel = false; pgActive = true; pgOk = false; pgErr = false; pgPct = 0; pgBase = 0; pgSpan = 100;
    pgTitle = title; pgStage = stage; dbg("%s", pgStage.c_str()); pgList.clear(); logs.clear();
}
static void drawLog() {
    rect(0, 0, 1280, 720, C_BG);
    if (!pgActive) {
        size_t st = logs.size() > 14 ? logs.size() - 14 : 0;
        for (size_t i = st; i < logs.size(); i++) txt(logs[i], 24, 48, 108 + (int)(i - st) * 36, C_WHITE, 0, 1180);
        chrome("Save_WebDAV", logHint, false);
    } else {
        txt(pgStage, 30, 48, 98, pgOk ? C_ACC : (pgErr ? C_ERR : C_WHITE), 0, 1180);
        int ly0 = 150; size_t maxL = 11;
        if (!pgList.empty()) {   // 打包的資料夾總目錄 (兩欄, 最多 20 個)
            for (size_t i = 0; i < pgList.size() && i < 20; i++)
                txt(pgList[i], 22, 48 + (int)(i / 10) * 600, 150 + (int)(i % 10) * 31, C_MARK, 0, 570);
            ly0 = 476; maxL = 3;
        }
        size_t st = logs.size() > maxL ? logs.size() - maxL : 0;
        for (size_t i = st; i < logs.size(); i++) txt(logs[i], 22, 48, ly0 + (int)(i - st) * 30, C_GRAY, 0, 1180);
        rect(48, 580, 1184, 38, C_CARD);
        rect(48, 580, 1184 * pgPct / 100, 38, pgErr ? C_ERR : C_ACC);
        txt(to_string(pgPct) + "%", 26, 640, 584, pgPct >= 50 ? C_DARK : C_WHITE, 1);
        chrome(pgTitle, logHint, false);
    }
    SDL_RenderPresent(ren);
}
static void setPct(int p) {   // 進行中最多顯示 99%，100% 只在全部完成時才出現
    if (p > 99) p = 99;
    if (p <= pgPct) return;
    pgPct = p;
    drawLog();
}
static void logf_(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    dbg("%s", buf);
    string s = buf; bool repl = false;
    if (!s.empty() && s[0] == '\r') { repl = true; s.erase(0, 1); }
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find('\n', i); if (e == string::npos) e = s.size();
        string line = s.substr(i, e - i); i = e + 1;
        if (line.empty()) continue;
        if (repl && !logs.empty()) { logs.back() = line; repl = false; } else logs.push_back(line);
    }
    while (logs.size() > 200) logs.pop_front();
    drawLog();
}
static void waitDone() {
    if (pgActive && !pgOk) { pgErr = true; pgStage = "未完成，請看下方訊息"; dbg("%s", pgStage.c_str()); }
    logHint = "按 A / B 或點一下螢幕返回";
    bool touching = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) break;
        HidTouchScreenState ts = {0};
        hidGetTouchScreenStates(&ts, 1);
        if (ts.count > 0) touching = true; else if (touching) break;
        drawLog();
    }
    logHint = "處理中，請勿關閉程式　按住 B 可取消";
    pgActive = false; pgOk = false; pgErr = false;
}
static void finishOK(const string& msg) {   // 100% 與完成訊息同時出現
    pgPct = 100; pgStage = msg; dbg("%s", pgStage.c_str()); pgOk = true;
    drawLog();
    waitDone();
}

// ---------- 功能 ----------
static string cleanName(string s) {
    for (auto& ch : s) if (strchr("/\\:*?\"<>| %#\t", ch)) ch = '_';
    return s;
}

// 檔名用的時間: 依 Switch 設定的時區 (台灣 = UTC+8)
static string nowStamp() {
    char buf[32];
    u64 ts = 0;
    TimeCalendarTime ct; TimeCalendarAdditionalInfo ai;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &ts)) && R_SUCCEEDED(timeToCalendarTimeWithMyRule(ts, &ct, &ai))) {
        snprintf(buf, sizeof buf, "%04u%02u%02u_%02u%02u%02u", (unsigned)ct.year, (unsigned)ct.month, (unsigned)ct.day,
                 (unsigned)ct.hour, (unsigned)ct.minute, (unsigned)ct.second);
    } else {
        time_t t = time(NULL); struct tm* lt = localtime(&t);
        strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", lt);
    }
    return buf;
}

static long long sdFree() {   // SD 卡剩餘空間 (bytes)，-1 = 取不到
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    s64 f = 0;
    if (!fs || R_FAILED(fsFsGetFreeSpace(fs, "/", &f))) return -1;
    return (long long)f;
}

// 還原前先把「目前的共享資料夾」備份成一份保險檔 (只留最近一份)
static bool backupBeforeRestore() {
    totalFiles = 0; totalBytes = 0;
    for (auto& d : cfg.dirs) totalFiles += countFiles(normDir(d));
    if (totalFiles == 0) return true;   // 目前沒有檔案，不需要備份
    long long fb = sdFree();
    if (fb >= 0 && fb < totalBytes + (64LL << 20)) { logf_("SD 卡空間不足，無法備份目前存檔"); return false; }
    if (!packAll()) return false;
    remove(BEFORE_FILE);
    if (rename(TMP_FILE, BEFORE_FILE) != 0) { logf_("無法儲存備份檔"); remove(TMP_FILE); return false; }
    return true;
}

static void doTest() {
    gCancel = false; pgActive = false; logs.clear();
    logf_("測試連線: %s", cfg.url.c_str());
    if (cfg.url.empty()) { logf_("失敗: 還沒設定 WebDAV 位址"); waitDone(); return; }
    bool ok = davExists();
    if (!ok && (lastHttp == 404 || lastHttp == 409)) {
        logf_("資料夾不存在，嘗試自動建立...");
        ok = davEnsureDir();
        if (ok) logf_("已自動建立資料夾");
    }
    if (!ok) { logf_("失敗: %s", describeErr().c_str()); waitDone(); return; }
    logf_("1) 連線與帳號密碼: 成功");
    mkdirs(string(CFG_DIR));
    FILE* f = fopen(TEST_FILE, "wb");
    if (f) { fputs("ok", f); fclose(f); }
    bool w = davPut(".save_webdav_test.tmp", TEST_FILE);
    if (w) davDel(".save_webdav_test.tmp");
    remove(TEST_FILE);
    if (!w) { logf_("失敗: 沒有寫入權限 (%s)", describeErr().c_str()); waitDone(); return; }
    logf_("2) 寫入與刪除權限: 成功");
    logf_("全部正常，可以使用!");
    waitDone();
}

static void doUpload() {
    gCancel = false;
    pgStart("存檔上傳", "準備中...");
    if (cfg.url.empty()) { logf_("請先設定 WebDAV 位址"); waitDone(); return; }
    if (cfg.dirs.empty()) { logf_("請先新增至少一個共享資料夾"); waitDone(); return; }
    string user = cfg.name.empty() ? "Switch" : cfg.name;
    for (size_t i = 0; i < cfg.dirs.size(); i++) pgList.push_back(to_string(i + 1) + "  " + normDir(cfg.dirs[i]));
    pgStage = "1/3  開始打包 (掃描檔案中...)"; dbg("%s", pgStage.c_str()); drawLog();
    totalFiles = 0; totalBytes = 0;
    for (auto& d : cfg.dirs) totalFiles += countFiles(normDir(d));
    long long fb = sdFree(), need = totalBytes + (64LL << 20);
    if (fb >= 0 && fb < need) {
        char m[200];
        snprintf(m, sizeof m, "SD 卡剩餘空間可能不夠\n剩餘約 %lld MB，預估需要約 %lld MB", fb >> 20, need >> 20);
        if (!confirm(m, "仍要繼續")) { pgActive = false; return; }
    }
    pgStage = "1/3  打包中  共 " + to_string(totalFiles) + " 個檔案"; dbg("%s", pgStage.c_str()); pgBase = 0; pgSpan = 40; drawLog();
    if (!packAll()) { waitDone(); return; }

    pgBase = 40; pgSpan = 10; setPct(40);
    pgStage = "2/3  驗證 7z 檔案完整性..."; dbg("%s", pgStage.c_str()); drawLog();
    if (!verify7zFull(TMP_FILE, packedCount)) {
        remove(TMP_FILE);
        logf_("驗證沒通過，已取消上傳 (遠端的舊版本不受影響)");
        waitDone(); return;
    }
    struct stat st; long long sz = 0;
    if (stat(TMP_FILE, &st) == 0) sz = (long long)st.st_size;
    string nm = "Save_" + user + "_" + nowStamp() + ".7z", tmpn = nm + ".tmp";
    pgBase = 50; pgSpan = 49; setPct(50);
    pgStage = "3/3  上傳中  " + nm + "  (" + to_string(packedCount) + " 個檔案, " + to_string(sz / 1024) + " KB)"; dbg("%s", pgStage.c_str());
    drawLog();
    bool ok = davPut(tmpn, TMP_FILE);   // 先傳成暫存檔名，傳完確認後才改成正式檔名
    if (!ok && (lastHttp == 404 || lastHttp == 409)) {
        logf_("WebDAV 資料夾不存在，嘗試自動建立...");
        if (davEnsureDir()) ok = davPut(tmpn, TMP_FILE);
    }
    if (!ok) { logf_("上傳失敗: %s", describeErr().c_str()); davDel(tmpn); remove(TMP_FILE); waitDone(); return; }
    long long rs = davSize(tmpn);
    if (rs >= 0 && rs != sz) {
        logf_("上傳後大小不符 (本機 %lld / 伺服器 %lld)，已取消", sz, rs);
        davDel(tmpn); remove(TMP_FILE); waitDone(); return;
    }
    if (!davMove(tmpn, nm)) {
        logf_("伺服器不支援改名，改為直接上傳");
        davDel(tmpn);
        if (!davPut(nm, TMP_FILE)) { logf_("上傳失敗: %s", describeErr().c_str()); davDel(nm); remove(TMP_FILE); waitDone(); return; }
    }
    remove(TMP_FILE);

    pgStage = "3/3  整理舊版本中..."; dbg("%s", pgStage.c_str()); drawLog();
    string prefix = "Save_" + user + "_";
    vector<string> names;
    if (davList(names, prefix)) {
        names.erase(remove_if(names.begin(), names.end(), [&](const string& n) { return n.size() != prefix.size() + 18; }), names.end());
        for (size_t i = KEEP; i < names.size(); i++)   // 只留最新 5 份，第 6 份起刪除
            logf_("刪除舊版本: %s %s", names[i].c_str(), davDel(names[i]) ? "OK" : "失敗");
    }
    finishOK("上傳完成!  " + nm);
}

static void doDownload() {
    gCancel = false; pgActive = false; logs.clear();
    if (cfg.url.empty()) { logf_("請先設定 WebDAV 位址"); waitDone(); return; }
    logf_("讀取遠端列表...");
    vector<string> names;
    if (!davList(names, "Save_")) { waitDone(); return; }
    if (names.empty()) { logf_("遠端沒有 Save_*.7z"); waitDone(); return; }
    vector<Row> rows; for (auto& n : names) rows.push_back(R(0, n));
    int sel = 0;
    int r = menu("選擇要還原的版本", "A 選擇　B 返回　可觸控", rows, sel, true);
    if (r < 0) return;
    if (!confirm("將還原:\n" + names[r] + "\n這會覆蓋 SD 卡上相同路徑的檔案!\n(還原前會先自動備份目前的存檔)", "確定還原")) return;

    pgStart("存檔下載覆蓋", "準備中...");
    bool hasBackup = false;
    if (!cfg.dirs.empty()) {
        pgStage = "1/3  備份目前存檔 (保險用)"; dbg("%s", pgStage.c_str()); pgBase = 0; pgSpan = 25; drawLog();
        hasBackup = backupBeforeRestore();
        if (!hasBackup) {
            if (gCancel) { logf_("已取消"); waitDone(); return; }
            if (!confirm("備份目前存檔失敗\n仍要繼續還原嗎?", "仍要還原")) { pgActive = false; return; }
            gCancel = false;
        }
    }
    pgBase = 25; pgSpan = 25; setPct(25);
    pgStage = "2/3  下載中  " + names[r]; dbg("%s", pgStage.c_str()); drawLog();
    mkdirs(string(CFG_DIR));
    long long rsz = davSize(names[r]), fb = sdFree();
    if (rsz >= 0 && fb >= 0 && fb < rsz * 3) {
        char m[200];
        snprintf(m, sizeof m, "SD 卡剩餘空間可能不夠\n剩餘約 %lld MB，預估需要約 %lld MB", fb >> 20, (rsz * 3) >> 20);
        if (!confirm(m, "仍要繼續")) { pgActive = false; return; }
    }
    if (!davGet(names[r], TMP_FILE)) { logf_("下載失敗: %s", describeErr().c_str()); remove(TMP_FILE); waitDone(); return; }
    struct stat st;
    if (rsz >= 0 && stat(TMP_FILE, &st) == 0 && (long long)st.st_size != rsz) {
        logf_("下載不完整 (%lld / %lld)", (long long)st.st_size, rsz);
        remove(TMP_FILE); waitDone(); return;
    }
    pgBase = 50; pgSpan = 49; setPct(50);
    pgStage = "3/3  解壓還原中，請勿關閉程式"; dbg("%s", pgStage.c_str()); drawLog();
    bool ok = unpackAll(TMP_FILE);
    remove(TMP_FILE);
    if (hasBackup) logf_("還原前的存檔已備份在 SD 卡 config/Save_WebDAV/before_restore.7z");
    if (ok) finishOK("還原完成!  共 " + to_string(restoredCount) + " 個檔案");
    else waitDone();
}

// ---------- 資料夾選擇 ----------
static bool pickFolder(string& out) {
    string cur = "sdmc:/"; int sel = 0;
    while (true) {
        vector<string> subs;
        if (DIR* d = opendir(cur.c_str())) {
            struct dirent* e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                string f = cur + (cur.back() == '/' ? "" : "/") + e->d_name;
                struct stat st;
                if (stat(f.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) subs.push_back(e->d_name);
            }
            closedir(d);
        }
        sort(subs.begin(), subs.end());
        bool root = (cur == "sdmc:/");
        vector<Row> r;
        r.push_back(R(1, "選擇這個資料夾"));
        r.push_back(R(3, "目前位置: " + cur));
        if (!root) r.push_back(R(0, ".. 上一層"));
        int base = (int)r.size();
        for (auto& s : subs) r.push_back(R(0, s, "", "進入"));
        int i = menu("選擇共享資料夾", "A 進入/選擇　B 上一層　可觸控", r, sel, true);
        if (i == -9) return false;
        if (i == -1 || (!root && i == 2)) {
            if (root) return false;
            cur = cur.substr(0, cur.rfind('/'));
            if (cur == "sdmc:") cur = "sdmc:/";
            sel = 0; continue;
        }
        if (i == 0) { if (root) continue; out = cur; return true; }
        cur = cur + (cur.back() == '/' ? "" : "/") + subs[i - base];
        sel = 0;
    }
}

// 自動取得 Switch 使用者暱稱
static string getNickname() {
    string out;
    // 只在「前端模式」嘗試，而且只讀系統預先選好的使用者；
    // 不再彈出選擇使用者畫面，也不用其他可能出錯的呼叫。拿不到就留空，讓使用者自己填。
    if (appletGetAppletType() != AppletType_Application) { dbg("nickname: skip (not application mode)"); return out; }
    Result rc = accountInitialize(AccountServiceType_Application);
    dbg("accountInitialize rc=0x%x", (unsigned)rc);
    if (R_FAILED(rc)) return out;
    AccountUid uid = {};
    rc = accountGetPreselectedUser(&uid);
    dbg("accountGetPreselectedUser rc=0x%x", (unsigned)rc);
    if (R_SUCCEEDED(rc) && accountUidIsValid(&uid)) {
        AccountProfile prof;
        rc = accountGetProfile(&prof, uid);
        dbg("accountGetProfile rc=0x%x", (unsigned)rc);
        if (R_SUCCEEDED(rc)) {
            AccountProfileBase base = {};
            AccountUserData ud = {};
            if (R_SUCCEEDED(accountProfileGet(&prof, &ud, &base))) out = base.nickname;
            accountProfileClose(&prof);
        }
    }
    accountExit();
    return out;
}

// ---------- 密碼輸入 (只顯示最後輸入的那一個字，其餘變 *) ----------
static bool pwEdit(string& out) {
    static const char* lowR[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm.@_"};
    static const char* upR[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM.@_"};
    static const char* symR[4] = {"!@#$%^&*()", "-_=+[]{};:", "'\"\\|/<>?,.", "~`"};
    static const char* fn[6] = {"大小寫", "符號/字母", "空白", "刪除", "完成", "取消"};
    string val; bool reveal = false; int page = 0, cr = 1, cc = 0;
    bool touching = false, moved = false; int sx = 0, sy = 0, lx = 0, ly = 0;
    auto rows = [&]() -> const char** { return page == 0 ? lowR : page == 1 ? upR : symR; };
    auto rlen = [&](int r) { return r < 4 ? (int)strlen(rows()[r]) : 6; };
    auto geom = [&](int r, int c, int& x, int& y, int& w, int& h) {
        h = 70; y = 230 + r * 78;
        if (r < 4) { int n = rlen(r); w = 104; int tot = n * 104 + (n - 1) * 8; x = (1280 - tot) / 2 + c * 112; }
        else { w = 178; x = 84 + c * 186; }
    };
    auto press = [&](int r, int c) -> int {
        if (r < 4) { if ((int)val.size() < 64) { val += rows()[r][c]; reveal = true; } return 0; }
        switch (c) {
            case 0: page = (page == 0) ? 1 : 0; break;
            case 1: page = (page == 2) ? 0 : 2; break;
            case 2: val += ' '; reveal = true; break;
            case 3: if (!val.empty()) val.pop_back(); reveal = false; break;
            case 4: return 1;
            case 5: return 2;
        }
        return 0;
    };
    while (appletMainLoop()) {
        cc = min(cc, rlen(cr) - 1);
        padUpdate(&pad);
        u64 dn = padGetButtonsDown(&pad);
        int res = 0;
        if (dn & HidNpadButton_AnyUp) cr = (cr + 4) % 5;
        if (dn & HidNpadButton_AnyDown) cr = (cr + 1) % 5;
        cc = min(cc, rlen(cr) - 1);
        if (dn & HidNpadButton_AnyLeft) cc = (cc + rlen(cr) - 1) % rlen(cr);
        if (dn & HidNpadButton_AnyRight) cc = (cc + 1) % rlen(cr);
        if (dn & HidNpadButton_A) res = press(cr, cc);
        if (dn & HidNpadButton_B) res = press(4, 3);
        if (dn & HidNpadButton_X) page = (page == 0) ? 1 : 0;
        if (dn & HidNpadButton_Y) page = (page == 2) ? 0 : 2;
        if (dn & HidNpadButton_Plus) res = 1;
        if (dn & HidNpadButton_Minus) res = 2;
        HidTouchScreenState ts = {0};
        hidGetTouchScreenStates(&ts, 1);
        if (ts.count > 0) {
            int tx = ts.touches[0].x, ty = ts.touches[0].y;
            if (!touching) { touching = true; moved = false; sx = tx; sy = ty; }
            else if (abs(tx - sx) > 20 || abs(ty - sy) > 20) moved = true;
            lx = tx; ly = ty;
        } else if (touching) {
            touching = false;
            if (!moved) {
                bool hit = false;
                for (int r = 0; r < 5 && !hit; r++)
                    for (int c = 0; c < rlen(r) && !hit; c++) {
                        int x, y, w, h; geom(r, c, x, y, w, h);
                        if (lx >= x && lx < x + w && ly >= y && ly < y + h) { cr = r; cc = c; res = press(r, c); hit = true; }
                    }
            }
        }
        if (res == 1) { out = val; return true; }
        if (res == 2) return false;

        rect(0, 0, 1280, 720, C_BG);
        txt("新密碼 (只顯示最後輸入的字，取消則保留舊密碼)", 20, 84, 96, C_GRAY);
        rect(84, 128, 1112, 76, C_CARD);
        string shown;
        for (size_t i = 0; i < val.size(); i++) shown += (i + 1 == val.size() && reveal) ? string(1, val[i]) : string("*");
        txt(shown, 40, 104, 142, C_WHITE, 0, 1070);
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < rlen(r); c++) {
                int x, y, w, h; geom(r, c, x, y, w, h);
                bool sl = (r == cr && c == cc);
                if (sl) rect(x - 3, y - 3, w + 6, h + 6, C_ACC);
                rect(x, y, w, h, sl ? C_SEL : C_CARD);
                string lb = r < 4 ? string(1, rows()[r][c]) : string(fn[c]);
                txt(lb, r < 4 ? 34 : 26, x + w / 2, y + (h - (r < 4 ? 48 : 38)) / 2, C_WHITE, 1);
            }
        chrome("輸入密碼", "A 輸入　B 刪除　X 大小寫　Y 符號　+ 完成　- 取消", false);
        SDL_RenderPresent(ren);
    }
    return false;
}

static string baseName(const string& p) {
    size_t i = p.rfind('/');
    return (i == string::npos || i + 1 >= p.size()) ? p : p.substr(i + 1);
}

// ---------- 主畫面: 一頁雙欄 (左: 功能+設定 / 右: 共享資料夾) ----------
static void drawRowAt(const Row& r, int x, int y, int w, bool s) {
    if (r.kind == 3) { txt(r.a, 20, x + 12, y + 6, C_GRAY, 0, w - 24); return; }
    if (s) rect(x - 3, y - 3, w + 6, r.h + 6, C_ACC);
    Col bg = r.kind == 1 ? (s ? C_ACC : C_BTN) : (s ? C_SEL : C_CARD);
    rect(x, y, w, r.h, bg);
    if (r.kind == 1) txt(r.a, 32, x + w / 2, y + (r.h - 44) / 2, s ? C_DARK : C_WHITE, 1, w - 20);
    else if (r.kind == 5) {   // 資料夾: 名稱在上，位置在下，都靠左
        txt(r.a, 28, x + 20, y + 8, C_WHITE, 0, w - 40);
        txt(r.b, 20, x + 20, y + r.h - 32, s ? C_WHITE : C_GRAY, 0, w - 40);
    } else {
        txt(r.a, 26, x + 20, y + (r.h - 36) / 2, C_WHITE, 0, w * 40 / 100);
        txt(r.c, 24, x + w - 20, y + (r.h - 34) / 2, s ? C_WHITE : C_GRAY, 2, w * 55 / 100);
    }
}

// 回傳 欄*1000+列 / -1 = B / -9 = +
static int mainMenu(vector<Row>& L, vector<Row>& Rr, int& col, int& sl, int& sr) {
    const int VT = 98, VB = 642, VH = VB - VT;
    vector<Row>* RW[2] = {&L, &Rr};
    int* SEL[2] = {&sl, &sr};
    const int PX[2] = {24, 568}, PW[2] = {516, 688};
    vector<int> ys[2]; int maxS[2]; float scroll[2] = {0, 0}, target[2] = {0, 0};
    for (int p = 0; p < 2; p++) {
        auto& rows = *RW[p]; int total = 0;
        for (auto& r : rows) { ys[p].push_back(total); total += r.h + 8; }
        maxS[p] = max(0, total - VH);
        int& sv = *SEL[p];
        if (sv < 0 || sv >= (int)rows.size() || !rows[sv].ok) {
            sv = 0; while (sv < (int)rows.size() && !rows[sv].ok) sv++;
            if (sv >= (int)rows.size()) sv = 0;
        }
    }
    auto ensure = [&](int p) {   // 自動定位
        auto& rows = *RW[p]; int sv = *SEL[p];
        float top = ys[p][sv] - 8, bot = ys[p][sv] + rows[sv].h + 8;
        if (top < target[p]) target[p] = top; else if (bot > target[p] + VH) target[p] = bot - VH;
        target[p] = max(0.f, min((float)maxS[p], target[p]));
    };
    ensure(0); ensure(1);
    scroll[0] = target[0]; scroll[1] = target[1];
    bool touching = false, moved = false; int sx = 0, sy = 0, lx = 0, ly = 0, tp = 0, hold = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 dn = padGetButtonsDown(&pad), hd = padGetButtons(&pad);
        if (dn & HidNpadButton_Plus) return -9;
        if (dn & HidNpadButton_B) return -1;
        if ((dn & HidNpadButton_A) && (*RW[col])[*SEL[col]].ok) return col * 1000 + *SEL[col];
        if (dn & HidNpadButton_AnyLeft) { col = 0; ensure(0); }
        if (dn & HidNpadButton_AnyRight) { col = 1; ensure(1); }
        int mv = 0;
        if (dn & HidNpadButton_AnyUp) { mv = -1; hold = 0; }
        else if (dn & HidNpadButton_AnyDown) { mv = 1; hold = 0; }
        else if (hd & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) {
            if (++hold > 22 && hold % 5 == 0) mv = (hd & HidNpadButton_AnyUp) ? -1 : 1;
        } else hold = 0;
        if (mv) {
            auto& rows = *RW[col];
            int n = *SEL[col] + mv;
            while (n >= 0 && n < (int)rows.size() && !rows[n].ok) n += mv;
            if (n >= 0 && n < (int)rows.size()) { *SEL[col] = n; ensure(col); }
        }
        HidTouchScreenState ts = {0};
        hidGetTouchScreenStates(&ts, 1);
        if (ts.count > 0) {
            int tx = ts.touches[0].x, ty = ts.touches[0].y;
            if (!touching) { touching = true; moved = false; sx = tx; sy = ty; tp = (tx < 556) ? 0 : 1; }
            else {
                if (abs(tx - sx) > 14 || abs(ty - sy) > 14) moved = true;
                if (moved) { scroll[tp] -= (ty - ly); scroll[tp] = max(0.f, min((float)maxS[tp], scroll[tp])); target[tp] = scroll[tp]; }
            }
            lx = tx; ly = ty;
        } else if (touching) {
            touching = false;
            if (!moved && ly >= VT && ly < VB) {
                for (int p = 0; p < 2; p++) {
                    if (lx < PX[p] || lx >= PX[p] + PW[p]) continue;
                    auto& rows = *RW[p];
                    for (size_t i = 0; i < rows.size(); i++) {
                        float y = VT + ys[p][i] - scroll[p];
                        if (ly >= y && ly < y + rows[i].h && rows[i].ok) { col = p; *SEL[p] = (int)i; return p * 1000 + (int)i; }
                    }
                }
            }
        }
        for (int p = 0; p < 2; p++) {
            scroll[p] += (target[p] - scroll[p]) * 0.3f;
            if (fabsf(target[p] - scroll[p]) < 0.5f) scroll[p] = target[p];
        }
        rect(0, 0, 1280, 720, C_BG);
        for (int p = 0; p < 2; p++) {
            auto& rows = *RW[p];
            for (size_t i = 0; i < rows.size(); i++) {
                int y = (int)(VT + ys[p][i] - scroll[p]);
                if (y + rows[i].h < VT - 10 || y > VB) continue;
                drawRowAt(rows[i], PX[p], y, PW[p], col == p && (int)i == *SEL[p]);
            }
        }
        rect(553, VT, 2, VB - VT, C_BAR);
        chrome("Save_WebDAV", "上下 移動　左右 切換　A 選擇　也可直接觸控　+ 離開", false);
        SDL_RenderPresent(ren);
    }
    return -9;
}

static void mainScreen() {
    int col = 0, sl = 0, sr = 0;
    while (true) {
        vector<Row> L, Rr;
        auto H = [](Row r, int h) { r.h = h; return r; };
        L.push_back(H(R(1, "存檔上傳"), 76));
        L.push_back(H(R(1, "存檔下載覆蓋"), 76));
        L.push_back(H(R(3, "設定"), 36));
        L.push_back(H(R(0, "使用者名稱", "", cfg.name.empty() ? "(未設定)" : cfg.name), 64));
        L.push_back(H(R(0, "WebDAV 位址", "", cfg.url.empty() ? "(未設定)" : cfg.url), 64));
        L.push_back(H(R(0, "帳號", "", cfg.user.empty() ? "(未設定)" : cfg.user), 64));
        L.push_back(H(R(0, "密碼", "", cfg.pass.empty() ? "(未設定)" : "********"), 64));
        L.push_back(H(R(0, "測試連線", "", "確認設定正確"), 64));
        Rr.push_back(H(R(1, "+ 新增共享資料夾"), 76));
        Rr.push_back(H(R(3, "共享資料夾 (" + to_string(cfg.dirs.size()) + ")  選擇後自動記住，點一下可移除"), 36));
        for (auto& d : cfg.dirs) Rr.push_back(H(R(5, baseName(d), d), 84));
        int i = mainMenu(L, Rr, col, sl, sr);
        if (i == -9) return;
        if (i < 0) continue;
        int c = i / 1000, idx = i % 1000;
        int n = (int)cfg.dirs.size();
        if (c == 0) {
            if (idx == 0) doUpload();
            else if (idx == 1) doDownload();
            else if (idx == 3) { if (kbd("使用者名稱 (建議英文或數字)", cfg.name)) { cfg.name = cleanName(cfg.name); saveCfg(); } }
            else if (idx == 4) { if (kbd("WebDAV 位址 (例 https://dav.example.com/ns/)", cfg.url)) saveCfg(); }
            else if (idx == 5) { if (kbd("帳號", cfg.user)) saveCfg(); }
            else if (idx == 6) { string v; if (pwEdit(v)) { cfg.pass = v; saveCfg(); } }
            else if (idx == 7) doTest();
        } else if (idx == 0) {
            if (n >= 20) continue;
            string pth;
            if (pickFolder(pth)) {
                auto it = find(cfg.dirs.begin(), cfg.dirs.end(), pth);
                if (it == cfg.dirs.end()) { cfg.dirs.push_back(pth); saveCfg(); sr = 2 + (int)cfg.dirs.size() - 1; }
                else sr = 2 + (int)(it - cfg.dirs.begin());
            }
        } else if (idx >= 2 && idx < 2 + n) {
            int k = idx - 2;
            if (confirm("移除這個共享資料夾?\n" + cfg.dirs[k], "確定移除")) { cfg.dirs.erase(cfg.dirs.begin() + k); saveCfg(); sr = max(0, idx - 1); }
        }
    }
}

int main(int argc, char** argv) {
    mkdirs(string(CFG_DIR));
    { FILE* lf = fopen(LOG_FILE, "w"); if (lf) fclose(lf); }
    dbg("start  appletType=%d", (int)appletGetAppletType());
    {
        u64 tot = 0, used = 0;
        svcGetInfo(&tot, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        dbg("memory total=%llu MB used=%llu MB", (unsigned long long)(tot >> 20), (unsigned long long)(used >> 20));
    }
    Result rc = plInitialize(PlServiceType_User); dbg("plInitialize rc=0x%x", (unsigned)rc);
    rc = timeInitialize(); dbg("timeInitialize rc=0x%x", (unsigned)rc);
    if (R_FAILED(plGetSharedFontByType(&fdata, PlSharedFontType_ChineseTraditional)))
        plGetSharedFontByType(&fdata, PlSharedFontType_Standard);
    dbg("font addr=%p size=%u", fdata.address, (unsigned)fdata.size);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { dbg("SDL_Init failed: %s", SDL_GetError()); return 1; }
    dbg("SDL_Init ok");
    if (TTF_Init() != 0) { dbg("TTF_Init failed: %s", TTF_GetError()); return 1; }
    dbg("TTF_Init ok");
    win = SDL_CreateWindow("Save_WebDAV", 0, 0, 1280, 720, SDL_WINDOW_SHOWN);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!win || !ren) { dbg("window/renderer failed: %s", SDL_GetError()); return 1; }
    dbg("renderer ok");
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    dbg("input ok");
    rc = socketInitializeDefault(); dbg("socket rc=0x%x", (unsigned)rc);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    dbg("curl ok");
    loadCfg();
    dbg("config loaded  name=[%s] dirs=%d", cfg.name.c_str(), (int)cfg.dirs.size());
    if (cfg.name.empty()) {
        dbg("fetching nickname");
        string nk = cleanName(getNickname());
        dbg("nickname=[%s]", nk.c_str());
        if (!nk.empty()) { cfg.name = nk; saveCfg(); }
    }
    dbg("mainScreen start");
    mainScreen();
    dbg("mainScreen end");
    curl_global_cleanup();
    socketExit();
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    timeExit();
    plExit();
    dbg("exit ok");
    return 0;
}
