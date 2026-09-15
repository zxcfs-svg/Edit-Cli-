#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <algorithm>
#include <cstring>

enum Key {
    KEY_CTRL_A = 1, KEY_CTRL_C = 3, KEY_CTRL_D = 4, KEY_CTRL_G = 7,
    KEY_CTRL_K = 11, KEY_CTRL_O = 15, KEY_CTRL_R = 18, KEY_CTRL_S = 19,
    KEY_CTRL_U = 21, KEY_CTRL_V = 22, KEY_CTRL_W = 23, KEY_CTRL_X = 24,
    KEY_CTRL_Y = 25,
    KEY_ENTER = '\r', KEY_ESC = 27, KEY_BACKSPACE = 127, KEY_TAB = '\t',
    ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    DEL_KEY, HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN
};

static inline int utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static inline int prevCharPos(const std::string& s, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) p--;
    return p;
}

static inline int nextCharPos(const std::string& s, int pos) {
    if (pos >= (int)s.size()) return (int)s.size();
    int len = utf8Len((unsigned char)s[pos]);
    int np = pos + len;
    if (np > (int)s.size()) np = (int)s.size();
    return np;
}

static int displayWidth(const std::string& s, int endByte) {
    int count = 0, i = 0;
    int end = std::min(endByte, (int)s.size());
    if (end < 0) end = 0;
    while (i < end) {
        int len = utf8Len((unsigned char)s[i]);
        if (i + len > end) break;
        i += len;
        count++;
    }
    return count;
}

static int displayWidthAll(const std::string& s) {
    return displayWidth(s, (int)s.size());
}

static std::string truncateToWidth(const std::string& s, int maxw) {
    int w = 0, i = 0;
    while (i < (int)s.size()) {
        int len = utf8Len((unsigned char)s[i]);
        if (i + len > (int)s.size()) break;
        if (w + 1 > maxw) break;
        i += len;
        w++;
    }
    return s.substr(0, i);
}

class Terminal {
    termios orig_termios;
public:
    Terminal() {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return;
        termios raw = orig_termios;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= (CS8);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    ~Terminal() {
        static const char seq[] = "\x1b[?25h\x1b[2J\x1b[H";
        (void)!write(STDOUT_FILENO, seq, sizeof(seq) - 1);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }

    static void getSize(int& rows, int& cols) {
        winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
            rows = 24; cols = 80;
        } else {
            rows = std::max(5, (int)ws.ws_row);
            cols = std::max(20, (int)ws.ws_col);
        }
    }

    static int readKey() {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) return -1;
        if (c == '\x1b') {
            pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 10) > 0) {
                char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESC;
                if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESC;
                if (seq[0] == '[') {
                    if (seq[1] >= '0' && seq[1] <= '9') {
                        if (read(STDIN_FILENO, &seq[2], 1) != 1) return KEY_ESC;
                        if (seq[2] == '~') {
                            switch (seq[1]) {
                                case '1': case '7': return HOME_KEY;
                                case '3': return DEL_KEY;
                                case '4': case '8': return END_KEY;
                                case '5': return PAGE_UP;
                                case '6': return PAGE_DOWN;
                            }
                        }
                    } else {
                        switch (seq[1]) {
                            case 'A': return ARROW_UP;
                            case 'B': return ARROW_DOWN;
                            case 'C': return ARROW_RIGHT;
                            case 'D': return ARROW_LEFT;
                            case 'H': return HOME_KEY;
                            case 'F': return END_KEY;
                        }
                    }
                } else if (seq[0] == 'O') {
                    switch (seq[1]) {
                        case 'H': return HOME_KEY;
                        case 'F': return END_KEY;
                    }
                }
                return KEY_ESC;
            }
            return KEY_ESC;
        }
        return (unsigned char)c;
    }
};

class ScreenBuffer {
    std::string buffer;
public:
    ScreenBuffer() { buffer.reserve(8192); }
    void append(const std::string& s) { buffer += s; }
    void flush() {
        (void)!write(STDOUT_FILENO, buffer.data(), buffer.size());
        buffer.clear();
    }
};

class Editor {
public:
    std::vector<std::string> lines = {""};
    int cx = 0, cy = 0, row_offset = 0, col_offset = 0;
    std::string clipboard = "";
    bool is_modified = false;
    bool is_selected_all = false;

    void clampCursor() {
        if (cy < 0) cy = 0;
        if (cy >= (int)lines.size()) cy = lines.size() - 1;
        if (cx < 0) cx = 0;
        if (cx > (int)lines[cy].size()) cx = lines[cy].size();
    }

