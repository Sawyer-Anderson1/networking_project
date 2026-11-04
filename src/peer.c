
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Import the config loaders
#include "project_502/config_loaders.h"

/*
	clientThreadConfig is the same among each peer,
	however each peer (especially if they're on the same machines) will have different
	serverThreadConfigs
		-> To solve these the scripts to create the different peers will modify the base port by +1
			and then write to the config file.
			And it will also do a similar thing for the folder names (i.e folder1, folder2, folder3, ...)
*/
main(int argc, char *argv[])
{
	// Load the port, server IP, and update interval from the clientThreadConfig file using it's config loader
	// Used for peer to tracker and download behavior
	ClientThreadConfig peer_to_tracker_config = {0};
	if (!client_thread_config_loader("clientThreadConfig.cfg", &peer_to_tracker_config))
	{
		fprintf(stderr, "Error loading clientThreadConfig.cfg configuration.\n");
		return 1;
	}
	unsigned short int server_port = peer_to_tracker_config.server_port;
	struct in_addr server_ip = peer_to_tracker_config.server_ip;
	int update_interval = peer_to_tracker_config.update_interval;

	// Load the peer's port and peer's folder for documents from the serverThreadConfig file using it's config loader
	// Used for peer's listening for other peers that want to download it's documents, so for uploading
	ServerThreadConfig upload_config = {0};
	if (!server_thread_config_loader("serverThreadConfig.cfg", &upload_config))
	{
		fprintf(stderr, "Error loading serverThreadConfig.cfg configuration.\n");
		return 1;
	}
	unsigned short int peer_port = upload_config.peer_port;
	char peer_data_folder = upload_config.peer_data_folder;

	// char server_address[50];
	// int server_port = 3490; // you should instead read from configuration file

	struct sockaddr_in server_addr;
	int sockid;

	if ((sockid = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{ // create socket
		printf("socket cannot be created\n");
		exit(0);
	}

	server_addr.sin_family = AF_INET;		   // host byte order
	server_addr.sin_port = htons(server_port); // convert to network byte order
	if (connect(sockid, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1)
	{ // connect and error check
		printf("Cannot connect to server\n");
		exit(0);
	}

	/* If connected successfully*/

	if (!strcmp(argv[1], "list"))
	{ // if this is LIST command
		int list_req = htons(LIST);
		if ((write(sockid, (char *)&list_req, sizeof(list_req))) < 0)
		{ // inform the server of the list request
			printf("Send_request  failure\n");
			exit(0);
		}

		if ((read(sockid, (char *)&msg, sizeof(msg))) < 0)
		{ // read what server has said
			printf("Read  failure\n");
			exit(0);
		}

		close(sockid);
		printf("Connection closed\n");
		exit(0);
	} // end close
}
