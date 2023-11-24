/* includes */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* defines */

#define KILO_VERSION "0.0.1"

// set the upper three bits of the input character to 0 (mirroring what the ctrl key does)
#define CTRL_KEY(k) ((k) & 0x1f)

// choose a representation for arrow keys that doesn't conflict with wasd
enum editor_key {
    ARROW_LEFT  = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* data */

struct editor_config {
    size_t cx, cy;
    // number of rows and cols should never be negative
    size_t screen_rows;
    size_t screen_cols;
    struct termios orig_termios;
};

struct editor_config E;

/* terminal */

void die(const char *s) {
    // print a relevant error message and exit the program with a failure code

    // clear the screen if error is encountered
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void raw_mode_disable(void) {
    // turns on echoing in the terminal by disabling raw mode

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void raw_mode_enable(void) {
    // turns off echoing in the terminal by enabling raw mode

    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(raw_mode_disable);

    struct termios raw = E.orig_termios;

    // input flags
    // BRKINT enables sending a SIGINT signal to the program when a break condition is read
    // ICRNL enables converting carriage returns to newlines
    // INPCK enables parity checking
    // ISTRIP causes the 8th bit of each input byte to be stripped
    // IXON enables ctrl-s and ctrl-q (software flow control signals)
    raw.c_iflag &= ~(tcflag_t) (BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // output flags
    // OPOSE enables output processing of \n into \r\n (meaning \r\n needs to be manually written
    //       each time a newline will be written to output)
    raw.c_oflag &= ~(tcflag_t) OPOST;

    // control flags
    // CS8 sets the character size to 8 bits per byte
    raw.c_cflag |= (tcflag_t) CS8;

    // local flags
    // ECHO expands to 0000010, the bitwise NOT flips this to 1111101
    // bitwise AND forces the fourth bit in the flags field to become 0
    // ICANON enables canonical mode
    // IEXTEN enables ctrl-v
    // ISIG enables ctrl-c and ctrl-z
    raw.c_lflag &= ~(tcflag_t) (ECHO | ICANON | IEXTEN | ISIG);

    // sets the minimum number of input bytes needed before read() can return
    raw.c_cc[VMIN]  = 0;
    // sets the max time to wait before read() can return (tenths of a second)
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editor_read_key(void) {
    // waits for a single keypress and returns it

    // NOTE: read() returns ssize_t, not an int - do this to avoid conversion errors
    ssize_t nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // if an escape sequence is detected, immediately create a sequence buffer
    if (c == '\x1b') {
        // 3 bytes long to handle longer sequences in the future
        char seq[3];

        // if either of the two bytes times out, just return an escape
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        // look to see if the sequence is valid
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

                // detect page up and page down
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '2': return END_KEY;
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                // detect arrow keys
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == '0') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int get_cursor_position(size_t *rows, size_t *cols) {
    char buf[32];
    unsigned int i = 0;

    // query the terminal for the cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    // read the terminal's response into a buffer
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    // printf expects the null terminator
    buf[i] = '\0';

    // ensure the terminal responds first with a valid escape sequence
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;

    // pass a string of the form 24;80 to sscanf(), it will assign the ints to rows and cols
    if (sscanf(&buf[2], "%zu;%zu", rows, cols) != 2) return -1;

    return 0;
}

int get_window_size(size_t *rows, size_t *cols) {
    // return the number of rows and columns in the window

    struct winsize ws;

    // use ioctl in the ideal case
    // otherwise, position the cursor at the bottom-right, then use escape sequences to query the
    // position and thereby get the number of rows and cols
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // move the cursor to the bottom-right corner by moving forward and down by a large value
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return 0;
    }
}

/* append buffer */

struct abuf {
    char *b;
    size_t len;
};

// empty buffer constructor
#define ABUF_INIT {NULL, 0}

void ab_append(struct abuf *ab, const char *s, size_t len) {
    // append a string s to an abuf

    // make sure there's enough memory to hold the new string
    // realloc either extends the size of the memory block, or frees the current block and allocates
    // a new one that is large enough to store the string

    // realloc takes in size_t, not int; therefore len must be a size_t
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;

    // copy the string s after the end of the current data and update the pointer and length of abuf
    // memcpy takes size_t, not int
    memcpy(&new[ab->len], s, len);

    ab->b = new;
    ab->len += len;
}

void ab_free(struct abuf *ab) {
    // destructor that deallocates the dynamic memory used by an abuf

    free(ab->b);
}

/* output */

void editor_draw_rows(struct abuf *ab) {
    // handle drawing each row of the buffer

    size_t y;

    for (y = 0; y < E.screen_rows; y++) {
        // show the welcome message 1/3 of the way down
        if (y == E.screen_rows / 3) {
            char welcome[80];
            size_t welcome_len = (size_t) snprintf(welcome, sizeof(welcome), 
                "Kilo editor -- version %s", KILO_VERSION);

            if (welcome_len > E.screen_cols) welcome_len = E.screen_cols;

            // print the message in the middle of the screen
            size_t padding = (E.screen_cols - welcome_len) / 2;

            if (padding) {
                ab_append(ab, "~", 1);
                padding--;
            }

            while (padding--) ab_append(ab, " ", 1);

            ab_append(ab, welcome, welcome_len);
        } else {
            ab_append(ab, "~", 1);
        }

        // clear each line as it's redrawn
        ab_append(ab, "\x1b[K", 3);
        // print a newline for every row except the last
        if (y < E.screen_rows - 1) {
            ab_append(ab, "\r\n", 2);
        }
    }
}

void editor_refresh_screen(void) {
    struct abuf ab = ABUF_INIT;

    // hide cursor before refreshing the screen
    ab_append(&ab, "\x1b[?25l", 6);

    // position the cursor at the top-left corner of the terminal
    ab_append(&ab, "\x1b[H", 3);

    // draw a tilde at the beginning of each row
    editor_draw_rows(&ab);

    char buf[32];

    // specify the exact position to move the cursor (convert from 0 indexing to 1 indexing)
    snprintf(buf, sizeof(buf), "\x1b[%zu;%zuH", E.cy + 1, E.cx + 1);
    ab_append(&ab, buf, strlen(buf));

    // show the cursor once the screen is refreshed
    ab_append(&ab, "\x1b[?25h", 6);

    // write takes size_t, not int
    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/* input */

void editor_move_cursor(int key) {
    // move the cursor with wasd, note that +y is down

    switch (key) {
        case ARROW_LEFT:
            // prevent negative values in x-direction
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            // prevent the cursor from exiting the screen to the right
            if (E.cx != E.screen_cols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.screen_rows - 1) {
                E.cy++;
            }
            break;
    }
}

void editor_process_keypress(void) {
    // handles the keypresses returned by editor_key_read()

    int c = editor_read_key();

    switch (c) {
        case CTRL_KEY('q'):
            // clear the screen on user exit
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);

            exit(0);
            break;
        
        case HOME_KEY:
            E.cx = 0;
            break;
        
        case END_KEY:
            E.cx = E.screen_cols - 1;
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            // for now, page up and page down simply move the cursor to the top or bottom of the
            // screen by triggering arrow up or down multiple times
            {
                size_t times = E.screen_rows;
                while (times--)
                    editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editor_move_cursor(c);
            break;
    }
}

/* init */

void init_editor(void) {
    // set initial cursor position
    E.cx = 0;
    E.cy = 0;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) die("get_window_size");
}

int main(void) {
    // disable canonical mode so we can register input by keypress instead of needing to press enter
    raw_mode_enable();
    init_editor();

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