    void scroll(int view_rows, int view_cols) {
        clampCursor();
        if (cy < row_offset) row_offset = cy;
        if (cy >= row_offset + view_rows) row_offset = cy - view_rows + 1;

        int dispCx  = displayWidth(lines[cy], cx);
        int dispOff = displayWidth(lines[cy], col_offset);
        if (dispCx < dispOff) {
            col_offset = cx;
        } else if (dispCx - dispOff >= view_cols) {
            int target = cx;
            int w = 0;
            while (target > 0 && w < view_cols - 1) {
                target = prevCharPos(lines[cy], target);
                w++;
            }
            col_offset = target;
        }
        if (col_offset < 0) col_offset = 0;
        if (col_offset > (int)lines[cy].size()) col_offset = 0;
    }

    void insertChar(char c) {
        if (is_selected_all) clearSelection();
        lines[cy].insert(cx++, 1, c);
        is_modified = true;
    }

    void insertString(const std::string& s) {
        if (is_selected_all) clearSelection();
        lines[cy].insert(cx, s);
        cx += (int)s.size();
        is_modified = true;
    }

    void insertTab() {
        insertString("    ");
    }

    void insertNewline() {
        if (is_selected_all) clearSelection();
        lines.insert(lines.begin() + cy + 1, lines[cy].substr(cx));
        lines[cy] = lines[cy].substr(0, cx);
        cy++; cx = 0;
        is_modified = true;
    }

    void backspace() {
        if (is_selected_all) { clearSelection(); return; }
        if (cx > 0) {
            int p = prevCharPos(lines[cy], cx);
            lines[cy].erase(p, cx - p);
            cx = p;
            is_modified = true;
        } else if (cy > 0) {
            cx = lines[cy - 1].size();
            lines[cy - 1] += lines[cy];
            lines.erase(lines.begin() + cy);
            cy--;
            is_modified = true;
        }
    }

    void delChar() {
        if (is_selected_all) { clearSelection(); return; }
        if (cx < (int)lines[cy].size()) {
            int np = nextCharPos(lines[cy], cx);
            lines[cy].erase(cx, np - cx);
            is_modified = true;
        } else if (cy < (int)lines.size() - 1) {
            lines[cy] += lines[cy + 1];
            lines.erase(lines.begin() + cy + 1);
            is_modified = true;
        }
    }

    bool copy() {
        if (is_selected_all) {
            clipboard.clear();
            for (size_t i = 0; i < lines.size(); ++i) {
                clipboard += lines[i];
                if (i + 1 < lines.size()) clipboard += "\n";
            }
            return true;
        }
        if (cy >= 0 && cy < (int)lines.size()) {
            clipboard = lines[cy];
            return true;
        }
        return false;
    }

    void paste() {
        if (clipboard.empty()) return;
        if (is_selected_all) { is_selected_all = false; lines = {""}; cx = 0; cy = 0; }

        size_t start = 0;
        while (start <= clipboard.size()) {
            size_t nl = clipboard.find('\n', start);
            std::string chunk = (nl == std::string::npos)
                ? clipboard.substr(start)
                : clipboard.substr(start, nl - start);

            std::string tail = lines[cy].substr(cx);
            lines[cy] = lines[cy].substr(0, cx) + chunk;
            cx = (int)lines[cy].size();

            if (nl == std::string::npos) break;

            lines.insert(lines.begin() + cy + 1, tail);
            cy++;
            cx = 0;
            start = nl + 1;
        }
        is_modified = true;
    }

    void deleteAll() {
        lines = {""};
        cx = 0; cy = 0;
        row_offset = 0; col_offset = 0;
        is_selected_all = false;
        is_modified = true;
    }

    void clearSelection() {
        lines = {""}; cx = 0; cy = 0;
        is_selected_all = false;
        is_modified = true;
    }
};

class UI {
    Editor& ed;
    std::string& filename;
    std::string& status_msg;
    std::string popup_msg;

    static void appendCell(std::string& row, const std::string& key, const std::string& label, int width) {
        std::string cell = "\x1b[7m" + key + "\x1b[0m " + label;
        int visible = (int)key.size() + 1 + (int)label.size();
        if (visible < width) cell.append(width - visible, ' ');
        row += cell;
    }

