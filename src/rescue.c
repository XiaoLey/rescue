

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "deflate.h"

#ifndef RESCUE_BOOTSTRAP
#define rescue_header_only
#include "resources.c"
#endif

#if defined(__OS2__) || defined(__WINDOWS__) || defined(WIN32) || defined(WIN64) || defined(_MSC_VER)
#include <windows.h>
#define PATH_DELIMITER '\\'
#define IS_PATH_DELIMITER(C) ((C) == '\\' || (C) == '/')
#define PWD(B, L) GetCurrentDirectory(L, B)
#define ABSOLUTE_PATH(R, A, L) GetFullPathName(R, L, A, NULL)
#else
#include <unistd.h>
#define PATH_DELIMITER '/'
#define IS_PATH_DELIMITER(C) ((C) == '/')
#define PWD(B, L) getcwd(B, L)
#define ABSOLUTE_PATH(R, A, L) realpath(R, A)
#endif

#define BUFFER_SIZE 128
#define DEFAULT_IDENTIFIER "rescue"
#define PLACEHOLDER "__RESCUE"
#define LINE_WIDTH 80
#define STRING_LENGTH (1024)
#define SIZE_THRESHOLD 1024
#undef MAX_PATH
#define MAX_PATH 2048

#ifdef RESCUE_BOOTSTRAP
#define BOOTSTRAP_WRITE(R, C, D) {char* path; path_join(RESCUE_BOOTSTRAP, R, &path); write_file(path, C, D); free(path);}
#endif

#define PING {fprintf(stderr, "%s(%d): PING\n", __FILE__, __LINE__); }

typedef struct buffer_node {
    char* buffer;
    size_t size;
    struct buffer_node* next;
} buffer_node;

typedef struct compression_data {
    buffer_node *out;
    int line;
    int segment;
    size_t total;
} compression_data;

typedef struct source_data {
    FILE* out;
    char* placeholder;
    char* identifier;
    int state;
} source_data;

typedef struct resource_data {
    size_t inflated;
    size_t deflated;
    int metadata;
} resource_data;

typedef int (*data_callback)(const void* buffer, size_t len, void *user);

buffer_node *buffer_node_head_create()
{
    buffer_node *head = (buffer_node*)malloc(sizeof(buffer_node));
    if(!head) return NULL;
    head->buffer = NULL;
    head->size = 0;
    head->next = head;
    return head;
}

void buffer_node_destroy(buffer_node *head)
{
    while (head->next != head) {
        buffer_node *tmp = head->next;
        head->next = tmp->next;
        free(tmp->buffer);
        free(tmp);
    }

    free(head);
}

buffer_node *buffer_node_append(buffer_node *prev)
{
    buffer_node *new_node = (buffer_node*)malloc(sizeof(buffer_node));
    new_node->buffer = NULL;
    new_node->size = 0;
    new_node->next = prev->next;
    prev->next = new_node;
    return new_node;
}

int path_join(const char* root, const char* path, char** out) {

    size_t rlen = strlen(root);
    size_t plen = strlen(root);

    *out = (char*) malloc(sizeof(char) * (rlen + plen + 2));

    if (IS_PATH_DELIMITER(root[rlen - 1]))
    {
        strcpy(*out, root);
        strcpy(&((*out)[rlen]), path);
    } else if (IS_PATH_DELIMITER(path[0]))
    {
        strcpy(*out, path);
    }
    else {
        strcpy(*out, root);
        (*out)[rlen] = PATH_DELIMITER;
        strcpy(&((*out)[rlen + 1]), path);
    }

    return 1;

}

int path_split(const char* path, char** parent, char** name) {
    size_t i;
    size_t len = strlen(path);

    if (len < 2)
        return 0;

    for (i = len - 1; 1; i--)
    {
        if (IS_PATH_DELIMITER(path[i]))
        {
            if (parent) { *parent = (char*) malloc(sizeof(char) * i); memcpy(*parent, path, i - 1); (*parent)[i - 1] = 0; }
            if (name) { *name = (char*) malloc(sizeof(char) * (len - i)); memcpy(*name, &(path[i + 1]), len - i - 1); (*name)[len - i - 1] = 0; }
            return 1;
        }

        if(i == 0) break;
    }

    if (name)
    {
        *name = (char*) malloc(sizeof(char) * (len + 1));
        memcpy(*name, path, len);
        (*name)[len] = 0;
    }

    return 0;
}

