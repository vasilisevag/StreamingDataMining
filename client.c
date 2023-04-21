#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#define KGRN "\033[0;32;32m"
#define KRED "\033[0;32;31m"
#define KYEL "\033[1;33m"
#define KBLU "\033[0;32;34m"
#define RESET "\033[0m"

#define THREADS_QUEUE_SIZE 1000
#define CONSUMERS_TOTAL 3
#define TRADING_INFORMATION_BUFFER_SIZE 40000
#define SYMBOLS_TOTAL 4
#define TIME_WINDOW 15
#define CANDLESTICKS_BUFFER_SIZE TIME_WINDOW * SYMBOLS_TOTAL
#define FIELDS_TOTAL 4
#define NANOSECONDS_PER_SECOND 1000000000ULL
#define NANOSECONDS_PER_MICROSECOND 1000
#define SLEEP_TIME_IN_SECONDS 60
#define SLEEP_TIME_IN_NANOSECONDS SLEEP_TIME_IN_SECONDS * NANOSECONDS_PER_SECOND

#define TRUE (1==1)
#define FALSE !TRUE

typedef int BOOL;

struct timeval initialTime;

void sigtstp_handler(int sig){
    //printf("\n%lld", ArrivalPickupTimeIntervalSum);
    exit(1);
}

void* SaveTradingInformation(void*);
void* UpdateTradingInformationFiles(void* tradingInfo);
void* UpdateCandleSticks(void *tradingInformation);

static int destroy_flag = 0;
static int connection_flag = 0;
static int writeable_flag = 0;


struct WorkFunction{
    void* (*work)(void*);
    void* arg;
};


typedef struct {
    long long int ArrivalPickupTimeInterval[THREADS_QUEUE_SIZE];
    struct WorkFunction buf[THREADS_QUEUE_SIZE];
    long head, tail;
    int full, empty;
    pthread_mutex_t *mut;
    pthread_cond_t *notFull, *notEmpty;
} queue;


queue* threadsQueueFIFO;


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
    if (q->tail == THREADS_QUEUE_SIZE)
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
    long long int arrivalPickupTimeInterval = (pickupTime.tv_sec*1000000 + pickupTime.tv_usec) - q->ArrivalPickupTimeInterval[q->head];

    // save response time in a file so that we can collect time data
    FILE* out_file = fopen("tradingInformationTimeData", "a");
    fprintf(out_file, "%lld\n", arrivalPickupTimeInterval);
    fclose(out_file);
    // 

    q->head++;
    if (q->head == THREADS_QUEUE_SIZE)
        q->head = 0;
    if (q->head == q->tail)
        q->empty = 1;
    q->full = 0;

    return;
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
        queueDel(fifo, &workFunction);
        pthread_mutex_unlock (fifo->mut);
        pthread_cond_signal (fifo->notFull);

        workFunction.work(workFunction.arg);
    }
    
    return NULL;
}

static void INT_HANDLER(int signo){
    destroy_flag = 1;
}

pthread_t tradingInformationThread;

struct lejp_ctx ctx;

struct TradingInformation{
    float price;
    char symbol[20];
    unsigned long timestrap;
    float volume;
};

struct TradingInformationBuffer{
    unsigned int bufferBack;
    unsigned int bufferFront;
    unsigned int counterIndex;
    struct TradingInformation buffer[TRADING_INFORMATION_BUFFER_SIZE];
};

struct TradingInformationBuffer tradingInformationBuffer;


struct CandleStick{
    float initialPrice, finalPrice, maximumPrice, minimumPrice;
    float totalVolume;
    float totalTransactionDollars;
    unsigned long long transactionsTotal;
};

struct CandleSticksBuffer{
    BOOL isFull;
    unsigned int bufferBack;
    struct CandleStick buffer[CANDLESTICKS_BUFFER_SIZE];
};

struct CandleSticksBuffer candleSticksBuffer;

void InitializeCandleSticksBuffer(struct CandleSticksBuffer* candleSticksBuffer){
    candleSticksBuffer->isFull = FALSE;
    candleSticksBuffer->bufferBack = 0;
    InitializeCandleStick(candleSticksBuffer);
}