    void drawPopup(ScreenBuffer& sb, int rows, int cols) {
        if (popup_msg.empty()) return;

        std::vector<std::string> wrapped;
        std::string cur;
        int maxInner = std::max(10, std::min(cols - 8, 60));
        for (size_t i = 0; i < popup_msg.size(); ) {
            int len = utf8Len((unsigned char)popup_msg[i]);
            std::string ch = popup_msg.substr(i, len);
            if (ch == "\n") {
                wrapped.push_back(cur); cur.clear();
            } else if (displayWidthAll(cur) + 1 > maxInner) {
                wrapped.push_back(cur); cur = ch;
            } else {
                cur += ch;
            }
            i += len;
        }
        if (!cur.empty() || wrapped.empty()) wrapped.push_back(cur);

        int innerW = 0;
        for (auto& l : wrapped) innerW = std::max(innerW, displayWidthAll(l));
        innerW = std::min(innerW, maxInner);

        int boxW = innerW + 4;
        int boxH = (int)wrapped.size() + 2;
        int top  = std::max(1, (rows - boxH) / 2);
        int left = std::max(1, (cols - boxW) / 2);

        sb.append("\x1b[" + std::to_string(top) + ";" + std::to_string(left) + "H");
        sb.append("\x1b[7m+");
        for (int i = 0; i < innerW + 2; ++i) sb.append("-");
        sb.append("+\x1b[0m");

        for (size_t i = 0; i < wrapped.size(); ++i) {
            sb.append("\x1b[" + std::to_string(top + 1 + (int)i) + ";" + std::to_string(left) + "H");
            std::string line = wrapped[i];
            int lw = displayWidthAll(line);
            std::string pad(innerW - lw, ' ');
            sb.append("\x1b[7m| \x1b[0m" + line + pad + "\x1b[7m |\x1b[0m");
        }

        sb.append("\x1b[" + std::to_string(top + boxH - 1) + ";" + std::to_string(left) + "H");
        sb.append("\x1b[7m+");
        for (int i = 0; i < innerW + 2; ++i) sb.append("-");
        sb.append("+\x1b[0m");
    }

public:
    UI(Editor& ed, std::string& fn, std::string& status)
        : ed(ed), filename(fn), status_msg(status) {}

    void setPopup(const std::string& msg) { popup_msg = msg; }
    void clearPopup() { popup_msg.clear(); }
    bool hasPopup() const { return !popup_msg.empty(); }

    void render(int rows, int cols) {
        int view_rows = std::max(1, rows - 4);
        int view_cols = std::max(1, cols);

        ed.scroll(view_rows, view_cols);

        ScreenBuffer sb;
        sb.append("\x1b[?25l\x1b[H");

        std::string fname_disp = filename.empty() ? "New Buffer" : filename;
        std::string mod_disp = ed.is_modified ? " [Modified]" : "";
        std::string header = "  edit 1.0   " + fname_disp + mod_disp;
        int hw = displayWidthAll(header);
        if (hw < view_cols) header.append(view_cols - hw, ' ');
        else header = truncateToWidth(header, view_cols);
        sb.append("\x1b[7m" + header + "\x1b[0m\r\n");

        for (int i = 0; i < view_rows; ++i) {
            int file_line = ed.row_offset + i;
            if (file_line < (int)ed.lines.size()) {
                std::string line = ed.lines[file_line];
                if (ed.col_offset > 0) {
                    if ((int)line.size() > ed.col_offset) line = line.substr(ed.col_offset);
                    else line.clear();
                }
                line = truncateToWidth(line, view_cols);
                if (ed.is_selected_all) sb.append("\x1b[7m" + line + "\x1b[0m");
                else sb.append(line);
            }
            sb.append("\x1b[K\r\n");
        }

        sb.append("\x1b[K");
        if (!status_msg.empty()) {
            std::string m = truncateToWidth(status_msg, view_cols);
            sb.append(m);
        }
        sb.append("\r\n");

        int columns = 6;
        int col_width = std::max(4, view_cols / columns);
        std::string row1, row2;
        appendCell(row1, "^S", "Save", col_width);
        appendCell(row1, "^A", "All", col_width);
        appendCell(row1, "^C", "Copy", col_width);
        appendCell(row1, "^V", "Paste", col_width);
        appendCell(row1, "^D", "DelAll", col_width);
        appendCell(row1, "^X", "Exit", col_width);
        appendCell(row2, "^W", "Find", col_width);
        appendCell(row2, "^R", "Repl", col_width);
        appendCell(row2, "^Y", "PgUp", col_width);
        appendCell(row2, "^O", "PgDn", col_width);
        appendCell(row2, "^G", "Help", col_width);
        appendCell(row2, "Tab", "4sp", col_width);
        sb.append(row1 + "\x1b[K\r\n");
        sb.append(row2 + "\x1b[K");

        drawPopup(sb, rows, cols);

        int screen_y = ed.cy - ed.row_offset + 2;
        int screen_x = displayWidth(ed.lines[ed.cy], ed.cx)
                     - displayWidth(ed.lines[ed.cy], ed.col_offset) + 1;
        if (screen_x < 1) screen_x = 1;
        if (screen_x > view_cols) screen_x = view_cols;
        sb.append("\x1b[" + std::to_string(screen_y) + ";" + std::to_string(screen_x) + "H");
        sb.append("\x1b[?25h");

        sb.flush();
    }
};

