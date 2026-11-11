#include "project_502/config_loaders.h"
#include "project_502/common.h"

#include <stdio.h> // For file I/O ops
/* stdio.h Commands or Data Types Used:
    FILE: data type representing the file stream
    fopen() and fclose(): To open and close the config files
    fgets(): To read the file line-by-line safely
    fprintf() and stderr: To print error messages to terminal
*/
#include <stdlib.h> // To convert text numbers into numeric variables, used to bound check for the unsigned short int variables (port)
/* stdlib.h Commands Used:
    atoi(): Used to convert string nums into an integer
    strtol(): Convert strings (ports) to long integer, to bounds check before storing as unsigned short
*/
#include <string.h> // Used to read and scan the string format of the config files
/* string.h Commands Used:
    strlen(): Calculate string lengths
    strcmp(): To check if a key matches a specific name
    strchr(): Used scan a line looking for the specific location of a delimiter (of the =)
    strncpy(): To copy a text value safely into a fixed size char arr
    memmove(): Used to shift characters left when stripping out whitespaces
*/
#include <ctype.h> // Used to anlalyze characters individually, for whitespace, etc.
/* ctype.h Commands Used:
    isspace(): To check whether a specific character is a space, tab, or newline char
*/

#include <arpa/inet.h> // Used for explicit manipulation functions to convert plain-text IP strings into binary
/* arpa/inet.h Commands Used:
    inet_pton: Used to convert IP strings to binary
*/
#include <errno.h>

// Static helper function to trim whitespace from string
static void trim(char *str)
{
    // Trim trailing whitespace
    int len = strlen(str);

    while (len > 0 && isspace((unsigned char)str[len - 1]))
    {
        str[--len] = '\0';
    }

    // Trim leading whitespace
    int start = 0;
    while (str[start] && isspace((unsigned char)str[start]))
    {
        start++;
    }
    if (start > 0)
    { // i.e. there is leading whitespace
        memmove(str, str + start, len - start + 1);
    }
}

// Static helper function to convert a string into an int within [min, max]
// Returns 1 on success, 0 on failure (not a number, trailing junk, or out of range)
static int parse_int(const char *str, long min, long max, int *out)
{
    char *endptr;
    errno = 0; // strtol reports overflow through errno
    long val = strtol(str, &endptr, 10);

    // endptr == str means no digits were read, *endptr != '\0' means there was junk after the number
    if (errno != 0 || endptr == str || *endptr != '\0' || val < min || val > max)
    {
        return 0;
    }

    *out = (int)val;
    return 1;
}

// Static helper function to cut an inline comment (anything from '#' or ';' onward) off a string
static void strip_inline_comment(char *str)
{
    char *comment = strpbrk(str, "#;"); // finds the first '#' or ';' in the string
    if (comment)
    {
        *comment = '\0'; // end the string where the comment starts
    }
}

/* -----------------------------------------------------------------------------------
    Config Functions Defined Below:
        sconfig_loader: Used to load sconfig into SConfig Struct
        client_thread_config_loader: Used to load clientThreadConfig into it's struct
        server_thread_config_loader: Used to load serverThreadConfig into it's struct
-------------------------------------------------------------------------------------*/

int sconfig_loader(const char *filename, SConfig *config)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        return 0; // Returning 0 since this is essentially boolean
    }

    char line[MAXLINE];
    while (fgets(line, sizeof(line), file))
    {
        // call trim helper function
        trim(line);

        // Skip comments and empty lines
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        // Split the line by the "=" char, to get the values for the variables
        char *delimiter = strchr(line, '=');
        if (!delimiter)
        {
            continue; // malformed line, skip
        }

        *delimiter = '\0';
        char *key = line;
        char *value = delimiter + 1;

        strip_inline_comment(value); // drop trailing comments like "# 20 mins, in secs"

        trim(key);
        trim(value);

        // Assign values to the sconfig structure
        if (strcmp(key, "server_port") == 0)
        {
            int port;
            if (!parse_int(value, 1024, 65535, &port))
            {
                fprintf(stderr, "Invalid server_port in sconfig.cfg: %s\n", value);
                fclose(file);
                return 0;
            }
            config->server_port = atoi(value);
        }
        else if (strcmp(key, "shared_directory") == 0)
        {
            strncpy(config->shared_directory, value, sizeof(config->shared_directory) - 1);
        }
        else if (strcmp(key, "dead_peer_timeout") == 0)
        {
            // 1 second up to 1 day, anything else is treated as a config error
            if (!parse_int(value, 1, 86400, &config->dead_peer_timeout))
            {
                fprintf(stderr, "Invalid dead_peer_timeout in sconfig.cfg: %s\n", value);
                fclose(file);
                return 0;
            }
        }
        else
        {
            fprintf(stderr, "There is a field within the config file sconfig.cfg that is not defined within the corresponding struct");
        }
    }

    fclose(file);
    return 1; // Success
}