void UpdateCandleSticksBufferIndex(struct CandleSticksBuffer* candleSticksBuffer){
    candleSticksBuffer->bufferBack += 1;
    if(candleSticksBuffer->bufferBack == TIME_WINDOW){
        candleSticksBuffer->isFull = TRUE;
    }
    candleSticksBuffer->bufferBack %= TIME_WINDOW;
}

void InitializeTradingInformationBuffer(struct TradingInformationBuffer *tradingInformationBuffer){
    tradingInformationBuffer->bufferBack = 0;
    tradingInformationBuffer->bufferFront = 0;
    tradingInformationBuffer->counterIndex = 0;
}

void UpdateTradingInformationBufferIndex(struct TradingInformationBuffer *tradingInformationBuffer){
    tradingInformationBuffer->bufferBack = (tradingInformationBuffer->bufferBack + 1) % TRADING_INFORMATION_BUFFER_SIZE;
    if(tradingInformationBuffer->bufferBack == tradingInformationBuffer->bufferFront)
        printf("---BUFFER OVERFLOW---\n");
}

void UpdateTradingInformationBufferCounter(struct TradingInformationBuffer *tradingInformationBuffer){
    tradingInformationBuffer->counterIndex++;
    if(tradingInformationBuffer->counterIndex == FIELDS_TOTAL){
        tradingInformationBuffer->counterIndex = 0;
        UpdateTradingInformationBufferIndex(tradingInformationBuffer);
    }
}

void TradingInformationBufferPushback(struct TradingInformationBuffer* tradingInformationBuffer, char* data){
    switch (tradingInformationBuffer->counterIndex){
        case 0: (tradingInformationBuffer->buffer)[tradingInformationBuffer->bufferBack].price = atof(data);
            break;
        case 1: strcpy((tradingInformationBuffer->buffer)[tradingInformationBuffer->bufferBack].symbol, data); 
            break;
        case 2: (tradingInformationBuffer->buffer)[tradingInformationBuffer->bufferBack].timestrap = atoi(data);
            break;
        case 3: (tradingInformationBuffer->buffer)[tradingInformationBuffer->bufferBack].volume = atof(data);
            break;
    }
    UpdateTradingInformationBufferCounter(tradingInformationBuffer);
}

void InitializeCandleStick(struct CandleSticksBuffer* candleSticksBuffer){
    for(int symbolIdx = 0; symbolIdx < SYMBOLS_TOTAL; symbolIdx++){
        int index = symbolIdx * TIME_WINDOW + candleSticksBuffer->bufferBack;

        candleSticksBuffer->buffer[index].finalPrice = 0;
        candleSticksBuffer->buffer[index].initialPrice = 0;
        candleSticksBuffer->buffer[index].maximumPrice = __FLT_MIN__;
        candleSticksBuffer->buffer[index].minimumPrice = __FLT_MAX__;
        candleSticksBuffer->buffer[index].totalVolume = 0;
        candleSticksBuffer->buffer[index].totalTransactionDollars = 0;
        candleSticksBuffer->buffer[index].transactionsTotal = 0;
    }
}


const char * symbols[] = {"APPL\0", "AMZN\0", "BINANCE:BTCUSDT\0", "IC MARKETS:1\0"}; 

static const char * const tok[] = {"data[].s", "data[].p", "data[].t", "data[].v"};

static const char * const reason_names[] = {
        "LEJPCB_CONSTRUCTED",
        "LEJPCB_DESTRUCTED",
        "LEJPCB_START",
        "LEJPCB_COMPLETE",
        "LEJPCB_FAILED",
        "LEJPCB_PAIR_NAME",
        "LEJPCB_VAL_TRUE",
        "LEJPCB_VAL_FALSE",
        "LEJPCB_VAL_NULL",
        "LEJPCB_VAL_NUM_INT",
        "LEJPCB_VAL_NUM_FLOAT",
        "LEJPCB_VAL_STR_START",
        "LEJPCB_VAL_STR_CHUNK",
        "LEJPCB_VAL_STR_END",
        "LEJPCB_ARRAY_START",
        "LEJPCB_ARRAY_END",
        "LEJPCB_OBJECT_START",
        "LEJPCB_OBJECT_END",
        "LEJPCB_OBJECT_END_PRE",
};