mz_bool compression_callback(const void* data, int len, void *user)
{
    int i;
    char oct_buf_tmp[5];
    const char* buffer = (const char*) data;
    compression_data* env = (compression_data*) user;
    size_t l = env->total;
    size_t offset = 0;

    buffer_node *cur_node = env->out;

    cur_node->buffer = calloc(len * (2 + 4 + 2), sizeof(char)); // 2 for '\n\"', 4 for octal, 2 for '\"'

    for (i = 0; i < len; i++)
    {
        if (env->line == 0){
            memcpy(cur_node->buffer + offset, "\n\"", 2);
            offset += 2;
        }

        if (buffer[i] < 32 || buffer[i] == '"' || buffer[i] == '\\' || buffer[i] > 126) {
            sprintf(oct_buf_tmp, "\\%03o", (unsigned char)buffer[i]);
            memcpy(cur_node->buffer + offset, oct_buf_tmp, 4);
            offset += 4;
            env->line += 4;
        } else if (i > 0 && buffer[i - 1] == '?' && buffer[i] == '?') { // Avoiding trigraph warnings
            memcpy(cur_node->buffer + offset, "\\?", 2);
            offset += 2;
            env->line += 2;
        } else {
            cur_node->buffer[offset++] = buffer[i];
            env->line++;
        }

        if (l > 0 && ((i + l + 1) % STRING_LENGTH == 0)) {
            memcpy(cur_node->buffer + offset, "\",", 2);
            offset += 2;
            env->line = 0;
        } else if (env->line >= LINE_WIDTH) {
            memcpy(cur_node->buffer + offset++, "\"", 1);
            env->line = 0;
        }
    }

    cur_node->size = offset;
    env->total += len;
    return 1;
}

static size_t generate_resource_sub_fun(FILE *res_file, buffer_node *head, compression_data *cenv, int is_compress)
{
    tdefl_compressor compressor;
    char buffer[BUFFER_SIZE];
    size_t length = 0;

    cenv->out = head;
    cenv->line = 0;
    cenv->total = 0;

    if(is_compress)
        tdefl_init(&compressor, &compression_callback, cenv, TDEFL_MAX_PROBES_MASK);

    buffer_node *node = head;
    while (1) {
        size_t n = fread (buffer, sizeof(char), BUFFER_SIZE, res_file);
        if (n < 1) break;

        node = buffer_node_append(node);
        cenv->out = node;

        if(is_compress)
            tdefl_compress_buffer(&compressor, buffer, n, TDEFL_SYNC_FLUSH);
        else
            compression_callback(buffer, (int)n, cenv);

        length += n;

        if (n < BUFFER_SIZE) break;
    }

    if(is_compress){
        node = buffer_node_append(node);
        cenv->out = node;
        tdefl_compress_buffer(&compressor, NULL, 0, TDEFL_FINISH);
    }

    return length;
}

resource_data generate_resource(const char* filename, FILE* out)
{
    resource_data result;
    compression_data cenv;
    FILE* fp = fopen(filename, "rb");
    size_t length;
    char postfix[5] = {0};

    buffer_node *buf_head = buffer_node_head_create();

    if (!fp) {
        buffer_node_destroy(buf_head);
        result.inflated = -1;
        result.deflated = -1;
        return result;
    }

    length = generate_resource_sub_fun(fp, buf_head, &cenv, 1);

    if (length == 0) {
        result.metadata = 0;
    }
    else if (length <= cenv.total + SIZE_THRESHOLD) {
        buffer_node_destroy(buf_head);
        buf_head = buffer_node_head_create();
        fseek(fp, 0, SEEK_SET);
        length = generate_resource_sub_fun(fp, buf_head, &cenv, 0);
        if(length != cenv.total) {
            printf("Error: original size mismatch. (total: %zu, length: %zu)\n", cenv.total, length);
            buffer_node_destroy(buf_head);
            fclose(fp);
            result.inflated = -1;
            result.deflated = -1;
            return result;
        }

        result.metadata = 0;
    }
    else {
        result.metadata = 1;
    }

    result.inflated = length;
    result.deflated = cenv.total;

    if(cenv.line < LINE_WIDTH && cenv.line != 0)
        sprintf(postfix, "\"");

    if ((cenv.total) % STRING_LENGTH != 0)
        sprintf(postfix, "%s,\n", postfix);

    buffer_node *node = buf_head->next;
    while (node != buf_head) {
        fwrite(node->buffer, sizeof(char), node->size, out);
        node = node->next;
    }

    fwrite(postfix, sizeof(char), strlen(postfix), out);

    buffer_node_destroy(buf_head);
    fclose(fp);
    return result;
}

