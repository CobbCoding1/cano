#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include <curses.h>

#include "config.h"
#include "lex.c"
// global config vars

#define CRASH(str)                    \
        do {                          \
            endwin();                 \
            fprintf(stderr, str"\n"); \
            exit(1);                  \
        } while(0) 

#define ctrl(x) ((x) & 0x1f)

#define BACKSPACE   263 
#define ESCAPE      27
#define SPACE       32 
#define ENTER       10
#define DOWN_ARROW  258 
#define UP_ARROW    259 
#define LEFT_ARROW  260 
#define RIGHT_ARROW 261 

typedef enum {
    NORMAL,
    INSERT,
    SEARCH,
    COMMAND,
    VISUAL,
} Mode;

int ESCDELAY = 10;
time_t rawtime;
struct tm *timeinfo;

char run_time[80];

#define MAX_STRING_SIZE 1025

typedef struct {
    size_t index;
    size_t size;
    char *contents;
} Row;

#define MAX_ROWS 1024
#define STARTING_ROW_SIZE 128

typedef struct {
    size_t x;
    size_t y;
} Point;

typedef struct {
    Point starting_pos;
    Point ending_pos;
    int is_line;
} Visual;

typedef struct {
    Row *rows;
    size_t row_capacity;
    size_t row_index;
    size_t cur_pos;
    size_t row_s;
    char *filename;
    Visual visual;
} Buffer;

typedef struct {
    size_t size;
    char *arg;
} Arg;

typedef struct {
    char *command;
    size_t command_s;
    Arg args[16];
    size_t args_s;
} Command;

typedef struct {
    Buffer **buf_stack;
    size_t buf_stack_s;
    size_t buf_capacity;
} Undo;

typedef struct {
    Undo undo_stack;
    Undo redo_stack;
} State;

void shift_rows_left(Buffer *buf, size_t index);
void shift_row_left(Row *row, size_t index);
void shift_row_right(Row *row, size_t index);
void write_log(const char *message);

Mode mode = NORMAL;
int QUIT = 0;

int is_between(Point a, Point b, Point c) {
    if(a.y == b.y) {
        if(c.y == a.y && c.x <= b.x && c.x >= a.x) return 1;
    } else if ((a.y <= c.y && c.y <= b.y) || (b.y <= c.y && c.y <= a.y)) {
        if(c.y == b.y && c.x > b.x) return 0;
        if(c.y == a.y && c.x < a.x) return 0;
        return 1;  
    }
    return 0; 
}

char *stringify_mode() {
    switch(mode) {
        case NORMAL:
            return "NORMAL";
            break;
        case INSERT:
            return "INSERT";
            break;
        case SEARCH:
            return "SEARCH";
            break;
        case COMMAND:
            return "COMMAND";
            break;
        case VISUAL:
            return "VISUAL";
            break;
    }
    return "NORMAL";
}

typedef struct {
    char brace;
    int closing;
} Brace;

Brace find_opposite_brace(char opening) {
    switch(opening) {
        case '(':
            return (Brace){.brace = ')', .closing = 0};
            break;
        case '{':
            return (Brace){.brace = '}', .closing = 0};
            break;
        case '[':
            return (Brace){.brace = ']', .closing = 0};
            break;
        case ')':
            return (Brace){.brace = '(', .closing = 1};
            break;
        case '}':
            return (Brace){.brace = '{', .closing = 1};
            break;
        case ']':
            return (Brace){.brace = '[', .closing = 1};
            break;
    }
    return (Brace){.brace = '0'};
}

void free_buffer(Buffer **buffer) {
    write_log("before");
    for(size_t i = 0; i < (*buffer)->row_capacity; i++) {
        char msg[42] = {0};
        sprintf(msg, "%zu, %zu", i, (*buffer)->row_capacity);
        write_log(msg);
        if((*buffer)->rows[i].contents != NULL) {
            free((*buffer)->rows[i].contents);
            (*buffer)->rows[i].contents = NULL;
        }
        write_log(msg);
    }
    write_log("after");
    free((*buffer)->rows);
    free((*buffer)->filename);
    free(*buffer);
    *buffer = NULL;
}

Buffer *copy_buffer(Buffer *buffer) {
    write_log("copy b4");
    Buffer *buf = malloc(sizeof(Buffer));
    *buf = *buffer;
    size_t filename_s = strlen(buffer->filename)+1;
    buf->filename = malloc(filename_s*sizeof(char));
    strncpy(buf->filename, buffer->filename, filename_s);
    buf->rows = malloc(buffer->row_capacity*sizeof(Row));
    for(size_t i = 0; i < buffer->row_capacity; i++) {
        buf->rows[i].contents = malloc(MAX_STRING_SIZE*sizeof(char));
        memset(buf->rows[i].contents, 0, MAX_STRING_SIZE);
    }
    for(size_t i = 0; i < buffer->row_s+1; i++) {
        Row *cur = &buffer->rows[i];
        char siz[64] = {0};
        sprintf(siz, "%zu, %zu, %zu, %zu", buffer->rows[i].size, cur->size, buffer->row_s, buffer->row_capacity);
        buf->rows[i].size = cur->size; 
        buf->rows[i].index = cur->index; 
        write_log(siz);
        write_log(cur->contents);
        write_log("copy b5");
        strncpy(buf->rows[i].contents, cur->contents, buffer->rows[i].size);
    }
    return buf;
}

void shift_undo_left(Undo *undo, size_t amount) {
    for(size_t j = 0; j < amount; j++) {
        if(undo->buf_stack[0] != NULL) free_buffer(&undo->buf_stack[0]);
        write_log("free");
        for(size_t i = 1; i < undo->buf_stack_s; i++) {
            undo->buf_stack[i-1] = undo->buf_stack[i];
        }
        if(undo->buf_stack[undo->buf_stack_s] != NULL) {
            //free_buffer(&undo->buf_stack[undo->buf_stack_s]);
        }
        undo->buf_stack_s--;
    }
}

void push_undo(Undo *undo, Buffer *buf) {
    if(undo->buf_stack_s >= undo->buf_capacity) {
        shift_undo_left(undo, 1); 
    }
    Buffer *result = copy_buffer(buf); 
    write_log("push undo");
    undo->buf_stack[undo->buf_stack_s++] = result;
    write_log("push undo2");
}

Buffer *pop_undo(Undo *undo) {
    if(undo->buf_stack_s == 0) return NULL;
    Buffer *result = copy_buffer(undo->buf_stack[--undo->buf_stack_s]); 
    if(result != NULL) {
        free_buffer(&undo->buf_stack[undo->buf_stack_s]);
    }
    return result;
}

