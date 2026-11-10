#ifndef TRACKER_COMMANDS_H
#define TRACKER_COMMANDS_H

#include <netinet/in.h> // for struct in_addr
/* netinet/in.h Commands or Data Types Used:
    in_addr: stuct for IP addresses
*/
#include <stdint.h>
/* stdint.h Commands or Data Types Used:
    unit64_t: exactly 64 bits for start and end bytes
*/

// Structure for parsed commands
typedef struct
{
    char filename;
    size_t filesize;
    char description;
    char md5_string[33]; // Converted from the unsigned char [16] of the binary/byte form, so the final value can be displayed
    struct in_addr peer_ip;
    unsigned short int peer_port;
} CreateCommand;

typedef struct
{
    char filename;
    uint64_t start_byte;
    uint64_t end_byte;
    struct in_addr peer_ip;
    unsigned short int peer_port;
} UpdateCommand;

// Public function declaration
int handle_list_req(int server_worker, const char shared_directory);
char *xtract_fname(char read_msg);
void handle_get_req(int server_worker, char fname);
CreateCommand tokenize_createmsg(char read_msg);
void handle_createtracker_req(int server_worker, CreateCommand command);
UpdateCommand tokenize_updatemsg(char read_msg);
void handle_updatetracker_req(int server_worker, UpdateCommand command);

#endif