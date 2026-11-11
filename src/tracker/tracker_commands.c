#include "project_502/tracker_commands.h"
#include "project_502/common.h"
#include "project_502/md5_handler.h"

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
    strncpy: To get a copy string
*/
#include <stdlib.h>
/* stdlib.h Commands Used:
    free
*/
#include <pthread.h>
/* pthread.h Commands/Data Types Used:
    the pthread_mutex_t and PTHREAD_MUTEX_INITIALIZER
    pthread_mutex_lock and pthread_mutex_unlock
*/

// Define the mutex protecting every read and write of the tracker files in the torrents dir
// Static variable, private to this file only
static pthread_mutex_t tracker_mutex = PTHREAD_MUTEX_INITIALIZER;
// when the dead-peer cleanup thread is implemented in another file this mutex would need to be used in that
//  and can be provided using extern ...
// Use the mutex around file access reads and writes, not around send calls (more efficient that way)

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

// Helper function to send every byte of a buffer, handling partial writes
static int send_all(int s, const char *buf, size_t len)
{
    size_t total_sent = 0;
    while (total_sent < len)
    {
        ssize_t bytes_sent = send(s, buf + total_sent, len - total_sent, 0);
        if (bytes_sent <= 0)
        {
            return -1;
        }
        total_sent += (size_t)bytes_sent;
    }
    return 0;
}

// Helper function to check that a filename ends in ".track"
static int has_track_suffix(const char *name)
{
    size_t n = strlen(name);
    size_t suffix_len = strlen(".track");
    return n > suffix_len && strcmp(name + n - suffix_len, ".track") == 0;
}

// Struct for LIST's Response
typedef struct
{
    char filename[256];
    size_t filesize;
    char md5_string[MD5_HEX_LEN]; // Converted from the unsigned char [16] of the binary/byte form, so the final value can be displayed
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

int handle_list_req(int server_worker, const char *shared_directory)
{
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "./%s", shared_directory);

    // Lock mutex to keep updatetracker from updating the file while it reads from it
    pthread_mutex_lock(&tracker_mutex);

    DIR *torrents = opendir(dir_path);
    if (torrents == NULL)
    {
        pthread_mutex_unlock(&tracker_mutex);
        fprintf(stderr, "Unable to open directory: %s\n", shared_directory);
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

        // Skip anything that is not a tracker file (temp files, stray files, etc.)
        if (!has_track_suffix(entry->d_name))
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
                fprintf(stderr, "Could not open file: %s\n", full_path);
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

            // Skip tracker files that are missing the filename or MD5 so the count matches what we send
            if (new_resp.filename[0] == '\0' || new_resp.md5_string[0] == '\0')
            {
                fprintf(stderr, "Skipping malformed tracker file: %s\n", full_path);
                continue;
            }

            // Append the RESP_LIST struct to its array
            if (!add_resp(&resp_arr, &count, &capacity, new_resp))
            {
                fprintf(stderr, "Memory allocation failed.\n");
                pthread_mutex_unlock(&tracker_mutex);
                free(resp_arr);
                closedir(torrents);
                return -1;
            }
        }
    }

    closedir(torrents); // Close the directory stream

    pthread_mutex_unlock(&tracker_mutex);

    // Then write the contents of the RESP_LIST array to the peer
    char send_buf[512];

    // Define the first part of the response (<REP LIST X>\n)
    int len = snprintf(send_buf, sizeof(send_buf), "<REP LIST %zu>\n", count);
    if (send_all(server_worker, send_buf, (size_t)len) < 0)
    {
        perror("Failed to send LIST header");
        free(resp_arr);
        return -1;
    }

    // Array indexes start at 0, but the line numbers sent to the peer start at 1
    for (size_t i = 0; i < count; i++)
    {
        // Format the line for the current file
        int len = snprintf(send_buf, sizeof(send_buf), "<%zu %s %zu %s>\n", i + 1, resp_arr[i].filename, resp_arr[i].filesize, resp_arr[i].md5_string);

        // The count was already sent, so a skipped line would leave the peer waiting; fail instead
        if (len < 0 || (size_t)len >= sizeof(send_buf))
        {
            fprintf(stderr, "Formatting error or buffer overflow for %s\n", resp_arr[i].filename);
            free(resp_arr);
            return -1;
        }

        // Send the formatted string over the socket (handle partial writes)
        if (send_all(server_worker, send_buf, (size_t)len) < 0)
        {
            perror("Failed to send LIST entry");
            free(resp_arr);
            return -1;
        }
    }

    // Send the final line
    len = snprintf(send_buf, sizeof(send_buf), "<REP LIST END>\n");
    if (send_all(server_worker, send_buf, (size_t)len) < 0)
    {
        perror("Failed to send LIST end");
        free(resp_arr);
        return -1;
    }

    // Free the heap memory
    free(resp_arr);
    return 0;
}