static int websocket_write_back(struct lws *wsi_in, char *str, int str_size_in) 
{
    if (str == NULL || wsi_in == NULL) return -1;

    int n, len;
    char *out = NULL;

    if (str_size_in < 1) len = strlen(str);
    else len = str_size_in;

    out = (char *)malloc(sizeof(char)*(LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING));
    memcpy (out + LWS_SEND_BUFFER_PRE_PADDING, str, len );
    n = lws_write(wsi_in, out + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);
    printf(KBLU"[websocket_write_back] %s\n"RESET, str);

    free(out);

    return n;
}


static signed char cb(struct lejp_ctx *ctx, char reason)
{
    static int counter = 0;
    if (reason & LEJP_FLAG_CB_IS_VALUE && (ctx->path_match > 0)) { 
        TradingInformationBufferPushback(&tradingInformationBuffer, ctx->buf);
        if(tradingInformationBuffer.counterIndex == 0){ // if we got a whole packet
            struct WorkFunction WorkFunctionTrading;
            struct WorkFunction WorkFunctionCandlestick;
            WorkFunctionTrading.work = UpdateTradingInformationFiles;
            WorkFunctionTrading.arg = &tradingInformationBuffer.buffer[tradingInformationBuffer.bufferBack ? tradingInformationBuffer.bufferBack - 1 : TRADING_INFORMATION_BUFFER_SIZE - 1];
            WorkFunctionCandlestick.work = UpdateCandleSticks;
            WorkFunctionCandlestick.arg = &tradingInformationBuffer.buffer[tradingInformationBuffer.bufferBack ? tradingInformationBuffer.bufferBack - 1 : TRADING_INFORMATION_BUFFER_SIZE - 1];
            pthread_mutex_lock(threadsQueueFIFO->mut);
            while(threadsQueueFIFO->full)
                pthread_cond_wait(threadsQueueFIFO->notFull, threadsQueueFIFO->mut);
            queueAdd(threadsQueueFIFO, WorkFunctionTrading);
            while(threadsQueueFIFO->full)
                pthread_cond_wait(threadsQueueFIFO->notFull, threadsQueueFIFO->mut);
            queueAdd(threadsQueueFIFO, WorkFunctionCandlestick);
            pthread_mutex_unlock(threadsQueueFIFO->mut);
            pthread_cond_signal(threadsQueueFIFO->notEmpty);
        }
    }
    if (reason == LEJPCB_COMPLETE) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
    }
}


void CallBackClientEstablished(struct lws *wsi){
    printf(KYEL"[Main Service] Connect with server success.\n"RESET);
    connection_flag = 1;
	lws_callback_on_writable(wsi);
    
    gettimeofday(&initialTime, NULL); 
    // create the first child thread that is resposible for saving trading information and candlesticks in files.
    pthread_create(&tradingInformationThread, NULL, SaveTradingInformation, NULL); 
}


void CallBackClientConnectionError(void *in){
    printf(KRED"[Main Service] Connect with server error: %s.\n"RESET, in);
    destroy_flag = 1;
    connection_flag = 0;
}


void CallBackClosed(){
    printf(KYEL"[Main Service] LWS_CALLBACK_CLOSED\n"RESET);
    destroy_flag = 1;
    connection_flag = 0;
    lejp_destruct(&ctx);
}


void CallBackClientWriteable(struct lws *wsi){
    printf(KYEL"[Main Service] On writeable is called.\n"RESET);
    char* out = NULL;
    char str[50];
            
    for(int i = 0; i < 4; i++) {
        sprintf(str, "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symbols[i]);
        int len = strlen(str);
        out = (char *)malloc(sizeof(char)*(LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING ));
        memcpy(out + LWS_SEND_BUFFER_PRE_PADDING, str, len); 
        lws_write(wsi, out+LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);
    }

	free(out);
    writeable_flag = 1;
}


void CallBackClientReceive(void *in){
    lejp_construct(&ctx, cb, NULL, tok, LWS_ARRAY_SIZE(tok));
    char *message = (char*)in;
    lejp_parse(&ctx, message, strlen(message));
}


static int ws_service_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            CallBackClientEstablished(wsi);
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            CallBackClientConnectionError(in);
            break;

        case LWS_CALLBACK_CLOSED:
            CallBackClosed();        
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            CallBackClientReceive(in); // this is were parsing takes place
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE :
            CallBackClientWriteable(wsi);
            break;

        default:
            ;
    }

    return 0;
}


