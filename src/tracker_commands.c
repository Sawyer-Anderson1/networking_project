#include "project_502/tracker_commands.h"

#include <dirent.h>
/* dirent.h Commands or Data Types Used:
    opendir(): used to open the torrents directory
    DIR: Directory variable
    struct dirent: Directory entry variable
    readdir(): Used to read the directories
    closedir();
*/
#include <stdio.h>
/* stdio.h Commands Used:
    fprintf: Used for outputing failures specifically to stderr
    snprintf: Used to build path string
    fopen(): Used for reading the file contents
    fgets(): Used to read lines
*/
#include <sys/stat.h>
/* sys/stat.h Commands or Data Types Used:
    struct stat: Stores metadata on files
    S_ISREG: Used to check for regular files
*/
#include <ctype.h>
/* ctype.h Commands used:
    isspace()
*/
#include <string.h>
/* string.h Commands used:
    strncmp: used to check for filename, filesize, and MD5 fields
    strlen: Used to get the number of chars in a field
*/
#include <stdlib.h>
/* stdlib.h Commands Used:
    free
*/
// Static helper functions
static char *trim_whitespace(char *str)
{
    // Skip past any leading spaces, tabs, and newlines
    while (isspace((unsigned char)*str))
    {
        str++;
    }

    // If the string was all whitespace return early
    if (*str == '\0')
    {
        return str;
    }

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
    {
        end--;
    }
    *(end + 1) = '\0';

    return str;
}

// Struct for LIST's Response
typedef struct
{
    char filename[256];
    size_t filesize;
    char md5_string[33]; // Converted from the unsigned char [16] of the binary/byte form, so the final value can be displayed
} RESP_LIST;

// Helper function to append a new file information to the LIST Response array
int add_resp(RESP_LIST **resp_arr, size_t *count, size_t *capacity, RESP_LIST new_resp)
{
    // Grow the array if it is full
    if (*count == *capacity)
    {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        RESP_LIST *temp = realloc(*resp_arr, new_capacity * sizeof(RESP_LIST));
        if (temp == NULL)
        {
            return 0; // Allocation failed; original *roster remains intact
        }
        *resp_arr = temp;
        *capacity = new_capacity;
    }

    (*resp_arr)[*count] = new_resp;
    (*count)++;
    return 1; // Success
}

int handle_list_req(int server_worker, const char shared_directory)
{
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "./%s", shared_directory);

    DIR *torrents = opendir(dir_path);
    if (torrents == NULL)
    {
        fprintf(stderr, "Unable to open directory: %s", shared_directory);
        return -1;
    }

    struct dirent *entry;
    char full_path[1024];
    struct stat path_stat;

    // We keep an array of the contents first then afterwards we stream from the array of X
    RESP_LIST *resp_arr = NULL;
    size_t count = 0;
    size_t capacity = 0;

    while ((entry = readdir(torrents)) != NULL)
    {
        // Skip "." and "..", just to be safe
        // These shouldn't be any in the directory
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        // Build the full file path
        // readdir only gives bare filename
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        // Check if entry is a regular file (skip subfolders)
        if (stat(full_path, &path_stat) == 0 && S_ISREG(path_stat.st_mode))
        {
            printf("Reading: %s\n", full_path);

            // Open and read the file for its filesize and MD5, maybe I should also get the filename from here just to be safe
            FILE *file = fopen(full_path, "r");
            if (file == NULL)
            {
                fprintf(stderr, "Could not open file: %s", full_path);
                continue;
            }

            char line[256];

            // Declare the new RESP_LIST to be built
            RESP_LIST new_resp = {0};
            while (fgets(line, sizeof(line), file) != NULL)
            {
                // First trim outer and leading whitespaces first
                char *trimmed_line = trim_whitespace(line);

                // Skip empty lines or comment lines with '#'
                if (trimmed_line[0] == '\0' || trimmed_line[0] == '#')
                {
                    continue;
                }

                // Parse for filename, filesize, and MD5
                if (strncmp(trimmed_line, "Filename:", 9) == 0)
                {
                    // Jump past the field name and trim any spaces before the actual value
                    char *f_name = trim_whitespace(trimmed_line + 9);

                    // Add the filename to new_resp
                    snprintf(new_resp.filename, sizeof(new_resp.filename), "%s", f_name);
                }
                else if (strncmp(trimmed_line, "Filesize:", 9) == 0)
                {
                    // Jump past the field name and trim any spaces before the actual value
                    char *filesize_str = trim_whitespace(trimmed_line + 9);
                    // Convert string to size_t
                    new_resp.filesize = (size_t)strtoull(filesize_str, NULL, 10);
                }
                else if (strncmp(trimmed_line, "MD5:", 4) == 0)
                {
                    // Jump past the field name and trim any spaces before the actual value
                    char *md5 = trim_whitespace(trimmed_line + 4);
                    snprintf(new_resp.md5_string, sizeof(new_resp.md5_string), "%s", md5);
                }
            }

            fclose(file);

            // Append the RESP_LIST struct to its array
            if (!add_resp(&resp_arr, &count, &capacity, new_resp))
            {
                fprintf(stderr, "Memory allocation failed.\n");
                free(resp_arr);
                closedir(torrents);
                return -1;
            }
        }
    }

    closedir(torrents); // Close the directory stream

    // Free the heap memory
    free(resp_arr);
    return 0;
}

char xtract_fname(char read_msg)
{
}

void handle_get_req(int server_worker, char fname)
{
}

CreateCommand tokenize_createmsg(char read_msg)
{
}

void handle_createtracker_req(int server_worker, CreateCommand command)
{
}

UpdateCommand tokenize_updatemsg(char read_msg)
{
}

void handle_updatetracker_req(int server_worker, UpdateCommand command)
{
}