int source_callback(const void* data, size_t len, void *user)
{
    source_data* env = (source_data*) user;
    const char* buffer = (const char*) data;
    int i = 0;

    // Copy the buffer to output and look for placeholder, replace it with data
    while (i < len)
    {
        // We are already recognizing placeholder
        if (env->state > 0) {
            if (buffer[i] == env->placeholder[env->state])
            {
                env->state++;
            } else {
                if (env->placeholder[env->state] == 0) // Placeholder recognized, insert data
                {

                    env->state = 0;

                    fprintf(env->out, "%s", env->identifier);
                    fputc(buffer[i], env->out);
                } else { // Not a placeholder, abort
                    int j;

                    for (j = 0; j < env->state; j++)
                        fputc(env->placeholder[j], env->out);

                    env->state = 0;

                    fputc(buffer[i], env->out);
                }

            }
        } else {
            if (buffer[i] == env->placeholder[0]) // Enter placeholder seek mode, pause output
            {
                env->state = 1;
            } else { // Output normally
                fputc(buffer[i], env->out);
            }
        }
        i++;
    }

    return 1;
}

int write_file(const char* source, data_callback callback, void* user)
{
    char buffer[BUFFER_SIZE];
    FILE* fp = fopen(source, "rb");
    if (!fp)
        return 0;
    while (1) {
        size_t n = fread (buffer, sizeof(char), BUFFER_SIZE, fp);
        if (n < 1) break;
        callback(buffer, n, user);
        if (n < BUFFER_SIZE) break;
    }
    fclose(fp);
    return 1;
}

int help()
{

    fprintf(stdout, "rescue - A cross-platform resource compiler.\n\n");
    fprintf(stdout, "Usage: rescue [-h] [-v] [-o <path>] [-a] [-b] [-r <path>] [-p <prefix>] <file1> ...\n");
    fprintf(stdout, " -h\t\tPrint help.\n");
    fprintf(stdout, " -v\t\tBe verbose.\n");
    fprintf(stdout, " -o <path>\tOutput the resulting C source to the given file instead of printing it to standard output.\n\t\tThis flag can only be used before any source file is provided.\n");
    fprintf(stdout, " -r <path>\tSet the root directory for the following files.\n\t\tThe embedded names of the files will be relative to this path.\n");
    fprintf(stdout, " -a\t\tSet the naming mode of the files to absolute name.\n\t\tThe embedded names of the files will include the full absolute name of the file.\n");
    fprintf(stdout, " -b\t\tSet the naming mode of the files to file basename.\n\t\tThe embedded names of the files will include only the basename of the file.\n");
    fprintf(stdout, " -p <prefix>\tUse the following alphanumerical string as a prefix for the functions and\n\t\tvariables in the generated file (instead of `rescue`).\n\t\tThis flag can only be used before any source file is provided.\n");
    fprintf(stdout, "\n");
    return 0;
}

#define NAMING_MODE_ABSOLUTE 0
#define NAMING_MODE_RELATIVE 1
#define NAMING_MODE_BASENAME 2

#define MAX_IDENTIFIER 64

#define VERBOSE(...) if (verbose) { fprintf(stdout, __VA_ARGS__); }