char *xtract_fname(const char *read_msg, char *fname, size_t out_size)
{
    char copy[MAXLINE];
    char *tok;
    char *saveptr = NULL; // strtok_r keeps its state here, so the threads do not share it
    int len;

    strncpy(copy, read_msg, MAXLINE - 1);
    copy[MAXLINE - 1] = '\0';

    /* strip trailing newline, carriage return, spaces and the closing '>' */
    len = strlen(copy);
    while (len > 0 && (copy[len - 1] == '\n' || copy[len - 1] == '\r' ||
                       copy[len - 1] == ' ' || copy[len - 1] == '>'))
        copy[--len] = '\0';

    tok = strtok_r(copy, " ", &saveptr); // To get '<GET' before filename
    tok = strtok_r(NULL, " ", &saveptr); // To get second token, the filename.track

    if (tok == NULL || strlen(tok) >= out_size)
        return NULL; /* missing or too long */
    if (strchr(tok, '/') != NULL || strstr(tok, "..") != NULL)
        return NULL; /* path traversal attempt */
    if (!has_track_suffix(tok))
        return NULL; /* GET must ask for a .track file */

    strcpy(fname, tok);
    return fname;
}

int handle_get_req(int server_worker, const char *fname, const char *shared_directory)
{
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "./%s", shared_directory);

    // Lock mutex to keep updatetracker from updating the file while it reads from it
    pthread_mutex_lock(&tracker_mutex);

    DIR *torrents = opendir(dir_path);
    if (torrents == NULL)
    {
        pthread_mutex_unlock(&tracker_mutex);
        fprintf(stderr, "Unable to open directory: %s\n", shared_directory);
        return -1;
    }

    struct dirent *entry;
    char full_path[1024];
    struct stat path_stat;
    char *buffer = NULL; // Stays NULL until the tracker file is read, so we can tell if it was found
    long size = 0;
    char md5[MD5_HEX_LEN]; // MD5 of the tracker file content as 32 hex chars + null terminator

    while ((entry = readdir(torrents)) != NULL)
    {
        // Skip "." and "..", just to be safe
        // These shouldn't be any in the directory
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        // check if its the correct name
        if (strcmp(entry->d_name, fname) == 0)
        {
            // Build the full file path
            // readdir only gives bare filename
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

            // Check if entry is a regular file (skip subfolders)
            if (stat(full_path, &path_stat) == 0 && S_ISREG(path_stat.st_mode))
            {
                // Open the tracker file; the MD5 sent back is computed from this content, not read from the file
                FILE *file = fopen(full_path, "rb");
                if (file == NULL)
                {
                    fprintf(stderr, "Could not open file: %s\n", full_path);
                    break;
                }

                // Move to the end of the file to get its size
                fseek(file, 0L, SEEK_END);
                size = ftell(file);
                rewind(file);
                if (size < 0)
                {
                    fclose(file);
                    break;
                }

                // Allocate memory for the content + null terminator
                buffer = malloc(size + 1);
                if (!buffer)
                {
                    fclose(file);
                    break;
                }

                // Read the file into the buffer
                size_t read_bytes = fread(buffer, 1, size, file);
                if (read_bytes != (size_t)size)
                {
                    free(buffer);
                    buffer = NULL; // So the check below sees a failed read
                    fclose(file);
                    break;
                }

                buffer[size] = '\0'; // Null-terminate the string

                fclose(file);
                break;
            }
        }
    }

    closedir(torrents); // Close the directory stream, every path above ends up here

    // Now done with the files, so free the mutex
    pthread_mutex_unlock(&tracker_mutex);

    // File was not found or could not be read
    if (buffer == NULL)
    {
        fprintf(stderr, "GET: could not read tracker file %s\n", fname);
        return -1;
    }

    // MD5 of the tracker file content, the peer checks the received data against this
    if (compute_md5_hex((unsigned char *)buffer, (size_t)size, md5) != 0)
    {
        fprintf(stderr, "GET: could not compute MD5 for %s\n", fname);
        free(buffer);
        return -1;
    }

    // Then write the contents of the file to the peer
    char end_line[64];

    // Define the first part of the response (<REP GET BEGIN>\n)
    const char *begin_line = "<REP GET BEGIN>\n";
    if (send_all(server_worker, begin_line, strlen(begin_line)) < 0)
    {
        perror("Failed to send GET begin");
        free(buffer);
        return -1;
    }

    // Send the tracker file content as is (handle partial writes)
    if (send_all(server_worker, "<", 1) < 0)
    {
        perror("Failed to send GET content start");
        free(buffer);
        return -1;
    }

    // Send the tracker file content as is (handle partial writes)
    if (send_all(server_worker, buffer, (size_t)size) < 0)
    {
        perror("Failed to send GET content");
        free(buffer);
        return -1;
    }

    // Close the wrapper, the newline also makes sure the END line starts on its own line
    if (send_all(server_worker, ">\n", 2) < 0)
    {
        perror("Failed to send GET content end");
        free(buffer);
        return -1;
    }

    // Send the final line
    int len = snprintf(end_line, sizeof(end_line), "<REP GET END %s>\n", md5);
    if (send_all(server_worker, end_line, (size_t)len) < 0)
    {
        perror("Failed to send GET end");
        free(buffer);
        return -1;
    }

    // Free the buffer
    free(buffer);

    return 0;
}

CreateCommand tokenize_createmsg(const char *read_msg)
{
    CreateCommand command_args = {0};

    return command_args;
}

void handle_createtracker_req(int server_worker, CreateCommand command)
{
}

UpdateCommand tokenize_updatemsg(const char *read_msg)
{
    UpdateCommand command_args = {0};

    return command_args;
}

void handle_updatetracker_req(int server_worker, UpdateCommand command)
{
}
