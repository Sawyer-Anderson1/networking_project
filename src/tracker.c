#include "project_502/common.h"
// Load the config loader
#include "project_502/config_loaders.h"
// Load the command handlers
#include "project_502/tracker_commands.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/dir.h>

#include <sys/stat.h>
/* sys/stat.h Commands Used:
	mkdir(): To make the shared torrent directory
*/

main()
{
	pid_t pid;
	struct sockaddr_in server_addr, client_addr;
	int sockid;
	if ((sockid = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{ // create socket connection oriented
		printf("socket cannot be created \n");
		exit(0);
	}

	// Load the port, shared directory name, and dead peer timeout interval from the sconfig file using it's config loader
	SConfig server_config = {0};
	if (!sconfig_loader("sconfig.cfg", &server_config))
	{
		fprintf(stderr, "Error loading sconfig.cfg configuration.\n");
		exit(1);
	}
	unsigned short int server_port = server_config.server_port;
	const char *shared_directory = server_config.shared_directory;
	int dead_peer_timeout = server_config.dead_peer_timeout;

	/*******************************
		On creation of the Server,
		create the shared directory
	*******************************/
	// Uses permission 0700 for security reasons, only the tracker server can edit the shared folder torrents
	if (mkdir(shared_directory, 0700) == 0)
	{
		printf("Directory '%s' created successfully.\n", shared_directory);
	}
	else
	{
		fprintf(stderr, "Failed to create the shared directory %s\n", shared_directory);
	}

	// socket created at this stage
	// now associate the socket with local port to allow listening incoming connections
	server_addr.sin_family = AF_INET;		   // assign address family
	server_addr.sin_port = htons(server_port); // change server port to NETWORK BYTE ORDER
	// server_addr.sin_addr.s_addr = htons(INADDR_ANY); // INADDR_ANY uses the IP address of the machine
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // switched to htonl, for 32 bit address
	memset(&(server_addr.sin_zero), '\0', 8);		 // zero the rest of the struct

	if (bind(sockid, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
	{ // bind and check error
		fprintf(stderr, "Bind failure\n");
		// exit(0); This is success, so its wrong
		exit(1);
	}

	printf("Tracker SERVER READY TO LISTEN INCOMING REQUEST.... \n");
	// SOMAXCONN is the OS's maxiumum system limit
	if (listen(sockid, SOMAXCONN) < 0) // added backlog, the maximum number of unaccepted connections the kernel can queue up
	{								   //(parent) process listens at sockid and check error
		fprintf(stderr, "Tracker SERVER CANNOT LISTEN\n");
		// exit(0);
		exit(1);
	}

	while (1)
	{ // accept  connection from every requester client
		int sock_child;
		if ((sock_child = accept(sockid, (struct sockaddr *)&client_addr, sizeof(client_addr))) == -1)
		{ /* accept connection and create a socket descriptor for actual work */
			fprintf(stderr, "Tracker Cannot accept...\n");
			exit(1);
		}

		// create the worker threads via fork
		if ((pid = fork()) == 0)
		{												// New child process will serve the requester client. separate child will serve separate client
			close(sockid);								// child does not need listener
			peer_handler(sock_child, shared_directory); // child is serving the client.
			close(sock_child);							// printf("\n 1. closed");
			exit(0);									// kill the process. child process all done with work
		}
		close(sock_child); // parent all done with client, only child will communicate with that client from now
	} // accept loop ends
} // main fun ends

peer_handler(int sock_child, const char shared_directory)
{ // function for file transfer. child process will call this function
	// start handiling client request
	int length;
	char read_msg[MAXLINE - 1]; // MAXLINE - 1 since we add \0 at the end
	length = read(sock_child, read_msg, MAXLINE);

	read_msg[length] = '\0';
	if ((strcmp(read_msg, "REQ LIST") == 0) || (strcmp(read_msg, "req list") == 0) || (strcmp(read_msg, "<REQ LIST>") == 0) || (strcmp(read_msg, "<REQ LIST>\n") == 0))
	{												   // list command received
		handle_list_req(sock_child, shared_directory); // handle list request
		printf("list request handled.\n");
	}
	else if ((strstr(read_msg, "get") != NULL) || (strstr(read_msg, "GET") != NULL))
	{										// get command received
		char fname = xtrct_fname(read_msg); // extract filename from the command
		handle_get_req(sock_child, fname);
	}
	else if ((strstr(read_msg, "createtracker") != NULL) || (strstr(read_msg, "Createtracker") != NULL) || (strstr(read_msg, "CREATETRACKER") != NULL))
	{ // get command received
		CreateCommand command_struct = tokenize_createmsg(read_msg);
		handle_createtracker_req(sock_child, command_struct);
	}
	else if ((strstr(read_msg, "updatetracker") != NULL) || (strstr(read_msg, "Updatetracker") != NULL) || (strstr(read_msg, "UPDATETRACKER") != NULL))
	{ // get command received
		UpdateCommand command_struct = tokenize_updatemsg(read_msg);
		handle_updatetracker_req(sock_child, command_struct);
	}

} // end client handler function
