#pragma once
#include <curl/curl.h>
#include <stdint.h>

#define VERBOSE_OUTPUT_OFF 0
#define VERBOSE_OUTPUT_ON  1

#define VERIFY_CERTS_OFF 0
#define VERIFY_CERTS_ON  1

//#define DEBUG_CURL

static const char g_content_json[]         = "application/json";
static const char g_content_xml[]          = "application/xml";
static const char g_content_octet_stream[] = "application/octet-stream";
static const char g_content_text_html[]    = "text/html";

typedef enum
{
    Session_Static = 0,
    Session_Polling,
    Session_Stream,
} CURL_SESSION_TYPE;

typedef struct CURL_Static_Session_Data
{
    CURL_SESSION_TYPE sessionType;
    CURL* curlHandle;
    char* username;
    int usernameLength;
    char* password;
    int passwordLength;
    char* basePath;
    int basePathLength;
    char* readCallbackData;
    int readCallbackDataLength;
    int readCallbackBufferSize;
    char* writeCallbackData;
    int writeCallbackDataLength;
    int writeCallbackBufferSize;
} CURL_Static_Session_Data;

typedef struct CURL_Stream_Thread_Data
{
    pthread_t tid;
    int thread_ref;
    int stopFlag;
} CURL_Stream_Thread_Data;

typedef struct CURL_Stream_Session_Data
{
    CURL_SESSION_TYPE sessionType;
    CURL* curlHandle;
    CURLM* multiHandle;
    CURL_Stream_Thread_Data threadData;
    char* username;
    int usernameLength;
    char* password;
    int passwordLength;
    char* basePath;
    int basePathLength;
    char* dataBuffer;
    int dataBufferSize;
    int bufferReadIndex;
    int64_t bufferReadCounter;
    int bufferWriteIndex;
    int64_t bufferWriteCounter;
} CURL_Stream_Session_Data;

typedef struct CURL_Stream_Read_Data
{
    char* dataChunk;
    int dataChunkSize;
    int remainingData;   //data remaining in buffer
} CURL_Stream_Read_Data;

typedef struct _URL_DATA
{
    char url[1025];
    char scheme[10];      // http, ftp, etc
    char hostname[256];   // 255 for DNS name max + NULL
    char port[6];
    char username[256];
    char password[256];
    char path[1025];
    // int isAutoReboot;
    // char downloadFileName[FILENAME_MAX];
} URL_DATA, *PURL_DATA;

typedef struct _UPGRADE_DATA
{
    URL_DATA urlData;
    int statusCode;
    int isAutoReboot;
    char downloadFileName[FILENAME_MAX];
} UPGRADE_DATA, *PUPGRADE_DATA;

typedef struct _UPLOAD_DATA
{
    const uint8_t* readptr;
    size_t sizeleft;
} UPLOAD_DATA;

void
curlGlobalInit();

void
curlStreamBufferReadout(
    CURL_Stream_Session_Data* session_data);

size_t
curlStaticDataReadCallback(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userp);

size_t
curlStreamDataReadCallback(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userp);

size_t
curlDummyCallback(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userp);

CURL_Static_Session_Data*
curlStaticInit(
    const char* username,
    const char* password,
    const char* basePath,
    CURL_SESSION_TYPE type,
    int EnableVerify,
    long verboseOutput);

CURL_Stream_Session_Data*
curlStreamInit(
    const char* username,
    const char* password,
    const char* basePath,
    CURL_SESSION_TYPE type,
    int EnableVerify,
    long verboseOutput);

CURL_Stream_Read_Data
curlStreamReadBufferChunk(
    CURL_Stream_Session_Data* session_data);

void*
curlStreamReader(
    void* sessionData);

int
curlStreamSpawnReaderThread(
    CURL_Stream_Session_Data* session_data);

int
curlStreamStopThread(
    CURL_Stream_Session_Data* session_data);

void
curlStreamResetThread(
    CURL_Stream_Session_Data* session_data);

char*
curlStaticGet(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    int* statusCode);

char*
curlStaticPost(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    char* postData,
    int* statusCode);

char*
curlPostUploadFile(
    CURL_Static_Session_Data* Session_data,
    char* Endpoint,
    PUPGRADE_DATA UpgradeData,
    int* StatusCode,
    char* Buffer);

char*
curlStaticPut(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    char* putData,
    int* statusCode);

char*
curlStaticDelete(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    int* statusCode);

void
curlStaticCleanup(
    CURL_Static_Session_Data* session_data);

void
curlStreamCleanup(
    CURL_Stream_Session_Data* session_data);

void
curlGlobalCleanup();

size_t
curlStaticDataWriteCallback(
    void* write_data,
    size_t size,
    size_t nmemb,
    void* userp);

int
curlGetDownload(
    PUPGRADE_DATA UpgradeData);

int
curlGet(
    PURL_DATA GetData,
    char* Buffer);

int
curlPost(
    PURL_DATA PostData);

int
curlPut(
    PURL_DATA PutData,
    char* Payload);

size_t
curlWriteCallback(
    char* ptr,
    size_t size,
    size_t nmemb,
    void* userdata);