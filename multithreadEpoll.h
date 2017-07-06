

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <pthread.h> 

struct TaskContent{
    int fd;
    int type;
};

struct SocketContent{
    GSList *towriteList;
    pthread_mutex_t contentLock;
};


struct Node{
    struct Node* next;
    struct TaskContent content;
};

struct List{
    struct Node* head;
    struct Node* tail;
};