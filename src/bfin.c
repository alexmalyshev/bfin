/**
 * @file bfin.c
 * @mainpage
 * @brief An interpreter for brainfuck.
 *
 * Memory is implemented as a doubly linked list of chunks. If the data pointer
 * goes out of bounds we simply malloc a new chunk and link it into the list,
 * whether it be on the left or the right. Jumping between brackets is handled
 * using a stack. When the interpreter reaches a '[' character, it looks ahead
 * for the matching ']'. If the byte pointed to by the data pointer is zero,
 * then we jump over to the right bracket and continue on from there. If it
 * isn't zero, then we push the address of the left bracket onto a stack. When
 * we get to the corresponding right bracket, if the byte pointed to by data is
 * nonzero, then we jump back to the address at the top of the stack, it will
 * be the address of the matching left bracket. If the byte is zero, then we
 * pop off the top of the jump stack.
 *
 * @author Alexander Malyshev
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>


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


/* function that sets up the interpreter */
static void init_bfin(void);
/* jumpstack functions */
static jumpstack *alloc_jumpstack(void);
static void destroy_jumpstack(jumpstack *stack);
static jumpstack *push_jump(jumpstack *stack, char *leftbracket);
static jumpstack *pop_jump(jumpstack *stack);
/* memchunk functions */
static memchunk *alloc_memchunk(void);
static memchunk *overflow_left(memchunk *chunk);
static memchunk *overflow_right(memchunk *chunk);
/* reading input */
static char *get_line(char *start);
static char *get_prog(FILE *file, char *start);
/* executing brainfuck code */
static char *match_right(char *line);
static void execute(char *line);


/** @brief The size of chunks of memory on this system */
static long chunklen;

/** @brief The current chunk/page of memory we are on */
static memchunk *page;

/** @brief The data pointer specified by the brainfuck language */
static char *data;


/**
 * @brief Runs the brainfuck interpreter
 *
 * @param argc the number of arguments
 * @param argv the array of arguments
 *
 * @return Success status
 */
int main(int argc, char *argv[]) {
    /*
     * Usage: bfin [filename]
     *        Sets up and starts the interpreter. Every line passed to the
     *        interpreter must be a valid brainfuck program, not just part of
     *        one. If an input file is specified, then the interpreter will
     *        first open that file and attempt to execute it.
     */

    char *line;
    FILE *file;

    /* set up the interpreter */
    init_bfin();

    /* our initial line of text is just our chunk size (plus one for '\0').
     * we'll potentially resize line in get_line and get_prog, but we'll
     * always realloc it back to this original size for memory efficiency. */
    line = malloc(chunklen + 1);
    if (line == NULL) {
        fprintf(stderr, "Memory Allocation Error: Failed at allocating "
                        "internal text buffer, exiting\n");
        exit(1);
    }

    /* if there was a file specified, attempt to read it in and execute it */
    if (argc == 2) {
        file = fopen(argv[1], "r");
        if (file == NULL)
            fprintf(stderr, "IO Error: Could not open '%s'\n", argv[1]);
        else {
            line = get_prog(file, line);
            fclose(file);
            execute(line);
            line = realloc(line, chunklen);
            if (line == NULL) {
                fprintf(stderr, "Memory Allocation Error: Failed at resizing "
                                "internal text buffer, exiting\n");
                exit(1);
            }
        }
    }

    /* keep on getting lines of code and executing them */
    while (1) {
        printf("bfin: ");
        line = get_line(line);
        execute(line);

        /* clear out the line we just read in (attempt to handle EOF nicely) */
        line[0] = '\0';

        /* truncate 'line' back down to a page and reuse it */
        line = realloc(line, chunklen);
        if (line == NULL) {
            fprintf(stderr, "Memory Allocation Error: Failed at resizing "
                    "internal text buffer, exiting\n");
            exit(1);
        }
    }

    return 0;
}


