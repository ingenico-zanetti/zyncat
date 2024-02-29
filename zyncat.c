#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <linux/limits.h>
#include <sys/time.h>


#define RECEIVE_BUFFER_SIZE (1024 * 1024)

#define PRINT_INT(X) fprintf(stderr, #X "=%lu" "\n", X)

void checkSizeOf(void){
	struct timeval tv;
	PRINT_INT(sizeof(tv));
	PRINT_INT(sizeof(tv.tv_sec));
	PRINT_INT(sizeof(tv.tv_usec));
	exit(0);
}

struct SyncHeader {
	uint64_t tx_sec;  // time the data was received by the transmitting program
	uint64_t tx_usec; // time the data was received by the transmitting program
	uint64_t rx_sec;  // time the data was received by the receiving program
	uint64_t rx_usec; // time the data was received by the receiving program
	uint32_t t;        // type of data
	uint32_t l;        // length of following data
};

void fprintDiff(FILE *f, struct SyncHeader *sync){
	struct timeval tx = {.tv_sec = sync->tx_sec, .tv_usec = sync->tx_usec };
	struct timeval rx = {.tv_sec = sync->rx_sec, .tv_usec = sync->rx_usec };
	struct timeval delta;

	timersub(&rx, &tx, &delta);
	fprintf(f, "delta={.tv_sec=%lu, .tv_usec=%lu}" "\n", delta.tv_sec, delta.tv_usec);
}