class FlexApp {
    Terminal term;
    Editor ed;
    std::string filename;
    std::string status_msg;
    UI ui;
    std::string pending_popup;
    bool show_popup_next = false;

    void load() {
        if (filename.empty()) {
            status_msg = "New Buffer";
            return;
        }
        std::ifstream file(filename);
        if (file.is_open()) {
            ed.lines.clear();
            std::string line;
            while (std::getline(file, line)) ed.lines.push_back(line);
            if (ed.lines.empty()) ed.lines.push_back("");
            status_msg = "Read " + std::to_string(ed.lines.size()) + " lines";
        } else {
            status_msg = "New File: " + filename;
        }
    }

    void notify(const std::string& msg) {
        pending_popup = msg;
        show_popup_next = true;
    }

    bool prompt(const std::string& label, const std::string& initial, std::string& result) {
        std::string input = initial;
        while (true) {
            int rows, cols;
            Terminal::getSize(rows, cols);
            ui.render(rows, cols);

            int promptRow = rows - 2;
            std::string bar = label + input;

            int barw = displayWidthAll(bar);
            if (barw > cols) {
                int excess = barw - cols;
                int i = 0;
                while (excess > 0 && i < (int)bar.size()) {
                    int len = utf8Len((unsigned char)bar[i]);
                    i += len;
                    excess--;
                }
                bar = bar.substr(i);
            }

            std::string out = "\x1b[" + std::to_string(promptRow) + ";1H\x1b[K" + bar;
            int curCol = displayWidthAll(bar) + 1;
            out += "\x1b[" + std::to_string(promptRow) + ";" + std::to_string(curCol) + "H\x1b[?25h";
            (void)!write(STDOUT_FILENO, out.data(), out.size());

            int c = Terminal::readKey();
            if (c == -1) continue;
            if (c == KEY_ENTER || c == '\n') { result = input; return true; }
            if (c == KEY_ESC || c == KEY_CTRL_C) return false;
            if (c == KEY_BACKSPACE || c == 8) {
                if (!input.empty()) {
                    int p = prevCharPos(input, (int)input.size());
                    input.erase(p);
                }
                continue;
            }
            unsigned char uc = (unsigned char)c;
            if (uc >= 32 && c < 1000) input.push_back((char)c);
        }
    }

    bool writeFile() {
        std::ofstream file(filename, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            notify("Error writing " + filename);
            return false;
        }
        for (size_t i = 0; i < ed.lines.size(); ++i)
            file << ed.lines[i] << "\n";
        file.close();
        ed.is_modified = false;
        return true;
    }

    void save() {
        if (filename.empty()) {
            std::string name;
            if (!prompt("File Name to Write: ", "untitled.txt", name) || name.empty()) {
                status_msg = "Cancelled";
                return;
            }
            filename = name;
        }
        if (writeFile()) {
            status_msg = "Saved: " + filename;
            notify("File saved:\n" + filename);
        }
    }

    void doSearch() {
        std::string term;
        if (!prompt("Search: ", "", term) || term.empty()) {
            status_msg = "Cancelled";
            return;
        }
        int n = (int)ed.lines.size();
        for (int i = 0; i <= n; ++i) {
            int li = (ed.cy + i) % n;
            size_t from = (i == 0) ? (size_t)ed.cx + 1 : 0;
            if (from > ed.lines[li].size()) continue;
            size_t pos = ed.lines[li].find(term, from);
            if (pos != std::string::npos) {
                ed.cy = li; ed.cx = (int)pos;
                status_msg = "Found \"" + term + "\"";
                return;
            }
        }
        status_msg = "\"" + term + "\" not found";
        notify("Not found:\n" + term);
    }

