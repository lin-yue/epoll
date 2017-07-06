#include <sys/socket.h>
#include <netinet/in.h>

#include <unistd.h>
#include <errno.h>

#include <string.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <glib.h>

#define MAXLINE 1000

void setnonblocking(int sock)
{
    int opts;
    opts = fcntl(sock, F_GETFL);

    if(opts < 0) {
        perror("fcntl(sock, GETFL)");
        exit(1);
    }

    opts = opts | O_NONBLOCK;

    if(fcntl(sock, F_SETFL, opts) < 0) {
        perror("fcntl(sock, SETFL, opts)");
        exit(1);
    }
}

int main(int argc, char **argv) {
	int sockfd;
	struct sockaddr_in addr;
	char buf[MAXLINE + 1];
	char* tempt = NULL;
	int len;
	int p, n;
	int epfd, nfds;
	int i =0;
	struct epoll_event ev, events[256];
	GSList *towriteList = NULL;
	GSList *st = NULL;

	if ((sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		printf("Error socket(): %s(%d)\n", strerror(errno), errno);
		return 1;
	}

	epfd = epoll_create(256);	
    ev.data.fd = 0;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev);



	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(5555);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
		printf("Error inet_pton(): %s(%d)\n", strerror(errno), errno);
		return 1;
	}

	if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		printf("Error connect(): %s(%d)\n", strerror(errno), errno);
		return 1;
	}
    ev.data.fd = sockfd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

    //客户端告诉服务器客户端名字

	printf("please enter the client name: \n");
	if((n = read(0, buf, MAXLINE)) <= 0) 
	{
         printf("readline error");
		 exit(1);
    }
	buf[n - 1] = 0;
	tempt = (char*) malloc(sizeof(char) * MAXLINE);
	strcpy(tempt, "client:");
	strcat(tempt, buf);
	n = write(sockfd, tempt, strlen(tempt));
	free(tempt);				
	if (n < 0) 
	{
		  printf("Error write(): %s(%d)\n", strerror(errno), errno);
		  exit(1);
 	 } 		
	n = read(sockfd, buf, MAXLINE);
	buf[n] = 0;
	if(strcmp(buf, "ok") != 0)
	{
		printf("send client name error\n");
		exit(1);
	}
	setnonblocking(sockfd);
	setnonblocking(0);


	printf("please write the message to be sent to the server: \n");

	//监听各类事件

	for(; ;) {
        nfds = epoll_wait(epfd, events, 256, -1);

        for(i = 0; i < nfds; ++i) {
            if((events[i].data.fd == 0) && (events[i].events & EPOLLIN)) {  //控制台输入事件

				if((n = read(0, buf, MAXLINE)) <= 0) {
                        printf("readline error");
						exit(1);
                    }
				buf[n - 1] = 0;
				
                printf("received data from terminal: %s\n", buf);
				
                ev.data.fd = sockfd;
                ev.events = EPOLLOUT | EPOLLIN |EPOLLET;
				tempt = (char*) malloc(sizeof(char) * MAXLINE);
				strcpy(tempt, buf);
				towriteList = g_slist_append(towriteList, (gpointer)tempt);
                epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, &ev);
            }
            else if((events[i].data.fd == sockfd) && (events[i].events & EPOLLIN)) {  //socket读事件

                if((n = read(sockfd, buf, MAXLINE)) < 0) {
                        printf("readline error");
						exit(1);
                    }
				else if(n == 0)
				{
					close(sockfd);
                    events[i].data.fd = -1;
                    printf("connection end\n");
				}
				else
				{
					buf[n] = 0;
					printf("received data from socket: %s\n", buf);
					ev.data.fd = sockfd;
					if(g_slist_length(towriteList) == 0)
					{
						ev.events = EPOLLIN |EPOLLET;
					}
					else
					{
						ev.events = EPOLLOUT |EPOLLIN |EPOLLET;
					}
					epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, &ev);
					printf("please write the message to be sent to the server: \n");
				}                
            }
            else if((events[i].data.fd == sockfd) && (events[i].events & EPOLLOUT)) {  //socket写事件
								
				st = g_slist_nth(towriteList, 0);
				tempt = (char*) malloc(sizeof(char) * MAXLINE);
				//printf("to write is %s\n", (char*)st->data);
				//printf("num is %ld\n", strlen((char*)(st->data)));
				if(strcmp(st->data,"bye") == 0)
				{
					n = write(sockfd, (char*)st->data, strlen((char*)st->data));
					g_slist_free(towriteList);
	                close(sockfd);
					return 0;

				}
				else
				{
	                strcpy(tempt, "message:");
	                strcat(tempt, (char*) st->data);
	                n = write(sockfd, tempt, strlen(tempt));
				}
	            free(tempt);
				tempt = (char*)st->data;
				towriteList = g_slist_remove(towriteList, st->data);

				
	            if (n < 0) 
				{
					printf("Error write(): %s(%d)\n", strerror(errno), errno);
					exit(1);
 	            } 			

                printf("written data to socket: %s\n", tempt);
				free(tempt);

				if(g_slist_length(towriteList) == 0)
				{
						ev.events = EPOLLIN |EPOLLET;
				}
			    else
				{
						ev.events = EPOLLOUT |EPOLLIN |EPOLLET;
				}
                ev.data.fd = sockfd;
                epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, &ev);
            }
        }
    }
	
    g_slist_free(towriteList);
	close(sockfd);

	return 0;
}