void resize_rows(Buffer *buffer, size_t capacity) {
    Row *rows = calloc(capacity*2, sizeof(Row));
    if(rows == NULL) {
        CRASH("no more ram");
    }
    memcpy(rows, buffer->rows, sizeof(Row)*buffer->row_capacity);
    buffer->rows = rows;
    for(size_t i = buffer->row_s; i < capacity*2; i++) {
        buffer->rows[i].contents = calloc(MAX_STRING_SIZE, sizeof(char));
        if(buffer->rows[i].contents == NULL) {
            CRASH("no more ram");
        }
    }
    buffer->row_capacity = capacity * 2;
}

void push_window(WINDOW *windows[16], size_t *windows_s, WINDOW *win) {
    windows[(*windows_s)++] = win;
}

void refresh_all(WINDOW *windows[16], size_t windows_s) {
    for(size_t i = 0; i < windows_s; i++) wrefresh(windows[i]);
}

void write_log(const char *message) {
    FILE *file = fopen("logs/cano.log", "a");
    if (file == NULL) {
        return;
    }
    
    fprintf(file, "%s\n", message);
    fclose(file);
}

Point search(Buffer *buffer, char *command, size_t command_s) {
    Point point = {
            .x = buffer -> cur_pos,
            .y = buffer -> row_index,
    };
    for(size_t i = buffer->row_index; i <= buffer->row_s+buffer->row_index; i++) {

        size_t index = (i + buffer->row_s+1) % (buffer->row_s+1);
        Row *cur = &buffer->rows[index];
        size_t j = (i == buffer->row_index) ? buffer->cur_pos+1 : 0;
        for(; j < cur->size; j++) {
            if(strncmp(cur->contents+j, command, command_s) == 0) {
                // result found and will return the location of the word.
                point.x = j;
                point.y = index;
                return point;
            }
        }
    }

    return point;

}

void replace(Buffer *buffer, Point position, char *new_str, size_t old_str_s, size_t new_str_s) { 
    if (buffer == NULL || new_str == NULL) {
        write_log("Error: null pointer");
        return;
    }

    Row *cur = &buffer->rows[position.y];
    if (cur == NULL || cur->contents == NULL) {
        write_log("Error: null pointer");
        return;
    }

    /*
    if (position.x + new_str_s > cur->size || position.x + old_str_s > cur->size) {
        write_log("Error: array index out of bounds");
        return;
    }
    */
    size_t new_s = cur->size + new_str_s - old_str_s;

    for(size_t i = position.x; i < position.x+old_str_s; i++) {
        shift_row_left(cur, position.x);
    }
    cur->size = new_s;

    // Move the contents after the old substring to make space for the new string
    for(size_t i = position.x; i < position.x+new_str_s; i++) {
        shift_row_right(cur, position.x);
    }

    // Copy the new string into the buffer at the specified position
    memcpy(cur->contents + position.x, new_str, new_str_s);
}


void find_and_replace(Buffer *buffer, char *old_str, char *new_str) { 
    size_t old_str_s = strlen(old_str);
    size_t new_str_s = strlen(new_str);

    // Search for the old string in the buffer
    Point position = search(buffer, old_str, old_str_s);
    if (position.x != (buffer->cur_pos) && position.y != (buffer->row_index)){
        // If the old string is found, replace it with the new string
        replace(buffer, position, new_str, old_str_s, new_str_s);
    }
}

size_t num_of_open_braces(Buffer *buffer) {
    int posy = buffer->row_index;
    int posx = buffer->cur_pos;
    int count = 0;
    Row *cur = &buffer->rows[posy];
    while(posy >= 0) {
        posx--;
        if(posx < 0) {
            posy--;
            if(posy < 0) break;
            cur = &buffer->rows[posy];
            posx = cur->size;
        }

        Brace brace = find_opposite_brace(cur->contents[posx]);
        if(brace.brace != '0') {
            if(!brace.closing) count++; 
            if(brace.closing) count--; 
        }
    }
    if(count < 0) return 0;
    return count;
}


void reset_command(char *command, size_t *command_s) {
    memset(command, 0, *command_s);
    *command_s = 0;
}

void handle_save(Buffer *buffer) {
    FILE *file = fopen(buffer->filename, "w"); 
    for(size_t i = 0; i <= buffer->row_s; i++) {
        fwrite(buffer->rows[i].contents, buffer->rows[i].size, 1, file);
        fwrite("\n", sizeof("\n")-1, 1, file);
    }
    fclose(file);
}

Command parse_command(char *command, size_t command_s) {
    Command cmd = {0};
    size_t args_start = 0;
    for(size_t i = 0; i < command_s; i++) {
        if(i == command_s-1 || command[i] == ' ') {
            cmd.command_s = (i == command_s-1) ? i+1 : i;
            cmd.command = malloc(sizeof(char)*cmd.command_s);
            strncpy(cmd.command, command, cmd.command_s);
            args_start = i+1;
            break;
        }
    }
    if(args_start <= command_s) {
        for(size_t i = args_start; i < command_s; i++) {
            if(i == command_s-1 || command[i] == ' ') {
                cmd.args[cmd.args_s].arg = malloc(sizeof(char)*i-args_start);
                strncpy(cmd.args[cmd.args_s].arg, command+args_start, i-args_start+1);
                cmd.args[cmd.args_s++].size = i-args_start+1;
                args_start = i+1;
            }
        }
    } 
    return cmd;
}

typedef enum {
    NO_ERROR,
    UNKNOWN_COMMAND,
    INVALID_ARG,
    INVALID_VALUE,
} Command_Errors;