int client_thread_config_loader(const char *filename, ClientThreadConfig *config)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        return 0; // Returning 0 since this is essentially boolean
    }

    char line[MAXLINE];
    while (fgets(line, sizeof(line), file))
    {
        // call trim helper function
        trim(line);

        // Skip comments and empty lines
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        // Split the line by the "=" char, to get the values for the variables
        char *delimiter = strchr(line, '=');
        if (!delimiter)
        {
            continue; // malformed line, skip
        }

        *delimiter = '\0';
        char *key = line;
        char *value = delimiter + 1;

        strip_inline_comment(value); // drop trailing comments like "# 20 mins, in secs"

        trim(key);
        trim(value);

        // Assign values to the clientthreadconfig structure
        if (strcmp(key, "server_port") == 0)
        {
            int port;
            if (!parse_int(value, 1024, 65535, &port))
            {
                fprintf(stderr, "Invalid server_port in clientThreadConfig.cfg: %s\n", value);
                fclose(file);
                return 0;
            }
            config->server_port = (unsigned short)port;
        }
        else if (strcmp(key, "server_ip") == 0)
        {
            struct in_addr addr;
            int result = inet_pton(AF_INET, value, &addr);

            if (result == 1)
            {
                printf("Success! Network byte order integer: %u\n", addr.s_addr);

                // then save to the struct
                config->server_ip = addr;
            }
            else if (result == 0)
            {
                fprintf(stderr, "Error: Invalid IP address format.\n");
            }
            else
            {
                fprintf(stderr, "inet_pton failed\n");
            }
        }
        else if (strcmp(key, "update_interval") == 0)
        {
            if (!parse_int(value, 1, 86400, &config->update_interval))
            {
                fprintf(stderr, "Invalid update_interval in clientThreadConfig.cfg: %s\n", value);
                fclose(file);
                return 0;
            }
        }
        else
        {
            // There is an unaccounted field in the config file
            fprintf(stderr, "There is a field within the config file clientThreadConfig.cfg that is not defined within the corresponding struct");
        }
    }

    fclose(file);
    return 1; // Success
}

int server_thread_config_loader(const char *filename, ServerThreadConfig *config)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        return 0; // Returning 0 since this is essentially boolean
    }

    char line[MAXLINE];
    while (fgets(line, sizeof(line), file))
    {
        // call trim helper function
        trim(line);

        // Skip comments and empty lines
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        // Split the line by the "=" char, to get the values for the variables
        char *delimiter = strchr(line, '=');
        if (!delimiter)
        {
            continue; // malformed line, skip
        }

        *delimiter = '\0';
        char *key = line;
        char *value = delimiter + 1;

        strip_inline_comment(value); // drop trailing comments like "# 20 mins, in secs"

        trim(key);
        trim(value);

        // Assign values to the serverthreadconfig structure
        if (strcmp(key, "peer_port") == 0)
        {
            int port;
            if (!parse_int(value, 1024, 65535, &port))
            {
                fprintf(stderr, "Invalid server_port in serverThreadConfig.cfg: %s\n", value);
                fclose(file);
                return 0;
            }
            config->peer_port = atoi(value);
        }
        else if (strcmp(key, "peer_data_folder") == 0)
        {
            strncpy(config->peer_data_folder, value, sizeof(config->peer_data_folder) - 1);
        }
        else
        {
            fprintf(stderr, "There is a field within the config file serverThreadConfig.cfg that is not defined within the corresponding struct");
        }
    }

    fclose(file);
    return 1; // Success
}