/** @brief Initializes the brainfuck interpreter */
static void init_bfin(void) {
    /* hopefully this -32 will be enough for malloc's extra block data
     * and that we'll simply be handed a single page */
    chunklen = sysconf(_SC_PAGESIZE) - 32;

    /* if our system has a really small pagesize, don't modify it */
    if (chunklen < 0)
        chunklen += 32;

    /* set up our initial page of memory */
    page = alloc_memchunk();

    /* set the data pointer in the middle of the memory chunk. that way we
     * won't overflow left immediately should the program decide to move left
     * from the very beginning */
    data = page->mem + chunklen/2;
}


/**
 * @brief Allocates and initializes a new jumpstack
 *
 * Will exit with an error if call to calloc fails
 *
 * @return The address of a new jumpstack
 */
static jumpstack *alloc_jumpstack(void) {
    jumpstack *stack = calloc(1, sizeof(jumpstack));
    if (stack == NULL) {
        fprintf(stderr, "Memory Allocation Error: Could not allocate a "
                        "jumpstack, exiting\n");
        exit(1);
    }

    return stack;
}


/**
 * @brief Frees all the jumpstacks associated with 'stack'
 *
 * 'stack' may be NULL, in which case this function will do nothing
 *
 * @param stack the jumpstack we want to deallocate
 */
static void destroy_jumpstack(jumpstack *stack) {
    jumpstack *dead;
    
    while (stack != NULL) {
        dead = stack;
        stack = stack->next;
        free(dead);
    }
}


/**
 * @brief Pushes the address of a left bracket onto 'stack'
 *
 * 'stack' may be NULL
 *
 * @param stack The address of the jumpstack we want to push leftbracket on
 * @param leftbracket The address of the left bracket we want to store
 *
 * @return The resulting jumpstack from pushing leftbracket onto stack
 */
static jumpstack *push_jump(jumpstack *stack, char *leftbracket) {
    jumpstack *new;

    assert(leftbracket != NULL);

    new = alloc_jumpstack();
    new->leftbracket = leftbracket;
    new->next = stack;
    return new;
}


/**
 * @brief Pops the top address off of stack
 *
 * @param stack The address of the jumpstack we want to pop the top off of
 *
 * @return The resulting jumpstack after popping the top off of stack
 */
static jumpstack *pop_jump(jumpstack *stack) {
    jumpstack *new;

    assert(stack != NULL);

    new = stack->next;

    free(stack);

    return new;
}


/**
 * @brief Allocates and initializes a new chunk of memory
 *
 * Will exit with an error if call to calloc or call to malloc fail
 *
 * @return The address of a new memchunk
 */
static memchunk *alloc_memchunk(void) {
    memchunk *chunk;

    if ((chunk = malloc(sizeof(memchunk))) == NULL) {
        fprintf(stderr, "Memory Allocation Error: Could not allocate a chunk "
                        "type, exiting\n");
        exit(1);
    }
    if ((chunk->mem = malloc(chunklen)) == NULL) {
        fprintf(stderr, "Memory Allocation Error: Could not allocate a chunk "
                        "of memory, exiting\n");
        exit(1);
    }
    
    chunk->prev = NULL;
    chunk->next = NULL;

    return chunk;
}


/**
 * @brief Adds on more memory to the left of chunk
 *
 * @param chunk the address of the current memchunk the interpreter is using
 *
 * @return The address of the new memchunk the interpreter will be using
 */
static memchunk *overflow_left(memchunk *chunk) {
    memchunk *new;

    assert(chunk != NULL);

    /* the left of the chunk is supposed to be empty */
    assert(chunk->prev == NULL);

    new = alloc_memchunk();
    chunk->prev = new;
    new->next = chunk;

    return new;
}


/**
 * @brief Adds on more memory to the right of chunk
 *
 * @param chunk the address of the current memchunk the interpreter is using
 *
 * @return The address of the new memchunk the interpreter will be using
 */
static memchunk *overflow_right(memchunk *chunk) {
    memchunk *new;

    assert(chunk != NULL);

    /* the right of the chunk is supposed to be empty */
    assert(chunk->next == NULL);

    new = alloc_memchunk();

    chunk->next = new;

    new->prev = chunk;

    return new;
}


/**
 * @brief Gets a single line of input from the terminal
 *
 * Will call realloc to dynamically resize a string to get all the input.
 * Will exit with an error if call to realloc fails. 'start' is a char buffer
 * with length at least 'chunklen + 1'
 *
 * @param start the buffer we want to use to copy the line of text into
 *
 * @return The line of text read in from the terminal
 */
