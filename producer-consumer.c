#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <signal.h>
#include <math.h>

#define TRUE (1==1)
#define QUEUESIZE 1000
#define LOOP 1000

long long int ArrivalPickupTimeIntervalSum = 0;

void sigtstp_handler(int sig){
    printf("\n%lld", ArrivalPickupTimeIntervalSum);
    exit(1);
}

void *producer (void *args);
void *consumer (void *args);

void* TestFunction(void* args){
    double y;
    double x = 0.0;
    for(int i = 0; i < 10; i++){
        y = cos(x);
        x += 1.0;
    }
    return NULL;
}

struct WorkFunction{
    void* (*work)(void*);
    void* arg;
};

typedef struct {
    long long int ArrivalPickupTimeInterval[QUEUESIZE];
    struct WorkFunction buf[QUEUESIZE];
    long head, tail;
    int full, empty;
    pthread_mutex_t *mut;
    pthread_cond_t *notFull, *notEmpty;
} queue;

queue *queueInit (void);
void queueDelete (queue *q);
void queueAdd (queue *q, struct WorkFunction in);
void queueDel (queue *q, struct WorkFunction* out);

int main(int argc, char** argv)
{
    if(argc != 3){
        printf("invalid number of arguments");
        exit(1);
    }

    signal(SIGTSTP, sigtstp_handler);

    int proTotal = atoi(argv[1]);
    int conTotal = atoi(argv[2]);
    pthread_t* pro = (pthread_t*)malloc(proTotal*sizeof(pthread_t));
    pthread_t* con = (pthread_t*)malloc(conTotal*sizeof(pthread_t));
    
    queue *fifo = queueInit();
    if (fifo ==  NULL) {
        fprintf (stderr, "main: Queue Init failed.\n");
        exit (1);
    }
    
    int proIndex; // run threads
    for(proIndex = 0; proIndex < proTotal; proIndex++){
        pthread_create (&pro[proIndex], NULL, producer, fifo);
    }
    int conIndex;
    for(conIndex = 0; conIndex < conTotal; conIndex++){
        pthread_create (&con[conIndex], NULL, consumer, fifo);
    }

    for(proIndex = 0; proIndex < proTotal; proIndex++){ // join threads
        pthread_join(pro[proIndex], NULL);
    }
    for(conIndex= 0; conIndex < conTotal; conIndex++){
        pthread_join (con[conIndex], NULL);
    }
    
    queueDelete (fifo);
    printf("job is done!\n");

    return 0;
}

void *producer (void *q)
{
    queue *fifo = (queue *)q;
    struct WorkFunction workFunction;
    workFunction.work = TestFunction;
    workFunction.arg = NULL;

    int i;
    for (i = 0; i < LOOP; i++) {
        pthread_mutex_lock (fifo->mut);
        while (fifo->full) {
            //printf ("producer: queue FULL.\n");
            pthread_cond_wait (fifo->notFull, fifo->mut);
        }
        queueAdd (fifo, workFunction);
        pthread_mutex_unlock (fifo->mut);
        pthread_cond_signal (fifo->notEmpty);
    }

    return NULL;
}

void *consumer (void *q)
{
    queue *fifo = (queue*)q;
    struct WorkFunction workFunction; 

    int i;
    while(TRUE){
        pthread_mutex_lock (fifo->mut);
        while (fifo->empty) {
            //printf ("consumer: queue EMPTY.\n");
            pthread_cond_wait (fifo->notEmpty, fifo->mut);
        }
        queueDel (fifo, &workFunction);
        pthread_mutex_unlock (fifo->mut);
        pthread_cond_signal (fifo->notFull);

        workFunction.work(workFunction.arg);
    }
    
    return NULL;
}

queue *queueInit (void)
{
    queue *q;
    q = (queue *)malloc (sizeof (queue));
    if (q == NULL) return (NULL);

    q->empty = 1;
    q->full = 0;
    q->head = 0;
    q->tail = 0;
    q->mut = (pthread_mutex_t *) malloc (sizeof (pthread_mutex_t));
    pthread_mutex_init (q->mut, NULL);
    q->notFull = (pthread_cond_t *) malloc (sizeof (pthread_cond_t));
    pthread_cond_init (q->notFull, NULL);
    q->notEmpty = (pthread_cond_t *) malloc (sizeof (pthread_cond_t));
    pthread_cond_init (q->notEmpty, NULL);
        
    return (q);
}

void queueDelete (queue *q)
{
    pthread_mutex_destroy (q->mut);
    free (q->mut);	
    pthread_cond_destroy (q->notFull);
    free (q->notFull);
    pthread_cond_destroy (q->notEmpty);
    free (q->notEmpty);
    free (q);
}

void queueAdd (queue *q, struct WorkFunction in)
{
    q->buf[q->tail] = in;
    struct timeval arrivalTime;
    gettimeofday(&arrivalTime, NULL);
    q->ArrivalPickupTimeInterval[q->tail] = arrivalTime.tv_sec*1000000 + arrivalTime.tv_usec;

    q->tail++;
    if (q->tail == QUEUESIZE)
        q->tail = 0;
    if (q->tail == q->head)
        q->full = 1;
    q->empty = 0;

    return;
}

void queueDel (queue *q, struct WorkFunction* out)
{
    *out = q->buf[q->head];
    struct timeval pickupTime;
    gettimeofday(&pickupTime, NULL);
    ArrivalPickupTimeIntervalSum += (pickupTime.tv_sec*1000000 + pickupTime.tv_usec) - q->ArrivalPickupTimeInterval[q->head];

    q->head++;
    if (q->head == QUEUESIZE)
        q->head = 0;
    if (q->head == q->tail)
        q->empty = 1;
    q->full = 0;

    return;
}
