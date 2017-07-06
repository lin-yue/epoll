#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include "multithreadEpoll.h"

#define MAXLINE     1000  //读取socket和控制台的缓冲区的大小
#define OPEN_MAX    100  //最大并发数目
#define LISTENQ     20   //等待连接队列最大个数
#define SERV_PORT   5555  //服务器端口
#define THREAD_NUMBER 2   //线程池线程个数

#define TYPE_READ   1
#define TYPE_WRITE  2
#define TYPE_CONSOLEREAD 3






pthread_t threadId[THREAD_NUMBER];        //线程池
pthread_mutex_t threadLock[THREAD_NUMBER];//线程锁
pthread_mutex_t listLock;  //任务列表锁
pthread_mutex_t paramLock;  //线程参数锁
pthread_mutex_t nameSockfdHashLock;  //客户端和sockfd哈希锁
pthread_mutex_t sockfdContentHashLock;  //sockfd和内容哈希锁  


pthread_cond_t count_nonzero[THREAD_NUMBER];
int threadParam[THREAD_NUMBER][2];       //各线程参数
struct List fdList;                      //任务列表
int epfd;                                //epoll文件描述符
 GHashTable *table;                      //客户端到sockfd哈希表
 GHashTable *contentTable;               //sockfd到内容哈希表

    
/*初始化socketContent的内容*/
void initSocketContent(struct SocketContent* content)
{
	content->towriteList = NULL;
	pthread_mutex_init(&content->contentLock, NULL);
}


/*初始化任务链表*/
void initList(struct List* fdList){
    fdList->head = NULL;
    fdList->tail = NULL;
}

/*添加任务链表的文件描述符和任务类型*/
void add(struct List* fdList, int fd, int type){

    struct Node* newNode;
    newNode = (struct Node*) malloc(sizeof(struct Node));
    newNode->next = NULL;
    newNode->content.fd = fd;
	newNode->content.type = type;
	pthread_mutex_lock (&listLock);
    if(fdList->tail == NULL)
    {

        fdList->tail = newNode;
        fdList-> head = newNode;
    }
    else
    {
        fdList->tail->next = newNode;
        fdList->tail = newNode;
    }
	pthread_mutex_unlock (&listLock);
}

/*从任务列表的头部删除并返回此节点内容*/
struct TaskContent removeHead(struct List* fdList)
{
    struct Node* tempt = NULL;
	struct TaskContent tempt1;
	tempt1.fd = -1;
	pthread_mutex_lock (&listLock);
    if(fdList->head == NULL)
    {
		pthread_mutex_unlock (&listLock);
        return tempt1;
    }
    else
    {
        tempt = fdList->head;
		if(fdList->head == fdList->tail)
		{
			fdList->tail = fdList->head->next;
		}
        fdList->head = fdList->head->next;
		tempt1 = tempt->content;
		free(tempt);
		pthread_mutex_unlock (&listLock);
        return tempt1;
    }

}

/*对glib的哈希表做了一些封装函数，方便向contentTable中插入、查找具体内容*/


/*搜索哈希表中一个fd对应的内容*/
struct SocketContent* searchHash(int fd)
{
	//struct SocketContent content;
	char* key;
	struct SocketContent* tempt;
	key = (char*) malloc(sizeof(char) * 40);
	sprintf(key, "%d", fd);
	pthread_mutex_lock (&sockfdContentHashLock);
	tempt = (struct SocketContent*)g_hash_table_lookup(contentTable, key);//************
     if(tempt)
	 {  
		 pthread_mutex_unlock (&sockfdContentHashLock);
		 free(key);
          return tempt;
      }
	 else
	 {
		   pthread_mutex_unlock (&sockfdContentHashLock);
		   free(key);
           printf("error search hash\n");
		   exit(1);
     }      
}

