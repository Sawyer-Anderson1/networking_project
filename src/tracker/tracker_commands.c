#include "project_502/tracker_commands.h"
#include "project_502/common.h"
#include "project_502/md5_handler.h"
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

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
    char ip_string[INET_ADDRSTRLEN] = {0};
    unsigned int port = 0;

    int fields = sscanf(read_msg, " <createtracker %255s %zu %1023s %32s %15s %u", command_args.filename, &command_args.filesize, command_args.description, command_args.md5_string, ip_string, &port);
    if (fields != 6)
    {
        fprintf(stderr, "Invalid createtracker request");
        return command_args;
    }

    if (inet_pton(AF_INET, ip_string, &command_args.peer_ip) != 1 || port == 0 || port > 65535)
    {
        fprintf(stderr, "Invalid IP addres or port\n");
        return command_args;
    }
    command_args.peer_port = (unsigned short int)port;
    command_args.valid = 1;
    return command_args;
}

void handle_createtracker_req(int server_worker, CreateCommand command, const char *shared_directory)
{
    // Reject invalid requests
    if (!command.valid || command.filename[0] == '\0' ||
        strchr(command.filename, '/') != NULL ||
        strcmp(command.filename, ".") == 0 ||
        strcmp(command.filename, "..") == 0)
        {
            send_all(server_worker, "<createtracker fail>\n", strlen("<createtracker fail>\n"));
            return;
        }

        // Build the path to new tracker file
        char filepath[1024];
        int length = snprintf(filepath, sizeof(filepath), "%s/%s.track", shared_directory, command.filename);
        if (length < 0 || (size_t)length >= sizeof(filepath))
        {
            send_all(server_worker, "<createtracker fail>\n", strlen("<createtracker fail>\n"));
            return;
        }

        //Convert peer IP atddress into readable text
        char ip_string[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET, &command.peer_ip, ip_string, sizeof(ip_string)) == NULL)
        {
            send_all(server_worker, "<createtracker fail>\n", strlen("<createtracker fail>\n"));
            return;
        }

        pthread_mutex_lock(&tracker_mutex);

        FILE *file = fopen(filepath, "wx");

        if (file == NULL)
        {
            int already_exists = (errno == EEXIST);
            pthread_mutex_unlock(&tracker_mutex);

            const char *response = already_exists
                ? "<createtracker ferr>\n"
                : "<createtracker fail>\n";

                send_all(server_worker, response, strlen(response));
                return;
        }

        int result = fprintf(file, 
                            "Filename: %s\n"
                            "Filesize: %zu\n"
                            "Description: %s\n"
                            "MD5: %s\n"
                            "#list of peers follows next\n"
                            "%s:%u:0:%zu:%lld\n",
                            command.filename, command.filesize, command.description, command.md5_string, ip_string, (unsigned int)command.peer_port, command.filesize, (long long)time(NULL));
        int close_result = fclose(file);

        if (result < 0 || close_result != 0)
        {
            remove(filepath);
            pthread_mutex_unlock(&tracker_mutex);
            send_all(server_worker, "<createtracker fail>\n", strlen("<createtracker fail>\n"));
            return;
        }

        pthread_mutex_unlock(&tracker_mutex);

        send_all(server_worker, "<createtracker succ>\n", strlen("<createtracker succ>\n"));

        printf("created tracker: %s\n", filepath);







}

UpdateCommand tokenize_updatemsg(const char *read_msg)
{
    UpdateCommand command_args = {0};

    char ip_string[INET_ADDRSTRLEN];
    unsigned int port;

    int fields = sscanf(read_msg, " <updatetracker %255s %lu %lu %15s %u", command_args.filename, &command_args.start_byte, &command_args.end_byte, ip_string, &port);

    if (fields != 5) 
    {
        printf("Invalid update request\n");
        return command_args;
    }

    if (inet_pton(AF_INET, ip_string, &command_args.peer_ip) != 1 || port == 0 || port > 65535)
    {
        printf("Invalid IP or port\n");
        return command_args;
    }

    command_args.peer_port = (unsigned short int)port;
    command_args.valid = 1;
    return command_args;
}

