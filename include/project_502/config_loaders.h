#ifndef CONFIG_LOADERS_H
#define CONFIG_LOADERS_H

#include <netinet/in.h> // for struct in_addr
/* netinet/in.h Commands or Data Types Used:
    in_addr: stuct for IP addresses
*/

#define CONFIG_PATH_LEN 256

// Structure to hold configuration values
// For tracker server:
typedef struct
{
    unsigned short int server_port;
    char shared_directory[CONFIG_PATH_LEN];
    int dead_peer_timeout;
} SConfig;

// For peers:
// both for clientThreadConfig and serverThreadConfig
typedef struct
{
    unsigned short int server_port;
    struct in_addr server_ip;

    // in other possible values
    //...

    int update_interval;
} ClientThreadConfig;

typedef struct
{
    unsigned short int peer_port;

    // in other possible values
    // ...

    char peer_data_folder[CONFIG_PATH_LEN];
} ServerThreadConfig;

// Public function declaration
int sconfig_loader(const char *filename, SConfig *config);
int client_thread_config_loader(const char *filename, ClientThreadConfig *config);
int server_thread_config_loader(const char *filename, ServerThreadConfig *config);

#endif