static struct lws_protocols protocols[] = {{"example-protocol", ws_service_callback,}, { NULL, NULL, 0, 0 }};

void* UpdateCandleSticks(void *tradingInformation){
    struct TradingInformation* tradingInformationCurrent = (struct TradingInformation*)tradingInformation; 
    
    int symbolIdx;
    for(symbolIdx = 0; symbolIdx < SYMBOLS_TOTAL; symbolIdx++)
        if(strcmp(tradingInformationCurrent->symbol, symbols[symbolIdx]) == 0)
            break;

    
    int index = TIME_WINDOW * symbolIdx + candleSticksBuffer.bufferBack;

    if(candleSticksBuffer.buffer[index].initialPrice == 0) candleSticksBuffer.buffer[index].initialPrice = tradingInformationCurrent->price;
    if(tradingInformationCurrent->price > candleSticksBuffer.buffer[index].maximumPrice) candleSticksBuffer.buffer[index].maximumPrice = tradingInformationCurrent->price;
    if(tradingInformationCurrent->price < candleSticksBuffer.buffer[index].minimumPrice) candleSticksBuffer.buffer[index].minimumPrice = tradingInformationCurrent->price;
    candleSticksBuffer.buffer[index].finalPrice = tradingInformationCurrent->price;
    candleSticksBuffer.buffer[index].totalVolume += tradingInformationCurrent->volume;
    candleSticksBuffer.buffer[index].totalTransactionDollars += tradingInformationCurrent->volume * tradingInformationCurrent->price;
    candleSticksBuffer.buffer[index].transactionsTotal += 1;
}

void* UpdateTradingInformationFiles(void* tradingInfo){
    struct TradingInformation *tradingInformation = (struct TradingInformation*)tradingInfo;
    char filename[100] = "./";
    char fileTrailingName[40] = "_trading_information.txt";

    int symbolIdx;
    for(symbolIdx = 0; symbolIdx < SYMBOLS_TOTAL; symbolIdx++)
        if(strcmp(tradingInformation->symbol, symbols[symbolIdx]) == 0)
            break;

    strcat(filename, symbols[symbolIdx]);
    strcat(filename, "/");
    strcat(filename, symbols[symbolIdx]);
    strcat(filename, fileTrailingName);
    FILE* out_file = fopen(filename, "a");
    fprintf(out_file, "value: %f\tsymbol: %s\ttimestrap: %d\tvolume: %f\n", tradingInformation->price, tradingInformation->symbol, tradingInformation->timestrap, tradingInformation->volume);
    fclose(out_file);
}

void UpdateCandleSticksFiles(struct CandleStick *candleSticks){
    for(int symbolIdx = 0; symbolIdx < SYMBOLS_TOTAL; symbolIdx++){
        char fileTrailingName[40] = "_candlesticks.txt";
        char filename[100] = "./";
        strcat(filename, symbols[symbolIdx]);
        strcat(filename, "/");
        strcat(filename, symbols[symbolIdx]);
        strcat(filename, fileTrailingName);
        FILE* out_file = fopen(filename, "a");
        int index = symbolIdx * TIME_WINDOW + (candleSticksBuffer.bufferBack ? candleSticksBuffer.bufferBack - 1 : TIME_WINDOW - 1);
        fprintf(out_file, "initial price: %f\tfinal price: %f\tmax price: %f\tmin price: %f\t total volume: %f\n", candleSticks[index].initialPrice, candleSticks[index].finalPrice, candleSticks[index].maximumPrice, candleSticks[index].minimumPrice, candleSticks[index].totalVolume);
        fclose(out_file);
    }
}

