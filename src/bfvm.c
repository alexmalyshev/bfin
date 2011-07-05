/** @file bfvm.c
 *  @mainpage
 *  @brief A virtual machine for brainfuck.
 *
 *  Memory is implemented as a doubly linked list of chunks, with some
 *  tomfoolery to try to get malloc to give us blocks that are the same size
 *  as the system's page size. If the data pointer in the vm goes out of bounds
 *  we simply malloc on a new chunk and link it on the list, whether it be on
 *  the left or the right. Jumping between brackets is handled using a stack.
 *  When the vm reaches a '[' character, it looks to the right for the matching
 *  ']'. If the byte pointed to by the data pointer is zero, then we jump over
 *  to the right bracket and continue on from there. If it isn't zero, then we
 *  push the address of the left bracket onto a stack. When we get to the
 *  corresponding right bracket, if the byte pointed to by data is nonzero,
 *  then we jump back to the address at the top of the stack, it will be the
 *  address of the matching left bracket. If the byte is zero, then we pop off
 *  the top of the jump stack.
 *
 *  @author Alexander Malyshev
 *  @bug No known bugs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief A chunk of memory */
typedef struct memchunk_t {
    struct memchunk_t *next;    /**< the next chunk of memory */
    struct memchunk_t *prev;    /**< the previous chunk of memory */
    char *mem;                  /**< the block of bytes used as memory */
} memchunk;

/** @brief The stack used for jumping back to left brackets */
typedef struct jumpstack_t {
    struct jumpstack_t *next;   /**< the next stack */
    char *leftbracket;          /**< the address of a left bracket in memory */
} jumpstack;

/** @brief The size of chunks of memory on this system */
static long chunklen;

/** @brief The current chunk/page of memory we are on */
static memchunk *page;

/** @brief The data pointer specified by the brainfuck language */
static char *data;

/** @brief Initializes a new jumpstack.
 *
 *  Will exit with an error if call to calloc fails.
 *
 *  @return The address of a new jumpstack.
 */
static jumpstack *init_jumpstack(void) {
    jumpstack *stack = calloc(1, sizeof(jumpstack));
    if (stack == NULL) {
        fprintf(stderr, "Error: calloc failed, exiting");
        exit(1);
    }
    return stack;
}

/** @brief Frees all the jumpstacks associated with stack.
 *  @param stack the jumpstack we want to deallocate.
 *  @return Void.
 */
static void destroy_jumpstack(jumpstack *stack) {
    jumpstack *dead;
    
    while (stack != NULL) {
        dead = stack;
        stack = stack->next;
        free(dead);
    }
}

/** @brief Pushes on the address of a left bracket onto stack
 *  @param stack The address of the jumpstack we want to push leftbracket on.
 *  @param leftbracket The address of the left bracket we want to store.
 *  @return The resulting jumpstack from pushing leftbracket onto stack
 */
static jumpstack *push_jump(jumpstack *stack, char *leftbracket) {
    jumpstack *new = init_jumpstack();
    new->leftbracket = leftbracket;
    new->next = stack;
    return new;
}

/** @brief Pops the top address off of stack.
 *  @param stack The address of the jumpstack we want to pop the top off of.
 *  @return The resulting jumpstack after popping the top off of stack.
 */
static jumpstack *pop_jump(jumpstack *stack) {
    jumpstack *new = stack->next;
    free(stack);
    return new;
}

/** @brief Allocates and initializes a new chunk of memory.
 *
 *  Will exit with an error if call to calloc or call to malloc fail.
 *
 *  @return The address of a new memchunk.
 */
static memchunk *init_memchunk(void) {
    memchunk *chunk;

    if ((chunk = malloc(sizeof(memchunk))) == NULL) {
        perror("Error: malloc failed, exiting");
        exit(1);
    }
    if ((chunk->mem = calloc(1, chunklen)) == NULL) {
        perror("Error: calloc failed, exiting");
        exit(1);
    }
    
    chunk->prev = NULL;
    chunk->next = NULL;
    return chunk;
}

/** @brief Adds on more memory to the left of chunk.
 *  @param chunk the address of the current memchunk the vm is using.
 *  @return The address of the new memchunk the vm will be using.
 */
static memchunk *overflow_left(memchunk *chunk) {
    memchunk *new = init_memchunk();
    chunk->prev = new;
    new->next = chunk;
    return new;
}

/** @brief Adds on more memory to the right of chunk.
 *  @param chunk the address of the current memchunk the vm is using.
 *  @return The address of the new memchunk the vm will be using.
 */
static memchunk *overflow_right(memchunk *chunk) {
    memchunk *new = init_memchunk();
    chunk->next = new;
    new->prev = chunk;
    return new;
}

/** @brief Gets a single line of input from the terminal.
 *
 *  Will call realloc to dynamically resize a string to get all the input.
 *  Will exit with an error if call to realloc fails. start is a char buffer
 *  with length at least chunklen + 1.
 *
 *  @param start the buffer we want to use to copy the line of text into.
 *  @return The line of text read in from the terminal.
 */
