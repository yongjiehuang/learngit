#include <getopt.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>

int s;
int sNum;
int E;
int b = 6;
char trace1[100];
char trace2[100];
int hits;
int misses;
int evictions;
typedef struct Node{
    char state;    
    unsigned tag;
    struct Node* pre;
    struct Node* next;
} node;
typedef struct _LRU{
    node* head;
    node* tail; 
    int size;
}LRU;
typedef struct CoreInfo{
     char* traceFile;
     int coreNo;
}CoreInfo;
typedef struct Bus{
    unsigned addr;
    char cmd;
    unsigned char shared;
    unsigned char dirty;
}Bus;
typedef struct SnoopInfo{
    int coreNo;
    unsigned curS;
    unsigned curTag;
}SnoopInfo;
Bus bus;
LRU** lru;
sem_t mutex;
void parsePar(int argc, char** argv)
{
  int option;
  while((option  = getopt(argc, argv, "s:E:t:")) != -1)
  {
        switch(option){
           case 's': 
                  s = atoi(optarg);
                  break;
           case 'E': 
                  E = atoi(optarg);
                  break;
           case 't': 
                  strcpy(trace1, optarg);
                  strcpy(trace2, argv[optind]);
                  break;
        }
  }
  sNum = 1 << s;
}
void initLru(int j)
{
   int i;
   for(i = 0; i < 2; ++i)
   {
       lru[i][j].head = (node*)malloc(sizeof(node));
       lru[i][j].tail = (node*)malloc(sizeof(node));
       lru[i][j].head->pre = NULL;
       lru[i][j].head->next = lru[i][j].tail;
       lru[i][j].tail->pre = lru[i][j].head;
       lru[i][j].tail->next = NULL; 
       lru[i][j].size = 0;
   }
}
void deleteNode(int coreNo, unsigned curS, node* delNode)
{
    delNode->pre->next = delNode->next;
    delNode->next->pre = delNode->pre;
    lru[coreNo][curS].size--;
}
void addFirst(int coreNo, unsigned curS, node* addNode)
{
    lru[coreNo][curS].size++;
    addNode->next = lru[coreNo][curS].head->next;
    addNode->pre = lru[coreNo][curS].head;
    lru[coreNo][curS].head->next->pre = addNode;
    lru[coreNo][curS].head->next = addNode;
}
void deleteLast(int coreNo, unsigned curS){
    lru[coreNo][curS].size--;
    node* lastNode = lru[coreNo][curS].tail->pre;
    lastNode->pre->next = lastNode->next;
    lastNode->next->pre = lastNode->pre;
    free(lastNode);
}
void* snooping(void* arg)
{
    int coreNo = (*(int*)arg + 1) % 2;

    unsigned setMask = 0xffffffff;
    setMask >>= (32 - s);
    unsigned curS = setMask & (bus.addr >> b);
    unsigned curTag = bus.addr >> (b + s);
    LRU curLru = lru[coreNo][curS];
    node* cur = curLru.head->next;
    char cmd = bus.cmd;
    int flag = 0;
    while(cur != curLru.tail)
    {
        if(cur->tag == curTag){
           flag = 1;
           break;
        }       
        cur = cur->next;
    }
    if(flag)
    {
        char curState = cur->state; 
        if(cmd == 'r'){
            switch(curState){
               case 'm': 
                 bus.shared |= 1;
                 bus.dirty |= 1; 
                 cur->state = 's';
                 break;
               case 'e':
                 bus.shared |= 1;
                 cur->state = 's';
                 break;
               case 's':
                 bus.shared |= 1;
                 break;
            } 
        printf(" core%d knew this read to %x, it's corresponding cache line truns into %c from %c.  ", coreNo,bus.addr, curState, cur->state);
       }else{
            switch(curState){
               case 'm': 
                 deleteNode(coreNo, curS, cur); 
                 break;
               case 'e':
                 deleteNode(coreNo, curS, cur); 
                 break;
               case 's':
                 deleteNode(coreNo, curS, cur); 
                 break;
            } 
        printf(" core%d knew this write to %x, it's corresponding cache line truns into i from %c.  ",coreNo,bus.addr, curState);
       }
    } 
    return NULL;
}
void localRd(unsigned addr, int coreNo)
{
    unsigned setMask = 0xffffffff;
    setMask >>= (32 - s);
    unsigned curS = setMask & (addr >> b);
    unsigned curTag = addr >> (b + s);
    LRU curLru = lru[coreNo][curS];
    node* cur = curLru.head->next;
    int flag = 0;
    
    //临界区， 核在此操作读写事务， 并且监听总线
    sem_wait(&mutex); 
    memset(&bus, 0, sizeof(bus));
    while(cur != curLru.tail)
    {
        if(cur->tag == curTag){
           flag = 1;
           break;
        }       
        cur = cur->next;
    }
    if(flag)
    {
         printf("\ncore%d  read  addr: %x hits,  current state: %c. ", coreNo, addr, cur->state);
         hits++;
         deleteNode(coreNo, curS, cur);
         addFirst(coreNo, curS, cur);
    }else{
         node* newNode = (node*)malloc(sizeof(node));
         newNode->tag = curTag;
         misses++;
         pthread_t snoopThread; 
         printf("\ncore%d read addr: %x misses, current state: i, ", coreNo, addr);
         bus.addr = addr;
         bus.cmd = 'r';
         pthread_create(&snoopThread, NULL, snooping, &coreNo);
         pthread_join(snoopThread, NULL);
         newNode->state = bus.shared ? 's' : 'e';
         printf("after snooping, the cache state in core%d turns into %c. ", coreNo, newNode->state);
         if(curLru.size == E){
         printf(" and this read operation cause an cache line eviction!");
            evictions++;
            deleteLast(coreNo, curS);
            addFirst(coreNo, curS, newNode); 
         }else{
            addFirst(coreNo, curS, newNode); 
         }
         printf("\n");
    }
    sem_post(&mutex);
}
void localWr(unsigned addr, int coreNo)
{
    unsigned setMask = 0xffffffff;
    setMask >>= (32 - s);
    unsigned curS = setMask & (addr >> b);
    unsigned curTag = addr >> (b + s);
    LRU curLru = lru[coreNo][curS];
    node* cur = curLru.head->next;
    int flag = 0;
    
    //临界区， 核在此操作读写事务， 并且监听总线
    sem_wait(&mutex); 
    memset(&bus, 0, sizeof(bus));
    bus.addr = addr;
    bus.cmd = 'w';

    while(cur != curLru.tail)
    {
        if(cur->tag == curTag){
           flag = 1;
           break;
        }       
        cur = cur->next;
    }
    if(flag)
    {
         printf("\ncore%d  write addr: %x hits, state change from  %c to m. ", coreNo, addr, cur->state);
    pthread_t snoopThread; 
    pthread_create(&snoopThread, NULL, snooping, &coreNo);
    pthread_join(snoopThread, NULL);
         hits++;
         deleteNode(coreNo, curS, cur);
         cur->state = 'm';
         addFirst(coreNo, curS, cur);
    }else{
         printf("\ncore%d write addr: %x misses, state change from i to m. ", coreNo, addr);
    pthread_t snoopThread; 
    pthread_create(&snoopThread, NULL, snooping, &coreNo);
    pthread_join(snoopThread, NULL);
         node* newNode = (node*)malloc(sizeof(node));
         newNode->tag = curTag;
         newNode->state = 'm'; 
         misses++;
         if(curLru.size == E){
            evictions++;
            deleteLast(coreNo, curS);
            addFirst(coreNo, curS, newNode); 
         }else{
            addFirst(coreNo, curS, newNode); 
         }
    }
    sem_post(&mutex);
}
void* accessThread(void* arg)
{
    CoreInfo* coreInfo = (CoreInfo*)arg;
    char op;
    unsigned addr;
    FILE* file = fopen(coreInfo->traceFile, "r");
    while(fscanf(file, "%c %x\n", &op, &addr) > 0)
    {
       //printf("\ncore%d: %c %x\n", coreInfo->coreNo, op, addr);
       switch(op){
           case '0':
                localRd(addr, coreInfo->coreNo);
                break;
           case '1':
                localWr(addr, coreInfo->coreNo);
                break;
       }
    }
    return NULL;
}
void cachesimulate()
{
    lru = (LRU**)malloc(sizeof(LRU*) * 2);
    for(int i = 0; i < 2; ++i)
        lru[i] = (LRU*)malloc(sNum * sizeof(LRU));
    for(int j = 0; j < sNum; ++j)
          initLru(j);
    CoreInfo info1, info2;
    info1.traceFile = trace1;
    info1.coreNo = 0;
    info2.traceFile = trace2;
    info2.coreNo = 1;
    pthread_t core1, core2;
    pthread_create(&core1, NULL, accessThread, &info1);
    pthread_create(&core2, NULL, accessThread, &info2);
    pthread_join(core1, NULL);
    pthread_join(core2, NULL);
}
int main(int argc, char** argv)
{
    parsePar(argc, argv);
    sem_init(&mutex, 0, 1);
    cachesimulate();
    printf("\n");
    return 0;
}