int execute_command(Command *command, Buffer *buf, State *state) {
    if(command->command_s == 10 && strncmp(command->command, "set-output", 10) == 0) {
        if(command->args_s < 1) return INVALID_ARG; 
        char *filename = command->args[0].arg;
        free(buf->filename);
        buf->filename = malloc(sizeof(char)*command->args[0].size);
        strncpy(buf->filename, filename, command->args[0].size);
        for(size_t i = 0; i < command->args_s; i++) free(command->args[i].arg);
    } else if(command->command_s == 1 && strncmp(command->command, "e", 1) == 0) {
        QUIT = 1;
    } else if(command->command_s == 2 && strncmp(command->command, "we", 2) == 0) {
        handle_save(buf);
        QUIT = 1;
    } else if(command->command_s == 1 && strncmp(command->command, "w", 1) == 0) {
        handle_save(buf);
    } else if(command->command_s == 7 && strncmp(command->command, "set-var", 7) == 0) {
        if(command->args_s != 2) return INVALID_ARG;
        if(command->args[0].size >= 8 && strncmp(command->args[0].arg, "relative", 8) == 0) {
            if(command->args[1].size < 1) return INVALID_VALUE;
            int value = atoi(command->args[1].arg);
            if(value != 0 && value != 1) return INVALID_VALUE;
            relative_nums = value;
        } else if(command->args[0].size >= 11 && strncmp(command->args[0].arg, "auto-indent", 11) == 0) {
            if(command->args[1].size < 1) return INVALID_VALUE;
            int value = atoi(command->args[1].arg);
            if(value != 0 && value != 1) return INVALID_VALUE;
            auto_indent = value;
        } else if(command->args[0].size >= 6 && strncmp(command->args[0].arg, "indent", 6) == 0) {
            if(command->args[1].size < 1) return INVALID_VALUE;
            int value = atoi(command->args[1].arg);
            if(value < 0) return INVALID_VALUE;
            indent = value;
        } else if(command->args[0].size >= 6 && strncmp(command->args[0].arg, "syntax", 6) == 0) {
            if(command->args[1].size < 1) return INVALID_VALUE;
            int value = atoi(command->args[1].arg);
            if(value < 0) return INVALID_VALUE;
            syntax = value;
        } else if(command->args[0].size >= 9 && strncmp(command->args[0].arg, "undo-size", 9) == 0) {
            if(command->args[1].size < 1) return INVALID_VALUE;
            int value = atoi(command->args[1].arg);
            if(value < 0) return INVALID_VALUE;
            undo_size = value;
            state->undo_stack.buf_capacity = undo_size;
            for(size_t i = 0; i < state->undo_stack.buf_stack_s; i++) {
                free_buffer(&state->undo_stack.buf_stack[i]);
            }
            state->undo_stack.buf_stack_s = 0;
            free(state->undo_stack.buf_stack);
            state->undo_stack.buf_stack = calloc(state->undo_stack.buf_capacity, sizeof(Buffer));
            push_undo(&state->undo_stack, buf);
        } else {
            return UNKNOWN_COMMAND;
        }
    } else {
        return UNKNOWN_COMMAND;
    }
    free(command->command);
    return NO_ERROR;
}

// shift_rows_* functions shift the entire array of rows
void shift_rows_left(Buffer *buf, size_t index) {
    for(size_t i = index; i < buf->row_s; i++) {
        Row *cur = &buf->rows[i];
        Row *next = &buf->rows[i+1];
        memset(cur->contents, 0, cur->size);
        strncpy(cur->contents, next->contents, next->size);
        cur->size = next->size;
    }
    memset(buf->rows[buf->row_s].contents, 0, buf->rows[buf->row_s].size);
    buf->rows[buf->row_s].size = 0;
    buf->row_s--;
}

void shift_rows_right(Buffer *buf, size_t index) {
    if(buf->row_s+1 >= buf->row_capacity) resize_rows(buf, buf->row_capacity);
    char *new = calloc(MAX_STRING_SIZE, sizeof(char));
    if(new == NULL) {
        CRASH("no more ram");
    }
    for(size_t i = buf->row_s+1; i > index; i--) {
        buf->rows[i] = buf->rows[i-1];
    }
    buf->rows[index] = (Row){0};
    buf->rows[index].contents = new;
    buf->rows[index].index = index;
    buf->row_s++;
}

// shift_row_* functions shift single row
void shift_row_left(Row *row, size_t index) {
    for(size_t i = index; i < row->size; i++) {
        row->contents[i] = row->contents[i+1];
    }
    row->size--;  
}

void shift_row_right(Row *row, size_t index) {
    for(size_t i = row->size++; i > index; i--) {
        row->contents[i] = row->contents[i-1];
    }
    row->contents[index] = ' ';
}

void delete_char(Buffer *buffer, size_t row, size_t col, size_t *y, WINDOW *main_win) {
    Row *cur = &buffer->rows[row];
    if(cur->size > 0 && col < cur->size) {
        cur->contents[cur->size] = '\0';
        shift_row_left(cur, col);
        wmove(main_win, *y, col);
    }
}

void delete_row(Buffer *buffer, size_t row) {
    Row *cur = &buffer->rows[row];
    memset(cur->contents, 0, cur->size);
    cur->size = 0;
    if(buffer->row_s != 0) {
        shift_rows_left(buffer, row);
        if(row > buffer->row_s) row--;
    }
}

void shift_str_left(char *str, size_t *str_s, size_t index) {
    for(size_t i = index; i < *str_s; i++) {
        str[i] = str[i+1];
    }
    *str_s -= 1;
}

void shift_str_right(char *str, size_t *str_s, size_t index) {
    *str_s += 1;
    for(size_t i = *str_s; i > index; i--) {
        str[i] = str[i-1];
    }
}

#define NO_CLEAR_

void append_rows(Row *a, Row *b) {
    assert(a->size + b->size < MAX_STRING_SIZE);
    for(size_t i = 0; i < b->size; i++) {
        a->contents[(i + a->size)] = b->contents[i];
    }
    a->size = a->size + b->size;
}

void delete_and_append_row(Buffer *buf, size_t index) {
    append_rows(&buf->rows[index-1], &buf->rows[index]);
    delete_row(buf, index);
    //shift_rows_left(buf, index); 
}

void create_and_cut_row(Buffer *buf, size_t dest_index, size_t *str_s, size_t index) {
    assert(index < MAX_STRING_SIZE);
    assert(dest_index > 0);
    size_t final_s = *str_s - index;
    char *temp = calloc(final_s, sizeof(char));
    if(temp == NULL) {
        CRASH("no more ram");
    }
    size_t temp_len = 0;
    for(size_t i = index; i < *str_s; i++) {
        temp[temp_len++] = buf->rows[dest_index-1].contents[i];
        buf->rows[dest_index-1].contents[i] = '\0';
    }
    shift_rows_right(buf, dest_index);
    strncpy(buf->rows[dest_index].contents, temp, sizeof(char)*final_s);
    buf->rows[dest_index].size = final_s;
    *str_s = index;
    free(temp);
}

void create_newline_indent(Buffer *buffer, size_t num_of_braces) {
    if(!auto_indent) return;
    Row *cur = &buffer->rows[buffer->row_index];
    Brace brace = find_opposite_brace(cur->contents[buffer->cur_pos]);
    if(brace.brace != '0' && brace.closing) {
        create_and_cut_row(buffer, buffer->row_index+1,
                    &cur->size, buffer->cur_pos);
        for(size_t i = 0; i < indent*(num_of_braces-1); i++) {
            shift_row_right(&buffer->rows[buffer->row_index+1], i);
        }
    }
    for(size_t i = 0; i < indent*num_of_braces; i++) {
        shift_row_right(cur, buffer->cur_pos);
        cur->contents[buffer->cur_pos++] = ' ';
    }
}