void handle_updatetracker_req(int server_worker, UpdateCommand command, const char *shared_directory)
{
    char filepath[1024];
    char response[512];

    // Check whether the request is valid.
    if (!command.valid || command.filename[0] == '\0' || strchr(command.filename, '/') != NULL || strcmp(command.filename, ".") == 0 || strcmp(command.filename, "..") == 0) 
    {
        snprintf(response, sizeof(response), "<updatetracker %s fail>\n", command.filename);
        send_all(server_worker, response, strlen(response));
        return;
    }

    // Build the path to the tracker file.
    int length = snprintf(filepath, sizeof(filepath), "%s/%s.track", shared_directory, command.filename);

    if (length < 0 || (size_t)length >= sizeof(filepath)) 
    {
        snprintf(response, sizeof(response), "<updatetracker %s fail>\n", command.filename);
        send_all(server_worker, response, strlen(response));
        return;
    }

    // Open the existing tracker file for reading.
    pthread_mutex_lock(&tracker_mutex);

    FILE *file = fopen(filepath, "r");

    if (file == NULL) 
    {
        int missing = (errno == ENOENT);

        pthread_mutex_unlock(&tracker_mutex);

        snprintf(response, sizeof(response), "<updatetracker %s %s>\n", command.filename, missing ? "ferr" : "fail");
        send_all(server_worker, response, strlen(response));
        return;
    }

 // Create a temporary file to hold the updated information.
    char temp_path[1100];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", filepath);

    FILE *temp = fopen(temp_path, "w");

    if (temp == NULL) 
    {
        fclose(file);
        pthread_mutex_unlock(&tracker_mutex);

        snprintf(response, sizeof(response), "<updatetracker %s fail>\n", command.filename);
        send_all(server_worker, response, strlen(response));
        return;
    }

    // Convert the peer's IP address into readable text.
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &command.peer_ip, ip, sizeof(ip));

    char line[2048];
    int found = 0;
    int error = 0;

    // Read the tracker file one line at a time.
    while (fgets(line, sizeof(line), file) != NULL) {
        char old_ip[INET_ADDRSTRLEN];
        unsigned int old_port;
        unsigned long long start, end;
        long long timestamp;

        // Check whether this line contains peer information.
        int fields = sscanf(line, "%15[^:]:%u:%llu:%llu:%lld", old_ip, &old_port, &start, &end, &timestamp);

        if (fields == 5 && strcmp(old_ip, ip) == 0 && old_port == command.peer_port) 
        {
            // Existing peer: replace its byte range and timestamp.
            if (fprintf(temp, "%s:%u:%llu:%llu:%lld\n", ip, (unsigned int)command.peer_port, (unsigned long long)command.start_byte, (unsigned long long)command.end_byte, (long long)time(NULL)) < 0) 
            {
                error = 1;
            }

            found = 1;
        } 
        else 
        {
            // Keep all other lines unchanged.
            if (fputs(line, temp) == EOF)
            {
                error = 1;
            }
        }
    }

    // New peer: append its information.
    if (!found && !error) 
    {
        if (fprintf(temp, "%s:%u:%llu:%llu:%lld\n", ip, (unsigned int)command.peer_port, (unsigned long long)command.start_byte, (unsigned long long)command.end_byte, (long long)time(NULL)) < 0) 
        {
            error = 1;
        }
    }

    if (ferror(file))
    {
        error = 1;
    }

    fclose(file);

    if (fclose(temp) != 0)
    {
        error = 1;
    }

    // Replace the old file only if everything worked.
    if (!error && rename(temp_path, filepath) == 0)
    {
        snprintf(response, sizeof(response), "<updatetracker %s succ>\n", command.filename);
    } 
    else 
    {
        remove(temp_path);
        snprintf(response, sizeof(response), "<updatetracker %s fail>\n", command.filename);
    }

    pthread_mutex_unlock(&tracker_mutex);
    send_all(server_worker, response, strlen(response));

}