int main(int argc, char** argv)
{
    int i;
    FILE* out = stdout;
    char root[MAX_PATH];
    char identifier[MAX_IDENTIFIER];
    int processed_files = 0;
    int naming_mode = NAMING_MODE_BASENAME;
    source_data ctx;
    int verbose = 0;

    char** resource_names = (char**) malloc(sizeof(char*) * argc);
    int* resource_metadata = (int*) malloc(sizeof(int) * argc);
    size_t* resource_length_inflated = (size_t*) malloc(sizeof(size_t) * argc);
    size_t* resource_length_deflated = (size_t*) malloc(sizeof(size_t) * argc);

    PWD(root, MAX_PATH); // Get the current directory
    strcpy(identifier, DEFAULT_IDENTIFIER);

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0)
        {

            if (argc == 2) help();
            continue;

        } else if (strcmp(argv[i], "-o") == 0)
        {
            if (out != stdout)
            {
                fprintf(stderr, "Output already set.\n");
                continue;
            }

            if (processed_files > 0)
            {
                fprintf(stderr, "Output already set.\n");
                continue;
            }

            out = fopen(argv[++i], "wb");

            VERBOSE("Writing to file %s.\n", argv[i]);

            continue;

        } else if (strcmp(argv[i], "-v") == 0)
        {

            verbose = 1;

            continue;

        } else if (strcmp(argv[i], "-b") == 0)
        {

            naming_mode = NAMING_MODE_BASENAME;

            continue;

        } else if (strcmp(argv[i], "-a") == 0)
        {

            naming_mode = NAMING_MODE_ABSOLUTE;

            continue;

        } else if (strcmp(argv[i], "-r") == 0)
        {

            if ((i + 1) == argc)
            {
                fprintf(stderr, "Missing directory.\n");
                continue;
            }

            strcpy(root, argv[++i]);
            naming_mode = NAMING_MODE_RELATIVE;

            continue;

        } else if (strcmp(argv[i], "-p") == 0)
        {

            if ((i + 1) == argc)
            {
                fprintf(stderr, "Missing identifier.\n");
                continue;
            }

            if (processed_files > 0)
            {
                fprintf(stderr, "Output has already started.\n");
                continue;
            }

            strcpy(identifier, argv[++i]);

            continue;
        }

        ctx.state = 0;
        ctx.out = out;
        ctx.placeholder = PLACEHOLDER;
        ctx.identifier = identifier;

        // copy resource

        if (processed_files == 0)
        {
#ifndef RESCUE_BOOTSTRAP
            rescue_get_resource("inflate.c", &source_callback, &ctx);
#else
            BOOTSTRAP_WRITE("inflate.c", &source_callback, &ctx);
#endif
            fprintf(out, "#ifndef %s_header_only\n", identifier);

        }

        {
            fprintf(out, "static const char* %s_resource_data_%d[] = {", identifier, processed_files);

            VERBOSE("Generating resource from %s.\n", argv[i]);
            resource_data r = generate_resource(argv[i], out);
            fprintf(out, " 0};\n");


            if (r.deflated == -1)
            {
                fprintf(stderr, "File %s does not exist or cannot be opened for reading, skipping.\n", argv[i]);
                continue;
            } else
            {
                resource_length_inflated[processed_files] = r.inflated;
                resource_length_deflated[processed_files] = r.deflated;
                resource_metadata[processed_files] = r.metadata;
                switch (naming_mode)
                {
                case NAMING_MODE_BASENAME:
                {
                    path_split(argv[i], NULL, &resource_names[processed_files]);
                    break;
                }
                case NAMING_MODE_RELATIVE:
                {
                    char * basename = NULL;
                    path_split(argv[i], NULL, &basename);
                    char * relpath = (char *)malloc(sizeof(char) * strlen(basename) + strlen(argv[i]) + 1);
                    // sprintf(relpath, "%s%s%s", root, PATH_DELIMITER == '\\' ? "\\\\" : "/", basename);
                    sprintf(relpath, "%s%c%s", root, '/', basename); // universal path delimiter
                    resource_names[processed_files] = relpath;
                    free(basename);
                    break;
                }
                case NAMING_MODE_ABSOLUTE:
                {
                    char* abspath = (char*) malloc(sizeof(char) * MAX_PATH);
                    ABSOLUTE_PATH(argv[i], abspath, MAX_PATH);
                    resource_names[processed_files] = abspath;
                    break;
                }
                }

            }

        }

        processed_files++;
    }


    if (processed_files > 0)
    {
        int f;

        fprintf(out, "static const char** %s_resource_data[] = {", identifier);
        for (f = 0; f < processed_files; f++)
            fprintf(out, "%s_resource_data_%d,", identifier, f);
        fprintf(out, " 0};\n");

        fprintf(out, "static const char* %s_resource_names[] = {\n", identifier);
        for (f = 0; f < processed_files; f++)
            fprintf(out, "\"%s\",", resource_names[f]);
        fprintf(out, " 0};\n");

        fprintf(out, "static const int %s_resource_metadata[] = {\n", identifier);
        for (f = 0; f < processed_files; f++)
            fprintf(out, "%d,", resource_metadata[f]);
        fprintf(out, " 0};\n");

        fprintf(out, "static const size_t %s_resource_length_inflated[] = {\n", identifier);
        for (f = 0; f < processed_files; f++)
            fprintf(out, "%zu,", resource_length_inflated[f]);
        fprintf(out, " 0};\n");

        fprintf(out, "static const size_t %s_resource_length_deflated[] = {\n", identifier);
        for (f = 0; f < processed_files; f++)
            fprintf(out, "%zu,", resource_length_deflated[f]);
        fprintf(out, " 0};\n");

        free(resource_length_inflated);
        free(resource_length_deflated);
        free(resource_metadata);

        for (f = 0; f < processed_files; f++)
            free(resource_names[f]);
        free(resource_names);

        fprintf(out, "#define %s_SEGMENT_LENGTH (%d)\n", identifier, STRING_LENGTH),

        fprintf(out, "#endif\n");

#ifndef RESCUE_BOOTSTRAP
        rescue_get_resource("template.c", &source_callback, &ctx);
#else
        BOOTSTRAP_WRITE("template.c", &source_callback, &ctx);
#endif

    }

    if (argc < 2) {
        help();
        fprintf(stderr, "No input given.\n");
        return -1;
    }

    fflush(out);

    return 0;

}