void read_file_to_buffer(Buffer *buffer, char *filename) {
    size_t filename_s = strlen(filename);
    buffer->filename = malloc(sizeof(char)*filename_s);
    strncpy(buffer->filename, filename, filename_s);
    FILE *file = fopen(filename, "a+");
    if(file == NULL) {
        CRASH("could not open file");
    }
    fseek(file, 0, SEEK_END);
    size_t length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buf = malloc(sizeof(char)*length);
    fread(buf, sizeof(char)*length, 1, file);
    if(length > buffer->row_capacity) resize_rows(buffer, length);
    for(size_t i = 0; i+1 < length; i++) {
        if(buf[i] == '\n') {
            buffer->row_s++;
            continue;
        }
        buffer->rows[buffer->row_s].contents[buffer->rows[buffer->row_s].size++] = buf[i];
    }
}

int handle_motion_keys(Buffer *buffer, int ch, size_t *repeating_count) {
    switch(ch) {
        case 'g':
            if(*repeating_count-1 > 1 && *repeating_count-1 <= buffer->row_s) {
                buffer->row_index = *repeating_count-1;
                *repeating_count = 1;
            } else buffer->row_index = 0;
            break;
        case 'G':
            if(*repeating_count-1 > 1 && *repeating_count-1 <= buffer->row_s) {
                buffer->row_index = *repeating_count-1;
                *repeating_count = 1;
            } else buffer->row_index = buffer->row_s;
            break;
        case '0':
            buffer->cur_pos = 0;
            break;
        case '$':
            buffer->cur_pos = buffer->rows[buffer->row_index].size;
            break;
        case 'e': {
            Row *cur = &buffer->rows[buffer->row_index];
            while(buffer->cur_pos+1 < cur->size && cur->contents[buffer->cur_pos+1] == ' ') buffer->cur_pos++;
            if(cur->contents[buffer->cur_pos+1] == ' ') buffer->cur_pos++;
            while(cur->contents[buffer->cur_pos+1] != ' ' && buffer->cur_pos+1 < cur->size) {
                buffer->cur_pos++;
            }
        } break;
        case 'b': {
            Row *cur = &buffer->rows[buffer->row_index];
            if(cur->contents[buffer->cur_pos-1] == ' ') buffer->cur_pos--;
            while(cur->contents[buffer->cur_pos-1] != ' ' && buffer->cur_pos > 0) {
                buffer->cur_pos--;
            }
        } break;
        case 'w': {
            Row *cur = &buffer->rows[buffer->row_index];
            while(buffer->cur_pos+1 < cur->size && cur->contents[buffer->cur_pos+1] == ' ') buffer->cur_pos++;
            if(cur->contents[buffer->cur_pos-1] == ' ') buffer->cur_pos++;
            while(cur->contents[buffer->cur_pos-1] != ' ' && buffer->cur_pos < cur->size) {
                buffer->cur_pos++;
            }
        } break;
        case LEFT_ARROW:
        case 'h':
            if(buffer->cur_pos != 0) buffer->cur_pos--;
            break;
        case DOWN_ARROW:
        case 'j':
            if(buffer->row_index < buffer->row_s) buffer->row_index++;
            break;
        case UP_ARROW:
        case 'k':
            if(buffer->row_index != 0) buffer->row_index--;
            break;
        case RIGHT_ARROW:
        case 'l':
            buffer->cur_pos++;
            break;
        case '%': {
            Row *cur = &buffer->rows[buffer->row_index];
            char initial_brace = cur->contents[buffer->cur_pos];
            Brace initial_opposite = find_opposite_brace(initial_brace);
            if(initial_opposite.brace == '0') break;
            size_t brace_stack_s = 0;
            int posx = buffer->cur_pos;
            int posy = buffer->row_index;
            int dif = (initial_opposite.closing) ? -1 : 1;
            Brace opposite = {0};
            while(posy >= 0 && (size_t)posy <= buffer->row_s) {
                posx += dif;
                if(posx < 0 || (size_t)posx > cur->size) {
                    if(posy == 0 && dif == -1) break;
                    posy += dif;
                    cur = &buffer->rows[posy];
                    posx = (posx < 0) ? cur->size : 0;
                }
                opposite = find_opposite_brace(cur->contents[posx]);
                if(opposite.brace == '0') continue; 
                if((opposite.closing && dif == -1) || (!opposite.closing && dif == 1)) {
                    brace_stack_s++;
                } else {
                    if(brace_stack_s-- == 0 && opposite.brace == initial_brace) break;
                }
            }
            if((posx >= 0 && posy >= 0) && ((size_t)posy <= buffer->row_s)) {
                buffer->cur_pos = posx;
                buffer->row_index = posy;
            }
            break;
        }
        default: {
            return 0;
        } 
    }
    return 1;
}

int handle_modifying_keys(Buffer *buffer, int ch, WINDOW *main_win, size_t *y) {
    switch(ch) {
        case 'x': {
            delete_char(buffer, buffer->row_index, buffer->cur_pos, y, main_win);
        } break;
        case 'd': {
            delete_row(buffer, buffer->row_index);
            if(buffer->row_index != 0) buffer->row_index--;
        } break;
        case 'r': {
            curs_set(0);
            ch = wgetch(main_win);
            buffer->rows[buffer->row_index].contents[buffer->cur_pos] = ch;
            curs_set(1);
        } break;
        default: {
            return 0;
        }
    }
    return 1;
}

int handle_normal_to_insert_keys(Buffer *buffer, int ch) {
    switch(ch) {
        case 'i':
            break;
        case 'I': {
            Row *cur = &buffer->rows[buffer->row_index];
            buffer->cur_pos = 0;
            while(buffer->cur_pos < cur->size && cur->contents[buffer->cur_pos] == ' ') buffer->cur_pos++;
        } break;
        case 'a':
            if(buffer->cur_pos < buffer->rows[buffer->row_index].size) buffer->cur_pos++;
            break;
        case 'A':
            buffer->cur_pos = buffer->rows[buffer->row_index].size;
            break;
        case 'o': {
            shift_rows_right(buffer, buffer->row_index+1);
            buffer->row_index++; 
            buffer->cur_pos = 0;
            size_t num_of_braces = num_of_open_braces(buffer);
            if(num_of_braces > 0) {
                create_newline_indent(buffer, num_of_braces);
            }
        } break;
        case 'O': {
            shift_rows_right(buffer, buffer->row_index);
            buffer->cur_pos = 0;
            size_t num_of_braces = num_of_open_braces(buffer);
            if(num_of_braces > 0) {
                create_newline_indent(buffer, num_of_braces);
            }
        } break;
        default: {
            return 0;
        }
    }
    return 1;
}