/*向哈希表相应fd中插入待写内容*/
void insertIntoHash(int fd, char* towrite)
{
	char* key;
	struct SocketContent* content;
	key = (char*) malloc(sizeof(char) * 40);
    sprintf(key, "%d", fd);
	pthread_mutex_lock (&sockfdContentHashLock);
	content = (struct SocketContent*)g_hash_table_lookup(contentTable, key);//************
	if(content != NULL)
	{
		if(towrite == NULL)
		{
			pthread_mutex_unlock (&sockfdContentHashLock);
			return;
		}
		pthread_mutex_lock(&content->contentLock);
		content->towriteList = g_slist_append(content->towriteList, (gpointer)towrite);
		pthread_mutex_unlock(&content->contentLock);
	}
	else
	{
		content = (struct SocketContent*) malloc(sizeof(struct SocketContent));
		initSocketContent(content);
		if(towrite != NULL)
		{
			content->towriteList = g_slist_append(content->towriteList, (gpointer)towrite);
		}
		g_hash_table_insert(contentTable, key, content);
	}
	pthread_mutex_unlock (&sockfdContentHashLock);

}

/*删除哈希表中对应fd的内容*/
void deleteInHash(int fd)
{
	char* key;
	struct SocketContent* content;
	key = (char*) malloc(sizeof(char) * 40);
    sprintf(key, "%d", fd);
	pthread_mutex_lock (&sockfdContentHashLock);
	content = (struct SocketContent*)g_hash_table_lookup(contentTable, key);
	if(content != NULL)
	{
		pthread_mutex_lock(&content->contentLock);
		g_slist_free(content->towriteList);
		content->towriteList = NULL;
		pthread_mutex_unlock(&content->contentLock);
	}
	pthread_mutex_unlock (&sockfdContentHashLock);

}

