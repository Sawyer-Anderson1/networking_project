#ifndef TRACKER_COMMANDS_H
#define TRACKER_COMMANDS_H

#include "project_502/common.h"

#include <netinet/in.h> // for struct in_addr
/* netinet/in.h Commands or Data Types Used:
    in_addr: stuct for IP addresses
*/
#include <stdint.h>
/* stdint.h Commands or Data Types Used:
    unit64_t: exactly 64 bits for start and end bytes
*/

#define TRACKER_FILENAME_LEN 256
#define TRACKER_DESCRIPTION_LEN 1024

// Structure for parsed commands
typedef struct
{
    char filename[TRACKER_FILENAME_LEN];
    size_t filesize;
    char description[TRACKER_DESCRIPTION_LEN];
    char md5_string[MD5_HEX_LEN]; // Converted from the unsigned char [16] of the binary/byte form, so the final value can be displayed
    struct in_addr peer_ip;
    unsigned short int peer_port;
    int valid;
} CreateCommand;

typedef struct
{
    char filename[TRACKER_FILENAME_LEN];
    uint64_t start_byte;
    uint64_t end_byte;
    struct in_addr peer_ip;
    unsigned short int peer_port;
    int valid;
} UpdateCommand;

// Public function declaration
int handle_list_req(int server_worker, const char *shared_directory);
char *xtract_fname(const char *read_msg, char *fname, size_t out_size);
int handle_get_req(int server_worker, const char *fname, const char *shared_directory);
CreateCommand tokenize_createmsg(const char *read_msg);
void handle_createtracker_req(int server_worker, CreateCommand command, const char *shared_directory);
UpdateCommand tokenize_updatemsg(const char *read_msg);
void handle_updatetracker_req(int server_worker, UpdateCommand command, const char *shared_directory);

#endif