static char *getline(char *start) {
    char *end = start;
    size_t len = chunklen;

    for (;;) {
        fgets(end, chunklen, stdin);
        if (strchr(end, '\n'))
            break;
        len += chunklen;
        start = realloc(start, len);
        if (start == NULL) {
            fputs("Error: Could not read in line\n", stderr);
            return NULL;
        }
        end += chunklen;
    }
    return start;
}

/** @brief Reads all the text from file into a char buffer.
 *
 *  Will call realloc to dynamically resize a string to get all the input.
 *  Will exit with an error if call to realloc fails. start is a char buffer
 *  with length at least chunklen + 1.
 *
 *  @param file the file we want to read input from.
 *  @param start the buffer we want to use to copy the file text into.
 *  @return The text read in from file.
 */
static char *getprog(FILE *file, char *start) {
    char *end = start;
    size_t len = chunklen;
    size_t read;

    for (;;) {
        read = fread(end, 1, chunklen, file);

        if (feof(file)) {
            end[read] = '\0';
            break;
        }
        len += chunklen;
        start = realloc(start, len);
        if (start == NULL) {
            fputs("Error: Could not read in file\n", stderr);
            return NULL;
        }
        end += chunklen;
    }
    return start;
}

/** @brief Finds the next valid right bracket.
 *
 *  *line is a left bracket. search to the right to find the next unmatched
 *  right bracket.
 *
 *  @param line the string in which we want to find a matching right bracket.
 *  @return the address of the right bracket if it exists, NULL otherwise.
 */
static char *match_right(char *line) {
    char *c;
    int count = 0;
    for (c = line + 1; *c != '\0'; ++c) {
        if (*c == '[')
            ++count;
        else if (*c == ']') {
            if (count != 0)
                --count;
            else
                return c;
        }
    }
    return 0;
}

/** @brief Executes a line of brainfuck code.
 *
 *  line must be a valid brainfuck program. exits with error if there is
 *  a bracket that wants us to jump but there is no matching bracket.
 *
 *  @param line the brainfuck code we want to execute.
 *  @return Void.
 */
static void execute(char *line) {
    char *c;
    char *right;
    jumpstack *stack = NULL;

    if (line == NULL)
        return;

    for (c = line; *c != '\0'; ++c) {
        switch (*c) {
            case '>':
                ++data;
                if (data >= page->mem + chunklen) {
                    if (page->next == NULL)
                        page = overflow_right(page);
                    else
                        page = page->next;
                    data = page->mem;
                }
                break;
            case '<':
                --data;
                if (data < page->mem) {
                    if (page->prev == NULL)
                        page = overflow_left(page);
                    else
                        page = page->prev;
                    data = page->mem + chunklen - 1;
                }
                break;
            case '+':
                ++(*data);
                break;
            case '-':
                --(*data);
                break;
            case '.':
                putchar(*data);
                break;
            case ',':
                *data = getchar();
                break;
            case '[':
                right = match_right(c);
                if (*data == 0) {
                    c = right;
                    if (c == NULL) {
                        fputs("Error: '[' with no matching ']'\n", stderr);
                        return;
                    }
                } else {
                    if (right != NULL)
                        stack = push_jump(stack, c);
                }
                break;
            case ']':
                if (*data != 0) {
                    if (stack == NULL || stack->leftbracket == NULL) {
                        fputs("Error: ']' with no matching '['\n",stderr);
                        return;

                    }
                    c = stack->leftbracket;
                } else
                    stack = pop_jump(stack);
                break;
        }
    }
    destroy_jumpstack(stack);
}

/** @brief Runs the brainfuck virtual machine.
 *
 *  Usage: $ bfvm
 *         Sets up the vm and starts the interpreter. Every line passed to the
 *         interpreter must be a valid brainfuck program, not just part of one.
 *
 *         $ bfvm filename
 *         Reads in the input file and executes it, then starts the
 *         interpreter.
 *
 *  @param argc the number of arguments.
 *  @param argv the array of arguments.
 *  @return Success status
 */
int main(int argc, char *argv[]) {
    char *line;
    FILE *file;

    chunklen = sysconf(_SC_PAGESIZE) - 8;
    if (chunklen < 0)
        chunklen += 8;

    page = init_memchunk();
    data = page->mem + chunklen/2;
    line = malloc(chunklen + 1);

    if (argc == 2) {
        file = fopen(argv[1], "r");
        if (file == NULL)
            fprintf(stderr, "Error: Could not open %s\n", argv[1]);
        else {
            line = getprog(file, line);

            fclose(file);

            execute(line);

            line = realloc(line, chunklen);
        }
    }

    for (;;) {
        printf("bfvm: ");
        line = getline(line);
 
        execute(line);

        line = realloc(line, chunklen);
    }
    return 0;
}