void handle_keys(Buffer *buffer, Buffer **modify_buffer, State *state, WINDOW *main_win, WINDOW *status_bar, size_t *y, int ch, 
                 char *command, size_t *command_s, int *repeating, size_t *repeating_count, size_t *normal_pos, 
                 int *is_print_msg, char *status_bar_msg) {
    //(void)modify_buffer;
    switch(mode) {
        case NORMAL:
            switch(ch) {
                case ':':
                    mode = COMMAND;
                    buffer->cur_pos = 0;
                    *repeating_count = 1;
                    break;
                case '/':
                    mode = SEARCH;
                    *normal_pos = buffer->cur_pos;
                    buffer->cur_pos = 0;
                    reset_command(command, command_s);
                    *repeating_count = 1;
                    break;
                case 'v':
                    mode = VISUAL;
                    buffer->visual.is_line = 0;
                    buffer->visual.starting_pos.x = buffer->cur_pos;
                    buffer->visual.starting_pos.y = buffer->row_index;
                    buffer->visual.ending_pos.x = buffer->cur_pos;
                    buffer->visual.ending_pos.y = buffer->row_index;
                    break;
                case 'V':
                    mode = VISUAL;
                    buffer->visual.is_line = 1;
                    buffer->visual.starting_pos.x = 0;
                    buffer->visual.starting_pos.y = buffer->row_index;
                    buffer->visual.ending_pos.x = buffer->rows[buffer->row_index].size;
                    buffer->visual.ending_pos.y = buffer->row_index;
                    break;
                case ctrl('o'): {
                    shift_rows_right(buffer, ++buffer->row_index);
                    buffer->cur_pos = 0;
                    if(auto_indent) {
                        size_t num_of_braces = num_of_open_braces(buffer);
                        if(num_of_braces > 0) {
                            create_newline_indent(buffer, num_of_braces);
                        }
                    }
                } break;
                case 'R':
                    *repeating = 1;
                    break;
                case 'n': {
                    if(*command_s > 2 && strncmp(command, "s/", 2) == 0) {
                        // search and replace
                        char str[128]; // replace with the maximum length of your command
                        strncpy(str, command+2, *command_s-2);
                        str[*command_s-2] = '\0'; // ensure null termination

                        char *token = strtok(str, "/");
                        int count = 0;
                        char args[2][100];

                        while (token != NULL) {
                            char temp_buffer[100];
                            strcpy(temp_buffer, token);
                            if(count == 0) {
                                strcpy(args[0], temp_buffer);
                            } else if(count == 1) {
                                strcpy(args[1], temp_buffer);
                            }
                            ++count;

                            // temp message buffer
                            char message[100];
                            // format string to be logged later on
                            sprintf(message, "%s", token);

                            // log for args.
                            write_log(message);
                            token = strtok(NULL, "/");
                        }
                        Point new_pos = search(buffer, args[0], strlen(args[0]));
                        find_and_replace(buffer, args[0], args[1]);
                        buffer->cur_pos = new_pos.x;
                        buffer->row_index = new_pos.y;
                        break;

                    } 
                    Point new_pos = search(buffer, command, *command_s);
                    buffer->cur_pos = new_pos.x;
                    buffer->row_index = new_pos.y;
                } break;
                case 'u': {
                    write_log("BEFORE");
                    Buffer *new_buf = pop_undo(&state->undo_stack);
                    if(new_buf == NULL) break;
                    free_buffer(&buffer);
                    *modify_buffer = new_buf;
                    write_log("AFTER");
                } break;
                case 'U': {
                    Buffer *new_buf = pop_undo(&state->redo_stack);
                    if(new_buf == NULL) break; 
                    push_undo(&state->undo_stack, buffer);
                    *buffer = *new_buf;
                } break;
                case ctrl('s'): {
                    handle_save(buffer);
                    QUIT = 1;
                    *repeating_count = 1;
                } break;
                case ESCAPE:
                    reset_command(command, command_s);
                    mode = NORMAL;
                    break;
                default: {
                    int motion = handle_motion_keys(buffer, ch, repeating_count);
                    if(motion) break;
                    write_log("default1");
                    push_undo(&state->undo_stack, buffer);
                    write_log("default2");
                    int modified = handle_modifying_keys(buffer, ch, main_win, y);
                    (void)modified;
                    int switched = handle_normal_to_insert_keys(buffer, ch);
                    if(switched) {
                        mode = INSERT;
                        *repeating_count = 1;
                    }
                }break;
            }
            break;
        case INSERT: {
            switch(ch) {
                case '\b':
                case 127:
                case KEY_BACKSPACE: {
                    if(buffer->cur_pos == 0) {
                        if(buffer->row_index != 0) {
                            Row *cur = &buffer->rows[--buffer->row_index];
                            buffer->cur_pos = cur->size;
                            wmove(main_win, buffer->row_index, buffer->cur_pos);
                            delete_and_append_row(buffer, buffer->row_index+1);
                        }
                    } else {
                        Row *cur = &buffer->rows[buffer->row_index];
                        shift_row_left(cur, --buffer->cur_pos);
                        wmove(main_win, *y, buffer->cur_pos);
                    }
                } break;
                case ESCAPE:
                    mode = NORMAL;
                    break;
                case KEY_ENTER:
                case ENTER: {
                    Row *cur = &buffer->rows[buffer->row_index]; 
                    create_and_cut_row(buffer, buffer->row_index+1,
                                &cur->size, buffer->cur_pos);
                    buffer->row_index++; 
                    buffer->cur_pos = 0;
                    size_t num_of_braces = num_of_open_braces(buffer);
                    if(num_of_braces > 0) {
                        create_newline_indent(buffer, num_of_braces);
                    }
                } break;
                case LEFT_ARROW:
                    if(buffer->cur_pos != 0) buffer->cur_pos--;
                    break;
                case DOWN_ARROW:
                    if(buffer->row_index < buffer->row_s) buffer->row_index++;
                    break;
                case UP_ARROW:
                    if(buffer->row_index != 0) buffer->row_index--;
                    break;
                case RIGHT_ARROW:
                    if(buffer->cur_pos < buffer->rows[buffer->row_index].size) buffer->cur_pos++;
                    break;
                case KEY_RESIZE:
                    wrefresh(main_win);
                    break;
                default: {
                    Row *cur = &buffer->rows[buffer->row_index];
                    Brace cur_brace = find_opposite_brace(cur->contents[buffer->cur_pos]);
                    if(
                        (cur_brace.brace != '0' && cur_brace.closing && 
                         ch == find_opposite_brace(cur_brace.brace).brace) || 
                        (cur->contents[buffer->cur_pos] == '"' && ch == '"') ||
                        (cur->contents[buffer->cur_pos] == '\'' && ch == '\'')
                    ) {
                        buffer->cur_pos++;
                        break;
                    };
                    if(ch == 9) {
                        // TODO: use tabs instead of just 4 spaces
                        for(size_t i = 0; i < indent; i++) {
                            shift_row_right(cur, buffer->cur_pos);
                            cur->contents[buffer->cur_pos++] = ' ';
                        }
                    } else {
                        shift_row_right(cur, buffer->cur_pos);
                        cur->contents[buffer->cur_pos++] = ch;
                    }
                    Brace next_ch = find_opposite_brace(ch); 
                    if(next_ch.brace != '0' && !next_ch.closing) {
                        shift_row_right(cur, buffer->cur_pos);
                        cur->contents[buffer->cur_pos] = next_ch.brace;
                    } 
                    if(ch == '"' || ch == '\'') {
                        shift_row_right(cur, buffer->cur_pos);
                        cur->contents[buffer->cur_pos] = ch;
                    }
                } break;
            }
         } break;
        case COMMAND: {
            switch(ch) {
                case '\b':
                case 127:
                case KEY_BACKSPACE: {
                    if(buffer->cur_pos != 0) {
                        shift_str_left(command, command_s, --buffer->cur_pos);
                        wmove(status_bar, 1, buffer->cur_pos);
                    }
                } break;
                case ESCAPE:
                    reset_command(command, command_s);
                    mode = NORMAL;
                    break;
                case KEY_ENTER:
                case ENTER: {
                    if(command[0] == '!') {
                        shift_str_left(command, command_s, 0);
                        FILE *file = popen(command, "r");
                        if(file == NULL) {
                            CRASH("could not run command");
                        }
                        while(fgets(status_bar_msg, sizeof(status_bar_msg), file) != NULL) {
                            *is_print_msg = 1;
                        }
                        pclose(file);
                    } else {
                        Command cmd = parse_command(command, *command_s);
                        int err = execute_command(&cmd, buffer, state);
                        switch(err) {
                            case NO_ERROR:
                                break;
                            case UNKNOWN_COMMAND:
                                sprintf(status_bar_msg, "Unnown command: %s", cmd.command);
                                *is_print_msg = 1;
                                break;
                            case INVALID_ARG:
                                sprintf(status_bar_msg, "Invalid arg %s\n", cmd.args[0].arg);
                                *is_print_msg = 1;
                                break;
                            case INVALID_VALUE:
                                sprintf(status_bar_msg, "Invalid value %s\n", cmd.args[1].arg);
                                *is_print_msg = 1;
                                break;
                            default:
                                sprintf(status_bar_msg, "err");
                                *is_print_msg = 1;
                                break;
                        }
                    }
                    reset_command(command, command_s);
                    mode = NORMAL;
                } break;
                case LEFT_ARROW:
                    if(buffer->cur_pos != 0) buffer->cur_pos--;
                    break;
                case DOWN_ARROW:
                    break;
                case UP_ARROW:
                    break;
                case RIGHT_ARROW:
                    if(buffer->cur_pos < *command_s) buffer->cur_pos++;
                    break;
                default: {
                    shift_str_right(command, command_s, buffer->cur_pos);
                    command[buffer->cur_pos++] = ch;
                } break;
            }
        } break;
        case SEARCH: {
            switch(ch) {
                case '\b':
                case 127:
                case KEY_BACKSPACE: {
                    if(buffer->cur_pos != 0) {
                        shift_str_left(command, command_s, --buffer->cur_pos);
                        wmove(status_bar, 1, buffer->cur_pos);
                    }
                } break;
                case ESCAPE:
                    buffer->cur_pos = *normal_pos;
                    reset_command(command, command_s);
                    mode = NORMAL;
                    break;
                case ENTER: {
                    if(*command_s > 2 && strncmp(command, "s/", 2) == 0) {
                        /*
                        the search and replace works by taking the command and splitting it into two parts
                        to initialize the search and replace function. The first part is the search string
                        go in search mode and first type "s/" followed by the search string. Then type "/"
                        followed by what you want to replace it with.

                        Example:

                        s/Hello/World

                        This will replace all instances of "Hello" with "World" in the file.
                        */

                        // search and replace
                        char str[128]; // replace with the maximum length of your command
                        strncpy(str, command+2, *command_s-2);
                        str[*command_s-2] = '\0'; // ensure null termination

                        char *token = strtok(str, "/");
                        int count = 0;
                        char args[2][100];

                        while (token != NULL) {
                            char temp_buffer[100];
                            strcpy(temp_buffer, token);
                            if(count == 0) {
                                strcpy(args[0], temp_buffer);
                            } else if(count == 1) {
                                strcpy(args[1], temp_buffer);
                            }
                            ++count;

                            // temp message buffer
                            char message[100];
                            // format string to be logged later on
                            sprintf(message, "%s", token);

                            // log for args.
                            write_log(message);
                            token = strtok(NULL, "/");
                        }
                        Point new_pos = search(buffer, args[0], strlen(args[0]));
                        find_and_replace(buffer, args[0], args[1]);
                        buffer->cur_pos = new_pos.x;
                        buffer->row_index = new_pos.y;
                        mode = NORMAL;
                        break;

                    } 
                    Point new_pos = search(buffer, command, *command_s);
                    buffer->cur_pos = new_pos.x;
                    buffer->row_index = new_pos.y;
                    mode = NORMAL;
                } break;
                case LEFT_ARROW:
                    if(buffer->cur_pos != 0) buffer->cur_pos--;
                    break;
                case DOWN_ARROW:
                    break;
                case UP_ARROW:
                    break;
                case RIGHT_ARROW:
                    if(buffer->cur_pos < *command_s) buffer->cur_pos++;
                    break;
                default: {
                    shift_str_right(command, command_s, buffer->cur_pos);
                    command[buffer->cur_pos++] = ch;
                } break;
            }
        } break;
        case VISUAL: {
            curs_set(0);
            switch(ch) {
                case '\b':
                case 127:
                case KEY_BACKSPACE: {
                } break;
                case ESCAPE:
                    curs_set(1);
                    mode = NORMAL;
                    break;
                case ENTER: {
                } break;
                case 'd':
                case 'x':
                    push_undo(&state->undo_stack, buffer);
                    if(buffer->visual.starting_pos.y > buffer->visual.ending_pos.y || 
                      (buffer->visual.starting_pos.y == buffer->visual.ending_pos.y &&
                       buffer->visual.starting_pos.x > buffer->visual.ending_pos.x)) {
                        for(int i = buffer->visual.starting_pos.y; i >= (int)buffer->visual.ending_pos.y; i--) {
                            Row *cur = &buffer->rows[i];
                            if(i > (int)buffer->visual.ending_pos.y && i < (int)buffer->visual.starting_pos.y) delete_row(buffer, i);
                            else {
                                for(int j = cur->size-1; j >= 0; j--) {
                                    Point point = {.x = j, .y = i};
                                    if(is_between(buffer->visual.ending_pos, buffer->visual.starting_pos, point)) {
                                        delete_char(buffer, i, j, y, main_win);
                                    }
                                }
                            }
                        }
                    } else {
                        for(int i = buffer->visual.ending_pos.y; i >= (int)buffer->visual.starting_pos.y; i--) {
                            Row *cur = &buffer->rows[i];
                            if(i > (int)buffer->visual.starting_pos.y && i < (int)buffer->visual.ending_pos.y) delete_row(buffer, i);
                            else {
                                for(int j = cur->size-1; j >= 0; j--) {
                                    Point point = {.x = j, .y = i};
                                    if(is_between(buffer->visual.starting_pos, buffer->visual.ending_pos, point)) {
                                        delete_char(buffer, i, j, y, main_win);
                                    }
                                }
                            }
                        }
                    }
                    mode = NORMAL;
                    curs_set(1);
                    break;
                default: {
                    handle_motion_keys(buffer, ch, repeating_count);
                    if(buffer->visual.is_line) {
                        size_t cur_size = buffer->rows[buffer->row_index].size;
                        if(buffer->row_index <= buffer->visual.starting_pos.y) {
                            buffer->visual.ending_pos.x = 0;
                            buffer->visual.starting_pos.x = cur_size;
                            buffer->visual.ending_pos.y = buffer->row_index;
                        } else {
                            buffer->visual.ending_pos.x = cur_size;
                            buffer->visual.ending_pos.y = buffer->row_index;
                        }
                        break;
                    }
                    buffer->visual.ending_pos.x = buffer->cur_pos;
                    buffer->visual.ending_pos.y = buffer->row_index;
                } break;
            }
        } break;
    }
}