void UpdateMovingAverageFiles(struct CandleSticksBuffer* candleSticksBuffer){
    unsigned long long transactionsTotal[SYMBOLS_TOTAL] = {0};
    float movingAverageTotalVolume[SYMBOLS_TOTAL] = {0};
    float movingAverageTotalTransactionDollars[SYMBOLS_TOTAL] = {0};
    
    int maxIndex = (candleSticksBuffer->isFull ? TIME_WINDOW -1 : candleSticksBuffer->bufferBack - 1);
    for(int symbolIdx = 0; symbolIdx < SYMBOLS_TOTAL; symbolIdx++){
        for(int index = 0; index <= maxIndex; index++){
            movingAverageTotalVolume[symbolIdx] += candleSticksBuffer->buffer[symbolIdx * TIME_WINDOW + index].totalVolume * candleSticksBuffer->buffer[symbolIdx * TIME_WINDOW + index].transactionsTotal;
            movingAverageTotalTransactionDollars[symbolIdx] += candleSticksBuffer->buffer[symbolIdx * TIME_WINDOW + index].totalTransactionDollars * candleSticksBuffer->buffer[symbolIdx * TIME_WINDOW + index].transactionsTotal;
            transactionsTotal[symbolIdx] += candleSticksBuffer->buffer[symbolIdx * TIME_WINDOW + index].transactionsTotal;
        }
        char fileTrailingName[40] = "_moving_average.txt";
        char filename[100] = "./";
        strcat(filename, symbols[symbolIdx]);
        strcat(filename, "/");
        strcat(filename, symbols[symbolIdx]);
        strcat(filename, fileTrailingName);
        FILE* out_file = fopen(filename, "a");
        fprintf(out_file, "total volume: %f\ttotal transactions in dollars: %f\t\n", (transactionsTotal[symbolIdx] ? movingAverageTotalVolume[symbolIdx] / transactionsTotal[symbolIdx] : .0), movingAverageTotalTransactionDollars[symbolIdx]/(maxIndex+1));
        fclose(out_file);
    }
}

void ExtractDataFromBuffer(){
    unsigned int back = tradingInformationBuffer.bufferBack;
    unsigned int *front = &tradingInformationBuffer.bufferFront;
    *front = back;

    UpdateCandleSticksBufferIndex(&candleSticksBuffer);
    InitializeCandleStick(&candleSticksBuffer);
    
    UpdateCandleSticksFiles(candleSticksBuffer.buffer);
    UpdateMovingAverageFiles(&candleSticksBuffer);
}

CalculateSleepTime(struct timespec *sleepTime, long long int minuteCount){
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);

    long long currentTimeInNanoSeconds = currentTime.tv_sec * NANOSECONDS_PER_SECOND + currentTime.tv_usec * NANOSECONDS_PER_MICROSECOND;
    long long wakeUpTimeInNanoSeconds = (initialTime.tv_sec + SLEEP_TIME_IN_SECONDS * minuteCount) * NANOSECONDS_PER_SECOND + initialTime.tv_usec * NANOSECONDS_PER_MICROSECOND;
    long long remainingSleepTimeInNanoSeconds = wakeUpTimeInNanoSeconds - currentTimeInNanoSeconds;
    sleepTime->tv_sec = remainingSleepTimeInNanoSeconds / NANOSECONDS_PER_SECOND;
    sleepTime->tv_nsec = remainingSleepTimeInNanoSeconds - sleepTime->tv_sec * NANOSECONDS_PER_SECOND;
}

void InitializeSleepTime(struct timespec *sleepTime){
    sleepTime->tv_sec = SLEEP_TIME_IN_SECONDS;
    sleepTime->tv_nsec = 0;
}

void BlockThread(struct timespec* sleepTime){
    struct timespec remainingTime;
    int wokeUpEarly;
    do{
        wokeUpEarly = nanosleep(sleepTime, &remainingTime);
        *sleepTime = remainingTime;
    }while(wokeUpEarly == -1);
}