int main(int argc, char * const argv[]){

	// checkSizeOf();

        const char *formatString = NULL;
	struct in_addr remote_addr = {0};
	struct in_addr local_addr = {0};
	unsigned short remote_port = 0;
	unsigned short local_port = 0;
	int traffic_timeout = 0;
	char *recvBuffer = (char *)malloc(RECEIVE_BUFFER_SIZE);
	int hasSync = 0;

        while (1) {
                int option_index = 0;
                static struct option long_options[] = {
                {"bind",      required_argument, 0,  'b' }, // optional local IPv4 address to bind to
                {"remote",    required_argument, 0,  'r' }, // remote IPv4 address
                {"port",      required_argument, 0,  'p' }, // remote TCP port
                {"listen",    required_argument, 0,  'l' }, // local TCP listen port
                {"timeout",   required_argument, 0,  't' }, // no transfer timeout
                {"file",      required_argument, 0,  'f' }, // file to copy output to
		{"sync",      0,                 0,  's' }, // stream contain synchronization information
                {0,         0,                 0,  0 }
                };

                int c = getopt_long(argc, argv, "r:p:l:t:f:b:s", long_options, &option_index);
                if (c == -1)
                break;

                switch (c) {
                        case 'b':
                                // fprintf(stderr, "Local IPv4 address : [%s]" "\n", optarg);
				inet_aton(optarg, &local_addr);
                        break;
                        case 'r':
                                // fprintf(stderr, "Remote IPv4 address : [%s]" "\n", optarg);
				inet_aton(optarg, &remote_addr);
                        break;
                        case 'p':
                                fprintf(stderr, "Remote TCP port : [%s]" "\n", optarg);
				remote_port = htons(atoi(optarg));
                        break;
                        case 'l':
                                fprintf(stderr, "Local TCP port : [%s]" "\n", optarg);
				local_port = htons(atoi(optarg));
                        break;
                        case 't':
                                fprintf(stderr, "Traffic timeout : [%s]" "\n", optarg);
				traffic_timeout = atoi(optarg);
                        break;
                        case 'f':
                                fprintf(stderr, "Output file : [%s]" "\n", optarg);
				formatString = strdup(optarg);
                        break;
			case 's':
				hasSync = 1;
                                fprintf(stderr, "Stream has SYNC info" "\n");
			break;
		}
	}
	if((0 == local_port) || (0 == remote_port) || (0 == remote_addr.s_addr)){
		fprintf(stderr, "local and remote port must be specified, as well as remote address" "\n");
	}else{
		fprintf(stderr, "Local address = [0x%08X/%s:%d]" "\n", local_addr.s_addr, inet_ntoa(local_addr), ntohs(local_port));
		fprintf(stderr, "Remote address = [0x%08X/%s:%d]" "\n", remote_addr.s_addr, inet_ntoa(remote_addr), ntohs(remote_port));
		{
			struct sockaddr_in local_sock_addr = {.sin_family = AF_INET, .sin_addr = local_addr, .sin_port = local_port};
			int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
			fprintf(stderr, "listen_socket=%d" "\n", listen_socket);
			const int enable = 1;
			if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0){
			    fprintf(stderr,"setsockopt(SO_REUSEADDR) failed" "\n");
			    getchar();
			}else{
				int error = bind(listen_socket, (struct sockaddr *)&local_sock_addr, sizeof(local_sock_addr));
				if(0 == error){
					error = listen(listen_socket, 1);
					if(0 != error){
						fprintf(stderr, "listen()=>%d, (%d/%s)" "\n", error, errno, strerror(errno));
						getchar();
					}else{
						int forward_socket = socket(AF_INET, SOCK_STREAM, 0);
						int traffic_socket = accept(listen_socket, NULL, NULL);
						fprintf(stderr, "traffic_socket=%d" "\n", traffic_socket);
						// Open file if a filename was provided
						int outputFile = -1;
						if(formatString){
							time_t t = time(NULL);
							struct tm *tmp = localtime(&t);
							if (tmp == NULL) {
								perror("localtime");
								exit(EXIT_FAILURE);
							}

							char filename[PATH_MAX];
							strftime(filename, sizeof(filename), formatString, tmp);
							outputFile = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, 00700);
						}
						// Connect forwarding socket
						struct sockaddr_in serverAddr = {.sin_family = AF_INET, .sin_port = remote_port, .sin_addr = remote_addr};
						int connected = connect(forward_socket, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
						if(connected < 0){
							fprintf(stderr, "connect(): %d/%s" "\n", errno, strerror(errno));
						}else{
							unsigned int socket_timeout = 0;
							unsigned long long totalRead = 0ULL;
							struct SyncHeader syncHeader;
							int readSyncHeader = hasSync;
							int syncHeaderRemaining = sizeof(syncHeader);
							int syncHeaderOffset = 0;
							unsigned char *syncHeaderPointer = (unsigned char *)&syncHeader;
							int payloadRemaining = 0;
							for(;;){
								fd_set read_set;
								FD_ZERO(&read_set);
								FD_SET(traffic_socket, &read_set);
								struct timeval timeout = {.tv_sec = traffic_timeout, .tv_usec = 0};
								int selected = select(traffic_socket + 1, &read_set, NULL, NULL, &timeout);
								if(-1 == selected){
									fprintf(stderr, "selected=%d, [%d/%s]" "\n", selected, errno, strerror(errno));
									break;
								}
								if(0 == selected){
									socket_timeout++;
									fprintf(stderr, "0 == selected, increasing timeout => %u" "\n", socket_timeout);
									if(socket_timeout > traffic_timeout){
										break;
									}
								}else{
									socket_timeout = 0;
									// fprintf(stderr, "check received" "\n");
									// Check if we are waiting for synchronization info
									if(readSyncHeader){
										int lus = read(traffic_socket, syncHeaderPointer + syncHeaderOffset, syncHeaderRemaining);
										if(lus > 0){
											syncHeaderRemaining -= lus;
											if(0 == syncHeaderRemaining){
												// SyncHeader receive complete
												struct timeval tv;
												gettimeofday(&tv, NULL);
												syncHeader.rx_sec = tv.tv_sec;
												syncHeader.rx_usec = tv.tv_usec;
												fprintDiff(stderr, &syncHeader);
												syncHeaderOffset = 0;
												readSyncHeader = 0;
												syncHeaderRemaining = sizeof(syncHeader);
												payloadRemaining = syncHeader.l; 
											}else{
												syncHeaderOffset += lus;
												continue;
											}
										}
									}
									// If we arrived here, either we have received a complete syncHeader
									// or the Sync feature is not active
									int maxRead = RECEIVE_BUFFER_SIZE;
									if(hasSync){
										// Only read up-to payload end
										maxRead = payloadRemaining;
									}

									int lus = read(traffic_socket, recvBuffer, maxRead);
									if(0 == lus){
										fprintf(stderr, "read()=>0, closing incoming connection" "\n");
										break;
									}else{
										if(hasSync){
											payloadRemaining -= lus;
											if(0 == payloadRemaining){
												readSyncHeader = 1;
											}
										}
										if(write(forward_socket, recvBuffer, lus) < 0){
											fprintf(stderr, "write(forward_socket) < 0 %d/%s" "\n", errno, strerror(errno));
											break;
										}
										if(-1 != outputFile){
											int written = write(outputFile, recvBuffer, lus);
											if(written < 0){
												fprintf(stderr, "write(%d) < 0, %d/%s" "\n", outputFile, errno, strerror(errno));
											}else if(written != lus){
												fprintf(stderr, "write(%d) => %d" "\n", lus, written);

											}
										}
										totalRead += (unsigned long long)lus;
									}
								}

							}
							fprintf(stderr, "total received = %llu" "\n", totalRead);
						}
						close(traffic_socket);
						close(forward_socket);
						if(-1 != outputFile){
							close(outputFile);
						}
					}
				}else{
					fprintf(stderr, "bind()=>%d, (%d/%s)" "\n", error, errno, strerror(errno));
					getchar();
				}
			}
			close(listen_socket);
		}
	}

	if(NULL != formatString){
		free((void*)formatString);
	}
	if(NULL != recvBuffer){
		free(recvBuffer);
	}
	return(0);
}