/*线程主处理函数，参数为线程参数*/
void* threadHandle(void* param)
{
   char buf[MAXLINE];
   int fd;
   int n;
   struct epoll_event ev;
   struct TaskContent content;
   struct SocketContent* content1;
   char* tempt, *tempt2;
   int* tempt1;
   GSList* st;
   pthread_detach(pthread_self());
   int threadNum = *((int*)param + 1);
   while(1)
   {
       pthread_mutex_lock (threadLock+ threadNum);
	   pthread_mutex_lock(&paramLock);
	   *(int*)param = 0;   //修改线程活跃状态为不活跃
	   pthread_mutex_unlock(&paramLock);
       pthread_cond_wait( count_nonzero+threadNum, threadLock+threadNum);  //阻塞，等待主线程信号
       pthread_mutex_unlock (threadLock+threadNum);
       
       printf("启动线程：%d\n", threadNum);
       while(1)
       {
            memset(buf,0,sizeof(buf));
            content = removeHead(&fdList);  //从任务列表中读取
			fd = content.fd;
			printf("content fd is %d\n", content.fd);
            if(fd == -1)
            {
                break;
            }
            else
            {
                printf("线程%d处理socket%d\n", threadNum, fd);
				if(content.type == TYPE_READ)   //如果为读socket任务
				{
                    if((n = read(fd, buf, MAXLINE)) < 0) //读取不成功
                    {
                        if(errno == ECONNRESET) 
                        {
                            
							deleteInHash(fd);
							close(fd);							 
                         } 
                         else
                         {
                             printf("readline error");
                         }
                     } 
                     else if(n == 0) //客户端关闭
                     {
                        
                         printf("socket %d connection end\n", fd);
                         ev.data.fd = fd;
                         ev.events = EPOLLIN| EPOLLOUT | EPOLLET;
                         if (epoll_ctl(epfd, EPOLL_CTL_DEL,fd, &ev) < 0)
                         {  
                              perror("epoll_ctl error:");    
                          }
						 deleteInHash(fd);
						 close(fd);
						 
						 
                     }
                     else
                    {
						buf[n] = 0;
						printf("received data: %s from socket %d\n", buf, fd);
						if(strstr(buf, "client:") != NULL)  //若为客户端发来名字的消息
						{
							tempt = (char*) malloc(sizeof(char) * (n + 1));
							tempt1 = (int*) malloc(sizeof(int));
						    strcpy(tempt, buf + strlen("client:"));
							*tempt1 = fd;
							pthread_mutex_lock(&nameSockfdHashLock);
							g_hash_table_insert(table,tempt, tempt1);
							pthread_mutex_unlock(&nameSockfdHashLock);
							tempt = (char*) malloc(sizeof(char) * (4));
							strcpy(tempt, "ok");
							insertIntoHash(fd, tempt);

						}
						else if(strstr(buf, "message:") != NULL)  //若为一般消息
						{
							tempt = (char*) malloc(sizeof(char) * (n + 1));
						    strcpy(tempt, buf + strlen("message:"));
						    insertIntoHash(fd, tempt);
						}
						else if(strcmp(buf,"bye") == 0)  //若为结束连接消息
						{
							 printf("socket %d connection end\n", fd);
                             ev.data.fd = fd;
                             ev.events = EPOLLIN| EPOLLOUT | EPOLLET;
                             if (epoll_ctl(epfd, EPOLL_CTL_DEL,fd, &ev) < 0)
                             {  
                                   perror("epoll_ctl error:");    
                             }
						     deleteInHash(fd);
						     close(fd);
						}

						ev.data.fd = fd;
                        ev.events = EPOLLIN| EPOLLOUT | EPOLLET;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
                    
                    }
				}
				if(content.type == TYPE_WRITE)//若为写客户端的消息
				{					
					content1 = searchHash(fd);
					pthread_mutex_lock(&content1->contentLock);
					if(g_slist_length(content1 -> towriteList) == 0)  //若socketContent中待写内容的链表为空
					{
						printf("warning: no data to write, maybe because the socket is closed\n");
					}
					else
					{
												
						st = g_slist_nth(content1->towriteList, 0);
						tempt = (char*)st->data;
						write(fd, tempt, strlen(tempt));
						content1->towriteList = g_slist_remove(content1->towriteList, st->data);
						if(g_slist_length(content1->towriteList) == 0)
				        {
						     ev.events = EPOLLIN |EPOLLET;
				        } 
			            else
				        {
							 ev.events = EPOLLOUT |EPOLLIN |EPOLLET;
				        }

						ev.data.fd = fd;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);

                        printf("write data: %s from socket %d\n", tempt, fd);
						free(tempt);
						
                        
					}
					pthread_mutex_unlock(&content1->contentLock);
				}
				if(content.type == TYPE_CONSOLEREAD)    //若为从控制台读取的任务
				{   
					if((n = read(fd, buf, MAXLINE)) <= 0) 
                    {
                         printf("readline error");
                    }
					else
					{
						
				        tempt = (char*) malloc(sizeof(char) * (n + 1));
						if((tempt2 = strstr(buf, "#")) == NULL)
						{
							printf("input error, please input client name and message again(separate by #) \n");
							continue;
						}
						*tempt2 = 0;
						pthread_mutex_lock(&nameSockfdHashLock);
						tempt1 = (int*) g_hash_table_lookup(table, buf);
						pthread_mutex_unlock(&nameSockfdHashLock);
						if(tempt1 == NULL)
						{
							printf("no such client, please input client name and message again(separate by #) \n");
							continue;
						}
				        strcpy(tempt, (tempt2 + 1));
						content1 = searchHash(*tempt1);
						pthread_mutex_lock(&content1->contentLock);
				        content1->towriteList = g_slist_append(content1->towriteList, (gpointer)tempt);
						pthread_mutex_unlock(&content1->contentLock);
						buf[n] = 0;
						ev.data.fd = *tempt1;
                        ev.events = EPOLLOUT | EPOLLIN |EPOLLET;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, *tempt1, &ev);
						ev.data.fd = 0;
                        ev.events = EPOLLIN |EPOLLET;
						epoll_ctl(epfd, EPOLL_CTL_MOD, 0, &ev);
						printf("please enter the client name and the message you want to send(separate by #):\n");

					}

				}
            }
            
       }


   }
}