typedef struct {
    size_t row;
    size_t col;
    size_t size;
} Syntax_Highlighting;

int main(int argc, char **argv) {

    write_log("starting (int main)");

    time_t rawtime;
    struct tm *timeinfo;

    // get current time
    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(run_time, sizeof(run_time), "%Y-%m-%d %H:%M:%S", timeinfo);


    (void)argc;
    char *program = *argv++;
    char *flag = *argv++;
    char *config_filename = NULL;
    char *filename = NULL;
    while(flag != NULL) {
        if(strcmp(flag, "--config") == 0) {
            flag = *argv++;
            if(flag == NULL) {
                fprintf(stderr, "usage: %s --config <config.cano> <filename>\n", program);
                exit(1);
            }
            config_filename = flag;
            flag = *argv++;
        } else {
            filename = flag;
            flag = *argv++;
        }
    }

    initscr();

    if(has_colors() == FALSE) {
        CRASH("your terminal does not support colors");
    }

    // colors
    start_color();
    init_pair(YELLOW_COLOR, COLOR_YELLOW, COLOR_BLACK);
    init_pair(BLUE_COLOR, COLOR_BLUE, COLOR_BLACK);
    init_pair(GREEN_COLOR, COLOR_GREEN, COLOR_BLACK);
    init_pair(RED_COLOR, COLOR_RED, COLOR_BLACK);
    init_pair(MAGENTA_COLOR, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(CYAN_COLOR, COLOR_CYAN, COLOR_BLACK);

    noecho();
    raw();

    int grow, gcol;
    getmaxyx(stdscr, grow, gcol);
    int line_num_width = 5;
    WINDOW *main_win = newwin(grow-2, gcol-line_num_width, 0, line_num_width);
    WINDOW *line_num_win = newwin(grow-2, line_num_width, 0, 0);

    int main_row, main_col;
    int line_num_row, line_num_col;
    getmaxyx(main_win, main_row, main_col);
    getmaxyx(line_num_win, line_num_row, line_num_col);

    WINDOW *status_bar = newwin(2, gcol, grow-2, 0);

    WINDOW *windows[16] = {0};
    size_t  windows_s = 0; 
    push_window(windows, &windows_s, main_win);
    push_window(windows, &windows_s, line_num_win);
    push_window(windows, &windows_s, status_bar);

    refresh_all(windows, windows_s);
    keypad(main_win, TRUE);
    keypad(status_bar, TRUE);

    Buffer *buffer = malloc(sizeof(Buffer));
    buffer->row_capacity = STARTING_ROW_SIZE;
    buffer->rows = malloc(buffer->row_capacity * sizeof(Row));
    memset(buffer->rows, 0, sizeof(Row)*buffer->row_capacity);
    for(size_t i = 0; i < buffer->row_capacity; i++) {
        buffer->rows[i].contents = calloc(MAX_STRING_SIZE, sizeof(char));
        if(buffer->rows[i].contents == NULL) {
            CRASH("no more ram");
        }
    }
    if(filename != NULL) read_file_to_buffer(buffer, filename);
    else {
        buffer->filename = malloc(sizeof(char)*sizeof("out.txt"));
        strncpy(buffer->filename, "out.txt", sizeof("out.txt"));
    }

    mvwprintw(status_bar, 0, 0, "%.7s", stringify_mode());
    wmove(main_win, 0, 0);

    int ch = 0;

    char status_bar_msg[128] = {0};
    int is_print_msg = 0;

    int repeating = 0;

    size_t line_render_start = 0;
    size_t col_render_start = 0;
    char *command = malloc(sizeof(char)*64);
    size_t command_s = 0;

    size_t normal_pos = 0;

    State state = {0};
    state.undo_stack.buf_capacity = undo_size;
    state.redo_stack.buf_capacity = undo_size;
    state.undo_stack.buf_stack = calloc(state.undo_stack.buf_capacity, sizeof(Buffer));
    state.redo_stack.buf_stack = calloc(state.undo_stack.buf_capacity, sizeof(Buffer));
    push_undo(&state.undo_stack, buffer);

    write_log("finished loading");
    write_log("loading config");

    // load config
    if(config_filename == NULL) {
        char default_config_filename[128] = {0};
        char config_dir[64] = {0};
        sprintf(config_dir, "%s/.config/cano", getenv("HOME"));
        struct stat st = {0};
        if(stat(config_dir, &st) == -1) {
            mkdir(config_dir, 0755);
        }
        sprintf(default_config_filename, "%s/config.cano", config_dir);
        config_filename = default_config_filename;
    }
    char **lines = malloc(sizeof(char)*2);
    size_t lines_s = 0;
    int err = read_file_by_lines(config_filename, lines, &lines_s);
    if(err == 0) {
        for(size_t i = 0; i < lines_s; i++) {
            Command command = parse_command(lines[i], strlen(lines[i]));
            execute_command(&command, buffer, &state);
            free(lines[i]);
        }
    }
    free(lines);

    write_log("finished loading config");


    /*
    Syntax_Highlighting token_indexes[128] = {0};
    size_t token_indexes_s = 0;

    for(size_t i = 0; i < buffer->row_s; i++) {
        size_t token_capacity = 32;
        Token *token_arr = malloc(sizeof(Token)*token_capacity);
        size_t token_s = generate_tokens(buffer->rows[i].contents, buffer->rows[i].size, token_arr, &token_capacity);
        for(size_t j = 0; j < token_s; j++) {
            token_indexes[token_indexes_s++] = (Syntax_Highlighting){.row = i, .col=token_arr[j].index, .size=token_arr[j].size};
        }
        free(token_arr);
    }
    */

    size_t x = 0; 
    size_t y = 0;
    while(ch != ctrl('q') && QUIT != 1) {
        getmaxyx(main_win, main_row, main_col);
#ifndef NO_CLEAR
        werase(main_win);
        werase(status_bar);
        werase(line_num_win);
#endif
        int row_s_len = (int)log10(buffer->row_s)+2;
        if(row_s_len > line_num_col) {
            wresize(main_win, main_row, main_col-(row_s_len-line_num_width));
            wresize(line_num_win, line_num_row, row_s_len);
            mvwin(main_win, 0, row_s_len);
            getmaxyx(main_win, main_row, main_col);
            getmaxyx(line_num_win, line_num_row, line_num_col);
            line_num_width = row_s_len;
        }

        // status bar
        if(is_print_msg) {
            mvwprintw(status_bar, 1, 0, "%s", status_bar_msg);
            wrefresh(status_bar);
            sleep(1);
            wclear(status_bar);
            is_print_msg = 0;
        }
        if(mode == COMMAND || mode == SEARCH) {
            mvwprintw(status_bar, 1, 0, ":%.*s", (int)command_s, command);
        }
        mvwprintw(status_bar, 0, 0, "%.7s", stringify_mode());
        mvwprintw(status_bar, 0, gcol/2, "%.3zu:%.3zu", buffer->row_index+1, buffer->cur_pos+1);

        if(buffer->row_index <= line_render_start) line_render_start = buffer->row_index;
        if(buffer->row_index >= line_render_start+main_row) line_render_start = buffer->row_index-main_row+1;

        if(buffer->cur_pos <= col_render_start) col_render_start = buffer->cur_pos;
        if(buffer->cur_pos >= col_render_start+main_col) col_render_start = buffer->cur_pos-main_col+1;
        
        for(size_t i = line_render_start; i <= line_render_start+main_row; i++) {
            if(i <= buffer->row_s) {
                size_t print_index_y = i - line_render_start;
                wattron(line_num_win, COLOR_PAIR(YELLOW_COLOR));
                if(relative_nums) {
                    if(buffer->row_index == i) mvwprintw(line_num_win, print_index_y, 0, "%4zu", i+1);
                    else mvwprintw(line_num_win, print_index_y, 0, "%4zu", 
                                   (size_t)abs((int)i-(int)buffer->row_index));
                } else {
                    mvwprintw(line_num_win, print_index_y, 0, "%4zu", i+1);
                }
                wattroff(line_num_win, COLOR_PAIR(YELLOW_COLOR));

                size_t off_at = 0;

                size_t token_capacity = 32;
                Token *token_arr = malloc(sizeof(Token)*token_capacity);
                size_t token_s = 0;
                if(syntax) {
                    token_s = generate_tokens(buffer->rows[i].contents, 
                                                     buffer->rows[i].size, token_arr, &token_capacity);
                }
                
                Color_Pairs color = YELLOW_COLOR; 
                size_t j = 0;
                for(j = col_render_start; j <= col_render_start+main_col; j++) {
                    size_t keyword_size = 0;
                    if(syntax && is_in_tokens_index(token_arr, token_s, j, &keyword_size, &color)) {
                        wattron(main_win, COLOR_PAIR(color));
                        off_at = j + keyword_size;
                    }
                    if(syntax && j == off_at) wattroff(main_win, COLOR_PAIR(color));
                    if(mode == VISUAL) {
                        if(buffer->visual.starting_pos.y == buffer->visual.ending_pos.y) {

                        }
                        Point point = {.x = j, .y = i};
                        int between = 0;
                        if(buffer->visual.starting_pos.y > buffer->visual.ending_pos.y || 
                          (buffer->visual.starting_pos.y == buffer->visual.ending_pos.y &&
                           buffer->visual.starting_pos.x > buffer->visual.ending_pos.x)) {
                            between = is_between(buffer->visual.ending_pos, buffer->visual.starting_pos, point);
                        } else {
                            between = is_between(buffer->visual.starting_pos, buffer->visual.ending_pos, point);
                        }
                        if(between) {
                            wattron(main_win, A_STANDOUT);
                        }
                    }
                    size_t print_index_x = j - col_render_start;
                    mvwprintw(main_win, print_index_y, print_index_x, "%c", buffer->rows[i].contents[j]);
                    wattroff(main_win, A_STANDOUT);
                }
                free(token_arr);

            }
        }

        refresh_all(windows, windows_s);

        size_t repeating_count = 1;

        y = buffer->row_index-line_render_start;
        x = buffer->cur_pos-col_render_start;

        if(repeating) {
            mvwprintw(status_bar, 1, gcol-10, "r");
            wrefresh(status_bar);
        }

        if(mode != COMMAND && mode != SEARCH) {
            wmove(main_win, y, x);
            ch = wgetch(main_win);
        } else if(mode == COMMAND){
            wmove(status_bar, 1, buffer->cur_pos+1);
            ch = wgetch(status_bar);
        } else {
            wmove(status_bar, 1, buffer->cur_pos+1);
            ch = wgetch(status_bar);
        }

        if(repeating) {
            char num[32] = {0};
            size_t num_s = 0;
            while(isdigit(ch)) {
                num[num_s++] = ch;
                mvwprintw(status_bar, 1, (gcol-10)+num_s, "%c", num[num_s-1]);
                wrefresh(status_bar);
                ch = wgetch(main_win);
            }
            repeating_count = atoi(num);
            repeating = 0;
        }

        wmove(main_win, y, x);

        // TODO: move a lot of these extra variables into buffer struct
        for(size_t i = 0; i < repeating_count; i++) {
            handle_keys(buffer, &buffer, &state, main_win, status_bar, &y, ch, command, &command_s, 
                        &repeating, &repeating_count, &normal_pos, &is_print_msg, status_bar_msg);
        }
        if(mode != COMMAND && mode != SEARCH && buffer->cur_pos > buffer->rows[buffer->row_index].size) {
            buffer->cur_pos = buffer->rows[buffer->row_index].size;
        }
        x = buffer->cur_pos;
        y = buffer->row_index;
        getyx(main_win, y, x);
    }

    refresh_all(windows, windows_s);
    endwin();

    free_buffer(&buffer);
    for(size_t i = 0; i < state.undo_stack.buf_capacity; i++) {
        if(state.undo_stack.buf_stack[i] != NULL) {
            free_buffer(&state.undo_stack.buf_stack[i]);
        }
    }
    for(size_t i = 0; i < state.redo_stack.buf_capacity; i++) {
        if(state.redo_stack.buf_stack[i] != NULL) {
            free_buffer(&state.redo_stack.buf_stack[i]);
        }
    }
    return 0;
}