static char *get_line(char *start) {
    char *end = start;
    size_t len = chunklen;
    char *temp;

    assert(start != NULL);

    /* keep reading until a newline is reached */
    while (1) {
        /* read in a chunk's worth of text */
        fgets(end, chunklen, stdin);

        /* check for io errors */
        if (ferror(stdin)) {
            fprintf(stderr, "Input Error: Could not read in line of text\n");
            return NULL;
        }

        /* stop if a newline is found in what we just read in */
        temp = strchr(end, '\n');
        if (temp != NULL) {
            /* clear out the newline */
            *temp = '\0';
            break;
        }

        /* otherwise allocate space for the next chunk to be read in */
        len += chunklen;
        temp = realloc(start, len);
        if (temp == NULL) {
            free(start);
            fprintf(stderr, "Memory Allocation Error: Could not read in line "
                            "of text\n");
            return NULL;
        }
        start = temp;

        /* and update the where to write the chunk to */
        end = start + len - chunklen - 1;
    }

    return start;
}


/**
 * @brief Reads all the text from file into a char buffer
 *
 * Will call realloc to dynamically resize a string to get all the input.
 * Will exit with an error if call to realloc fails. start is a char buffer
 * with length at least 'chunklen + 1'
 *
 * @param file the file we want to read input from
 * @param start the buffer we want to use to copy the file text into
 *
 * @return The text read in from file
 */
static char *get_prog(FILE *file, char *start) {
    char *end = start;
    size_t len = chunklen + 1;
    char *temp;

    assert(file != NULL);
    assert(start != NULL);

    /* read in the entire file */
    while (1) {
        /* read in a chunk's worth of text */
        size_t read = fread(end, 1, chunklen, file);
        end[read] = '\0';

        /* handle errors in the call to fread */
        if (ferror(file)) {
            fprintf(stderr, "Input Error: Could not read in entire file\n");
            return NULL;
        }
        /* stop if everything has been read out from the file */
        if (feof(file))
            break;

        /* otherwise allocate space for the next chunk to be read in */
        len += chunklen;
        temp = realloc(start, len);
        if (start == NULL) {
            free(start);
            fprintf(stderr, "Memory Allocation Error: Could not read in entire "
                            "file\n");
            return NULL;
        }
        start = temp;

        /* and update the place where to write the chunk to */
        end = start + len - chunklen - 1;
    }

    return start;
}


/**
 * @brief Finds the next valid right bracket
 *
 * *line is a left bracket. search to the right to find the next unmatched
 * right bracket
 *
 * @param line the string in which we want to find a matching right bracket
 *
 * @return the address of the right bracket if it exists, NULL otherwise
 */
static char *match_right(char *line) {
    char *c;
    int count = 0;

    assert(line != NULL);
    assert(line[0] == '[');

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

    return NULL;
}


/**
 * @brief Executes a line of brainfuck code
 *
 * line must be a valid brainfuck program. Exits with error if there is
 * a bracket that wants us to jump but there is no matching bracket
 *
 * @param line the brainfuck code we want to execute
 */
static void execute(char *line) {
    char *c;
    char *right;
    jumpstack *stack = NULL;

    /* need to have already initialized our memory and data pointer */
    assert(page != NULL);
    assert(data != NULL);

    /* already reported the error elsewhere, no need to print something here */
    if (line == NULL)
        return;

    /* loop through all characters in the program */
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
                        fprintf(stderr, "Input Error: "
                                        "'[' with no matching ']'\n");
                        return;
                    }
                }
                else if (right != NULL)
                    stack = push_jump(stack, c);
                break;
            case ']':
                if (*data != 0) {
                    if (stack == NULL || stack->leftbracket == NULL) {
                        fprintf(stderr, "Input Error: "
                                        "']' with no matching '['\n");
                        return;
                    }
                    c = stack->leftbracket;
                }
                else
                    stack = pop_jump(stack);
                break;
        }
    }

    destroy_jumpstack(stack);
}