    void doReplace() {
        std::string term, repl;
        if (!prompt("Search (to replace): ", "", term) || term.empty()) {
            status_msg = "Cancelled";
            return;
        }
        if (!prompt("Replace with: ", "", repl)) {
            status_msg = "Cancelled";
            return;
        }
        int count = 0;
        for (auto& line : ed.lines) {
            size_t pos = 0;
            while ((pos = line.find(term, pos)) != std::string::npos) {
                line.replace(pos, term.size(), repl);
                pos += repl.size();
                count++;
            }
        }
        if (count > 0) ed.is_modified = true;
        status_msg = "Replaced " + std::to_string(count) + " occurrence" + (count == 1 ? "" : "s");
        notify("Replaced " + std::to_string(count) + " occurrence" + (count == 1 ? "" : "s"));
    }

    bool confirmExit() {
        if (!ed.is_modified) return true;
        std::string ans;
        if (!prompt("Save modified buffer? (y/n): ", "", ans)) return false;
        if (!ans.empty() && (ans[0] == 'y' || ans[0] == 'Y')) save();
        return true;
    }

public:
    FlexApp(const std::string& fn) : filename(fn), ui(ed, filename, status_msg) { load(); }

    void run() {
        while (true) {
            int rows, cols;
            Terminal::getSize(rows, cols);

            if (show_popup_next) {
                ui.setPopup(pending_popup);
                pending_popup.clear();
                show_popup_next = false;
            } else if (ui.hasPopup()) {
                ui.clearPopup();
            }

            ui.render(rows, cols);

            int c = Terminal::readKey();
            if (c == -1) continue;

            if (ui.hasPopup()) {
                ui.clearPopup();
                continue;
            }

            if (c == KEY_CTRL_X) {
                if (confirmExit()) break;
                status_msg.clear();
                continue;
            }

            switch (c) {
                case KEY_CTRL_S:
                    save();
                    break;
                case KEY_CTRL_A:
                    ed.is_selected_all = !ed.is_selected_all;
                    status_msg = ed.is_selected_all ? "Marked all text" : "Unmarked text";
                    break;
                case KEY_CTRL_C:
                    if (ed.copy()) status_msg = "Copied to clipboard";
                    else status_msg = "Nothing to copy";
                    break;
                case KEY_CTRL_V:
                    ed.paste();
                    status_msg.clear();
                    break;
                case KEY_CTRL_D:
                    ed.deleteAll();
                    status_msg = "Cleared buffer";
                    break;
                case KEY_CTRL_W:
                    doSearch();
                    break;
                case KEY_CTRL_R:
                    doReplace();
                    break;
                case KEY_CTRL_G:
                    status_msg = "^S Save  ^X Exit  ^W Find  ^R Repl  ^C Copy  ^V Paste  ^A All  ^D DelAll";
                    break;
                case KEY_CTRL_Y: case PAGE_UP:
                    ed.cy -= std::max(1, rows - 4);
                    status_msg.clear();
                    break;
                case KEY_CTRL_O: case PAGE_DOWN:
                    ed.cy += std::max(1, rows - 4);
                    status_msg.clear();
                    break;
                case KEY_BACKSPACE: case 8:
                    ed.backspace();
                    status_msg.clear();
                    break;
                case DEL_KEY:
                    ed.delChar();
                    status_msg.clear();
                    break;
                case KEY_ENTER: case '\n':
                    ed.insertNewline();
                    status_msg.clear();
                    break;
                case KEY_TAB:
                    ed.insertTab();
                    status_msg.clear();
                    break;
                case ARROW_UP:
                    ed.cy--; status_msg.clear(); break;
                case ARROW_DOWN:
                    ed.cy++; status_msg.clear(); break;
                case ARROW_LEFT:
                    if (ed.cx > 0) ed.cx = prevCharPos(ed.lines[ed.cy], ed.cx);
                    else if (ed.cy > 0) { ed.cy--; ed.cx = ed.lines[ed.cy].size(); }
                    status_msg.clear();
                    break;
                case ARROW_RIGHT:
                    if (ed.cx < (int)ed.lines[ed.cy].size()) ed.cx = nextCharPos(ed.lines[ed.cy], ed.cx);
                    else if (ed.cy < (int)ed.lines.size() - 1) { ed.cy++; ed.cx = 0; }
                    status_msg.clear();
                    break;
                case HOME_KEY:
                    ed.cx = 0; status_msg.clear(); break;
                case END_KEY:
                    ed.cx = ed.lines[ed.cy].size(); status_msg.clear(); break;
                default: {
                    unsigned char uc = (unsigned char)c;
                    if (uc >= 32 && c < 1000) {
                        ed.insertChar((char)c);
                        status_msg.clear();
                    }
                    break;
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    std::string fn = (argc >= 2) ? argv[1] : "";
    FlexApp app(fn);
    app.run();
    return 0;
}