/*初始化线程池中线程和一些相关内容*/
int initThreadPool(void) 
{   
    int a=0;
    pthread_mutex_init(&listLock, NULL);
	pthread_mutex_init(&paramLock, NULL);
	pthread_mutex_init(&nameSockfdHashLock, NULL);
	pthread_mutex_init(&sockfdContentHashLock, NULL);
    for(int i=0;i<THREAD_NUMBER;i++)  
    {  
 
        pthread_cond_init(count_nonzero+i,NULL);  
        pthread_mutex_init(threadLock+i,NULL);
		threadParam[i][0] = 0;
		threadParam[i][1] = i;
        a= pthread_create( threadId+ i, NULL, (void* (*)(void *))threadHandle,(void *)(threadParam[i]));  
        if(a!=0)  
        {  
            perror("pthread_create error:");  
            return -1;  
        }  
    }  
    return 0;  
}  

/*设置文件为非阻塞*/
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

/*检查是否有空闲进程，若有则发送相应信号*/
void checkThreadPoolAndSendSignal()
{
	int i =0;
	pthread_mutex_lock(&paramLock);
	for(i = 0; i < THREAD_NUMBER; i++)
	{
		if(threadParam[i][0] == 0)
		{
			threadParam[i][0] = 1;
			break;
		}
	}
	pthread_mutex_unlock(&paramLock);
	if(i == THREAD_NUMBER)
	{
		return;
	}
	
	pthread_mutex_lock (threadLock+i);
	pthread_cond_signal(count_nonzero+i);  //向对应线程发送信号
	pthread_mutex_unlock (threadLock+i);
}

int main(int argc, char *argv[])
{
    printf("epoll socket begins.\n");
    int i, maxi, listenfd, connfd, sockfd, nfds;
    ssize_t n;
    char line[MAXLINE];
    socklen_t clilen = 1;
    struct epoll_event ev, events[OPEN_MAX];

    initList(&fdList);
    initThreadPool();
	table = g_hash_table_new(g_str_hash, g_str_equal);
	contentTable = g_hash_table_new(g_str_hash, g_str_equal);

    epfd = epoll_create(256);

    struct sockaddr_in clientaddr;
    struct sockaddr_in serveraddr;

	setnonblocking(0);
    ev.data.fd = 0;
    ev.events = EPOLLIN | EPOLLET;
	epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev);
    listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setnonblocking(listenfd);
    ev.data.fd = listenfd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);
    bzero(&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    char *local_addr = "127.0.0.1";
    inet_aton(local_addr, &(serveraddr.sin_addr));
    serveraddr.sin_port = htons(SERV_PORT);
    bind(listenfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
    listen(listenfd, LISTENQ);
    maxi = 0;
	printf("if you want to send message,\n");
	printf("please enter the client name and the message you want to send(separate by #):\n");

    for(; ;) {
        nfds = epoll_wait(epfd, events, OPEN_MAX, -1);  //阻塞等待事件触发

        for(i = 0; i < nfds; ++i) {
            if(events[i].data.fd == listenfd) {  //若为客户端连接事件
                printf("accept connection, fd is %d\n", listenfd);
                connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clilen);
                if(connfd < 0) {
                    perror("connfd < 0");
					printf("connfd:%d\n", connfd);
					printf("Error accept(): %s(%d)\n", strerror(errno), errno);
                    exit(1);
                }

                setnonblocking(connfd);
                char *str = inet_ntoa(clientaddr.sin_addr);
                printf("connect from %s\n", str);
                ev.data.fd = connfd;
                ev.events = EPOLLIN | EPOLLET;
                epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev);
				insertIntoHash(connfd, NULL);
            }
            else if((events[i].events & EPOLLIN) && events[i].data.fd != 0) {  //若为socket读事件
                add(&fdList, events[i].data.fd, TYPE_READ);
				checkThreadPoolAndSendSignal();
            }
            else if(events[i].events & EPOLLOUT) {  //若为socket写事件
                add(&fdList, events[i].data.fd, TYPE_WRITE);
				checkThreadPoolAndSendSignal();
            }
			else if((events[i].events & EPOLLIN) && events[i].data.fd == 0) {  //若为控制台读事件
				add(&fdList, events[i].data.fd, TYPE_CONSOLEREAD);
				checkThreadPoolAndSendSignal();
			}
        }
    }
}