void* SaveTradingInformation(void* args){
    static long long int minuteCount = 0;
    
    struct timespec sleepTime;
    struct timeval currentTime;
    
    InitializeSleepTime(&sleepTime);
    while(TRUE){
        BlockThread(&sleepTime); // blocks thread for the given time
        
        minuteCount++; // calculate and save response time 
        gettimeofday(&currentTime, NULL);
        long long int responseTime = (currentTime.tv_sec - (initialTime.tv_sec + SLEEP_TIME_IN_SECONDS * minuteCount)) * 1000000 + (currentTime.tv_usec - initialTime.tv_usec); 
        FILE* out_file = fopen("FilesTimeData", "a");
        fprintf(out_file, "%lld\n", responseTime);
        fclose(out_file);   

        ExtractDataFromBuffer();

        CalculateSleepTime(&sleepTime, minuteCount + 1);
    }
}

int main(void) {
    struct sigaction act;
    act.sa_handler = INT_HANDLER;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction( SIGINT, &act, 0);

    struct stat sb; // create one file for each symbol
    for(int i = 0; i < SYMBOLS_TOTAL; i++)
        if(!(stat(symbols[i], &sb) == 0 && S_ISDIR(sb.st_mode))) // check if the file already exists
            mkdir(symbols[i], 0777);

    signal(SIGTSTP, sigtstp_handler);
    pthread_t consumers[CONSUMERS_TOTAL]; // create consumers
    
    threadsQueueFIFO = queueInit();
    for(int consumerIdx = 0; consumerIdx < CONSUMERS_TOTAL; consumerIdx++){
        pthread_create(&consumers[consumerIdx], NULL, consumer, threadsQueueFIFO);
    }

    InitializeTradingInformationBuffer(&tradingInformationBuffer); // initialize trading information buffer
    InitializeCandleSticksBuffer(&candleSticksBuffer); // initialize candlesticks buffer

    struct lws_context *context = NULL;
    struct lws_context_creation_info info;
    struct lws *wsi = NULL;
    struct lws_protocols protocol;
    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    char inputURL[300] = "ws.finnhub.io/?token=ccb1bl2ad3i49r0ulrs0";
    const char *urlProtocol, *urlTempPath;
	char urlPath[300];
    context = lws_create_context(&info);
    printf(KRED"[Main] context created.\n"RESET);

    if (context == NULL) {
        printf(KRED"[Main] context is NULL.\n"RESET);
        return -1;
    }

    struct lws_client_connect_info clientConnectionInfo;
    memset(&clientConnectionInfo, 0, sizeof(clientConnectionInfo));
    clientConnectionInfo.context = context;
    
    if (lws_parse_uri(inputURL, &urlProtocol, &clientConnectionInfo.address, &clientConnectionInfo.port, &urlTempPath))
        printf("Couldn't parse URL\n");

    urlPath[0] = '/';
    strncpy(urlPath + 1, urlTempPath, sizeof(urlPath) - 2);
    urlPath[sizeof(urlPath)-1] = '\0';
    clientConnectionInfo.port = 443;
    clientConnectionInfo.path = urlPath;
    clientConnectionInfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;    
    clientConnectionInfo.host = clientConnectionInfo.address;
    clientConnectionInfo.origin = clientConnectionInfo.address;
    clientConnectionInfo.ietf_version_or_minus_one = -1;
    clientConnectionInfo.protocol = protocols[0].name;
    printf("Testing %s\n\n", clientConnectionInfo.address);
    printf("Connecticting to %s://%s:%d%s \n\n", urlProtocol, 
    clientConnectionInfo.address, clientConnectionInfo.port, urlPath);    
    wsi = lws_client_connect_via_info(&clientConnectionInfo);

    if (wsi == NULL) {
        printf(KRED"[Main] wsi create error.\n"RESET);
        return -1;
    }

    printf(KGRN"[Main] wsi create success.\n"RESET);

    while(!destroy_flag) 
        lws_service(context, 50);
    
    lws_context_destroy(context);

    pthread_join(tradingInformationThread, NULL);
    for(int consumerIdx = 0; consumerIdx < CONSUMERS_TOTAL; consumerIdx++){
        pthread_join(consumers[consumerIdx], NULL);
    }
    queueDelete(threadsQueueFIFO);

    return 0;
}
