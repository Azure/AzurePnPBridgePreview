#include "curl_wrapper.h"
#include "curl/curl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <strings.h>
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/crt_abstractions.h"

void
curlGlobalInit()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

//
// Setup common options for CURL
//
struct curl_slist*
curlSetOpt(
    CURL* Curl,
    char* Username,
    char* Password,
    char* Url,
    char* Method)
{
    struct curl_slist* headers = NULL;
    if (Username && strlen(Username) > 0)
    {
        curl_easy_setopt(Curl, CURLOPT_USERNAME, Username);
    }
    if (Password && strlen(Password) > 0)
    {
        curl_easy_setopt(Curl, CURLOPT_PASSWORD, Password);
    }
    curl_easy_setopt(Curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(Curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(Curl, CURLOPT_NOSIGNAL, 1L);
    // curl_easy_setopt(Curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(Curl, CURLOPT_URL, Url);
    curl_easy_setopt(Curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(Curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(Curl, CURLOPT_CUSTOMREQUEST, Method);

    curl_easy_setopt(Curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(Curl, CURLOPT_POST, 0L);
    curl_easy_setopt(Curl, CURLOPT_POSTFIELDS, "");

    headers = curl_slist_append(headers, "Accept: application/json");
    //headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(Curl, CURLOPT_HTTPHEADER, headers);
}

//
// Checks CURL Code, get response code, and check content type
//
int
curlCheckResult(
    CURL* Curl,
    CURLcode CurlCode,
    const char* ContentTypeExpected)
{
    int httpStatus;
    CURLcode curlCode;
    char* contentType;

    if (CurlCode != CURLE_OK)
    {
        if (CurlCode == CURLE_COULDNT_CONNECT)
        {
            // This can happen if the device is rebooting..
            httpStatus = 503;   // Service Unavailable
        }
        else
        {
            LogError("R700 : curl_easy_perform() failed : %s (CurlCode %d)",
                     curl_easy_strerror(CurlCode),
                     CurlCode);
            httpStatus = 500;
        }
    }
    else if (curlCode = curl_easy_getinfo(Curl, CURLINFO_RESPONSE_CODE, &httpStatus) != CURLE_OK)
    {
        LogError("R700 : curl_easy_getinfo() failed for CURLINFO_RESPONSE_CODE : %s (CurlCode %d)",
                 curl_easy_strerror(curlCode),
                 curlCode);
    }
    else if (curlCode = curl_easy_getinfo(Curl, CURLINFO_CONTENT_TYPE, &contentType) != CURLE_OK)
    {
        LogError("R700 : curl_easy_getinfo() failed for CURLINFO_CONTENT_TYPE : %s (CurlCode %d)",
                 curl_easy_strerror(curlCode),
                 curlCode);
    }
    else if ((contentType != NULL) && (strcmp(contentType, ContentTypeExpected) != 0))
    {

        if (httpStatus == 404 && (strcmp(contentType, g_content_text_html) == 0))
        {
            // this means reader is not ready to accept REST calls
            httpStatus = 503;
        }
        else
        {
            LogError("R700 : response content is not '%s' but %s.  Http Status %d", ContentTypeExpected, contentType, httpStatus);
            // We can only process json
            httpStatus = 204;
        }
    }
    return httpStatus;
}

size_t
curlStaticDataReadCallback(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userp)
{

    size_t realsize = size * nmemb;

    struct CURL_Static_Session_Data* session_data = (struct CURL_Static_Session_Data*)userp;

    memcpy(session_data->readCallbackData, contents, nmemb);
    session_data->readCallbackDataLength = (int)nmemb;

    return nmemb;
}

CURL_Stream_Read_Data
curlStreamReadBufferChunk(
    CURL_Stream_Session_Data* session_data)
{
    CURL_Stream_Read_Data read_data;

    if (session_data->bufferReadIndex == session_data->bufferWriteIndex)
    {
        read_data.dataChunk     = NULL;
        read_data.dataChunkSize = 0;
        read_data.remainingData = session_data->bufferWriteCounter - session_data->bufferReadCounter;
        return read_data;
    }

    char* dataChunk = session_data->dataBuffer + session_data->bufferReadIndex;   // read first chunk of data from buffer

    if ((strlen(dataChunk) + session_data->bufferReadIndex) >= (session_data->dataBufferSize - 1))
    {   // if the data chunk wraps around end of buffer space, need to reassemble

        int overrun          = strlen(dataChunk) + session_data->bufferReadIndex - (session_data->dataBufferSize);
        int firstChunkLength = strlen(dataChunk) - overrun;
        char* firstDataChunk = (char*)malloc(sizeof(char) * strlen(dataChunk) + 1);
        memcpy(firstDataChunk, dataChunk, firstChunkLength);

        char* secondDataChunk = session_data->dataBuffer;
        int fullJoinedLength  = strlen(firstDataChunk) + strlen(secondDataChunk);
        char* joinedDataChunk = (char*)malloc(sizeof(char) * fullJoinedLength);
        strcat(joinedDataChunk, firstDataChunk);
        strcat(joinedDataChunk, secondDataChunk);

        session_data->bufferReadIndex = strlen(secondDataChunk) + 1;

        char* joinedDataChunkReturn = NULL;

        mallocAndStrcpy_s(&joinedDataChunkReturn, joinedDataChunk);

        session_data->bufferReadCounter += firstChunkLength + strlen(secondDataChunk) + 1;

        if (session_data->bufferReadCounter % session_data->dataBufferSize != session_data->bufferReadIndex)
        {
            read_data.dataChunk     = NULL;
            read_data.dataChunkSize = 0;
            read_data.remainingData = session_data->bufferWriteCounter - session_data->bufferReadCounter;

            LogInfo("R700 :\n  ERROR: Stream data buffer read index mismatch.");

            return read_data;
        }

        // LogInfo("R700 :\n JOINED DATA: %s", joinedDataChunkReturn);
        // LogInfo("R700 :\n   POST READ - BufferWriteIndex: %d, BufferReadIndex: %d", session_data->bufferWriteIndex, session_data->bufferReadIndex);

        free(firstDataChunk);
        free(joinedDataChunk);

        read_data.dataChunk     = joinedDataChunkReturn;
        read_data.dataChunkSize = strlen(joinedDataChunkReturn);
        read_data.remainingData = session_data->bufferWriteCounter - session_data->bufferReadCounter;

        return read_data;
    }

    else
    {   // if the chunk doesn't wrap around end of memory space, it should all be contained in first read
        session_data->bufferReadIndex += strlen(dataChunk) + 1;
        session_data->bufferReadCounter += strlen(dataChunk) + 1;

        // LogInfo("R700 :\n   POST READ - BufferWriteIndex: %d, BufferReadIndex: %d", session_data->bufferWriteIndex, session_data->bufferReadIndex);
        read_data.dataChunk     = dataChunk;
        read_data.dataChunkSize = strlen(dataChunk);
        read_data.remainingData = session_data->bufferWriteCounter - session_data->bufferReadCounter;

        return read_data;
    }
}

void
curlStreamBufferReadout(
    CURL_Stream_Session_Data* session_data)
{

    LogInfo("R700 :\n BUFFER DATA: ");
    for (int i = 0; i < session_data->dataBufferSize; i++)
        LogInfo("R700 :%c", *(session_data->dataBuffer + i));
    LogInfo("R700 :");
}

size_t
curlStreamDataReadCallback(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userp)
{
    size_t realsize = size * nmemb;

    struct CURL_Stream_Session_Data* session_data = (struct CURL_Stream_Session_Data*)userp;   // cast input pointer to session data

    if (((session_data->bufferWriteIndex + 1) % session_data->dataBufferSize) == session_data->dataBufferSize)
    {
        LogError("R700 : Stream data buffer overflow.");
        return -1;
    }

    // memset(session_data->dataBuffer, 'X', session_data->dataBufferSize); // DEBUG

    int spaceUntilWrap = session_data->dataBufferSize - session_data->bufferWriteIndex;

    // LogInfo("R700 :\n       PRE WRITE - BufferWriteIndex: %d, BufferReadIndex: %d, SpaceUntilWrap: %d, BufferSize: %d", session_data->bufferWriteIndex, session_data->bufferReadIndex, spaceUntilWrap, session_data->dataBufferSize);

    if (nmemb > spaceUntilWrap)
    {   // wrap around end of buffer
        memcpy(session_data->dataBuffer + session_data->bufferWriteIndex, (char*)contents, spaceUntilWrap);
        memcpy(session_data->dataBuffer + session_data->bufferWriteIndex, (char*)contents, spaceUntilWrap);
        memcpy(session_data->dataBuffer, (char*)contents + spaceUntilWrap, nmemb - spaceUntilWrap);
        session_data->bufferWriteIndex = nmemb - spaceUntilWrap;
    }
    else
    {
        memcpy(session_data->dataBuffer + session_data->bufferWriteIndex, (char*)contents, nmemb);
        session_data->bufferWriteIndex = session_data->bufferWriteIndex + (int)nmemb;
    }

    // follow contents with null char
    memset(session_data->dataBuffer + session_data->bufferWriteIndex, '\000', 1);
    session_data->bufferWriteIndex += 1;

    session_data->bufferWriteCounter += nmemb + 1;

    int64_t bufferAllocation = session_data->bufferWriteCounter - session_data->bufferReadCounter;

    int checkWriteCounter = session_data->bufferWriteCounter % session_data->dataBufferSize;
    // LogInfo("R700 :\n WRITECOUNTCALC: %d, WRITEINDEX: %d", checkWriteCounter, session_data->bufferWriteIndex); // debug

    if (checkWriteCounter != session_data->bufferWriteIndex)
    {
        LogError("R700 :\nStream data buffer write index mismatch.");
        return -1;
    }

    if (bufferAllocation >= session_data->dataBufferSize)
    {
        LogError("R700 :\nERROR: Stream data buffer overflow (writes faster than reads).");
        return -1;
    }

    spaceUntilWrap = session_data->dataBufferSize - session_data->bufferWriteIndex;

    // LogInfo("R700 :\n      POST WRITE - BufferWriteIndex: %d, BufferReadIndex: %d, SpaceUntilWrap: %d, BufferSize: %d", session_data->bufferWriteIndex, session_data->bufferReadIndex, spaceUntilWrap, session_data->dataBufferSize);

    // print out the entire buffer character space
    // curlStreamBufferReadout(session_data); //DEBUG

    // DEBUG readout
    // char * streamDataChunk = curlStreamReadBufferChunk(session_data);
    // LogInfo("R700 :\n  StreamDataLength: %d", (int)strlen(streamDataChunk));
    // LogInfo("R700 :\n  StreamData: %s", streamDataChunk);

    return nmemb;
}

void*
curlStreamReader(
    void* sessionData)
{
    LogInfo("R700 : %s() enter : cURL stream reader thread started.", __FUNCTION__);

    CURL_Stream_Session_Data* session_data = (CURL_Stream_Session_Data*)sessionData;

    CURLMcode mc; /* curl_multi_poll() return code */
    int numfds = 0;
    //    int count         = 0;
    int still_running = 1;

    while (still_running & (session_data->threadData.stopFlag == 0))
    {
        /* we start some action by calling perform right away */
        mc = curl_multi_perform(session_data->multiHandle, &still_running);

        if (still_running & mc == CURLM_OK)
            /* wait for activity, timeout or "nothing" */
            mc = curl_multi_poll(session_data->multiHandle, NULL, 0, 100, &numfds);

        // LogInfo("R700 :\n Multipoll Thread: Iteration %d", count);
        // count++;

        if (mc != CURLM_OK)
        {
            LogError("R700\ncurl_multi_wait() failed in curlStreamReader(), code %d.", mc);
            break;
        }
    }
    LogInfo("R700 : %s() exit : cURL stream reader thread shut down.", __FUNCTION__);
}

int
curlStreamSpawnReaderThread(
    CURL_Stream_Session_Data* session_data)
{
    session_data->threadData.thread_ref = pthread_create(&(session_data->threadData.tid), NULL, curlStreamReader, (void*)session_data);
}

int
curlStreamStopThread(
    CURL_Stream_Session_Data* session_data)
{
    session_data->threadData.stopFlag = 1;
}

CURL_Static_Session_Data*
curlStaticInit(
    const char* username,
    const char* password,
    const char* basePath,
    CURL_SESSION_TYPE type,
    int EnableVerify,
    long verboseOutput)
{
    CURL* static_handle;
    static_handle       = curl_easy_init();
    char* username_copy = NULL;
    char* password_copy = NULL;
    char* basePath_copy = NULL;

#define STATIC_READ_CALLBACK_DATA_BUFFER_SIZE 10000

    static char* read_callback_data;
    read_callback_data = (char*)malloc(sizeof(char) * STATIC_READ_CALLBACK_DATA_BUFFER_SIZE);

#define STATIC_WRITE_CALLBACK_DATA_BUFFER_SIZE 10000

    static char* write_callback_data;
    write_callback_data = (char*)malloc(sizeof(char) * STATIC_WRITE_CALLBACK_DATA_BUFFER_SIZE);

    if (read_callback_data == NULL)
    {
        /* out of memory! */
        LogError("R700 : not enough memory for cURL callback data buffer (malloc returned NULL)");
        return 0;
    }

    mallocAndStrcpy_s(&username_copy, username);
    mallocAndStrcpy_s(&password_copy, password);
    mallocAndStrcpy_s(&basePath_copy, basePath);

// Set session data values
#define staticInitArraySize 10
    int structInitSizes[staticInitArraySize] = {
        sizeof(CURL*),    //curl session pointer
        sizeof(int*),     //username pointer
        sizeof(int),      //username size
        sizeof(int*),     //password pointer
        sizeof(int),      //password size
        sizeof(int*),     //basepath pointer
        sizeof(int),      //basepath size
        sizeof(char**),   //readCallbackData pointer
        sizeof(int),      //readCallbackData size
        sizeof(int)       //callbackBuffer Size
    };

    CURL_Static_Session_Data* session_data = malloc(sizeof(CURL_Static_Session_Data));

    session_data->sessionType             = type;
    session_data->curlHandle              = static_handle;
    session_data->username                = username_copy;
    session_data->usernameLength          = strlen(username_copy);
    session_data->password                = password_copy;
    session_data->passwordLength          = strlen(password_copy);
    session_data->basePath                = basePath_copy;
    session_data->basePathLength          = strlen(basePath_copy);
    session_data->readCallbackData        = read_callback_data;
    session_data->readCallbackDataLength  = 0;
    session_data->readCallbackBufferSize  = STATIC_READ_CALLBACK_DATA_BUFFER_SIZE;
    session_data->writeCallbackData       = write_callback_data;
    session_data->writeCallbackDataLength = 0;
    session_data->writeCallbackBufferSize = STATIC_WRITE_CALLBACK_DATA_BUFFER_SIZE;

    // apply curl settings with libcurl calls
    curl_easy_setopt(static_handle, CURLOPT_USERNAME, session_data->username);
    curl_easy_setopt(static_handle, CURLOPT_PASSWORD, session_data->password);
    curl_easy_setopt(static_handle, CURLOPT_WRITEFUNCTION, curlStaticDataReadCallback);
    curl_easy_setopt(static_handle, CURLOPT_WRITEDATA, (void*)session_data);
    curl_easy_setopt(static_handle, CURLOPT_SSL_VERIFYPEER, EnableVerify);
    curl_easy_setopt(static_handle, CURLOPT_SSL_VERIFYHOST, EnableVerify);
    curl_easy_setopt(static_handle, CURLOPT_SSL_VERIFYSTATUS, EnableVerify);
    curl_easy_setopt(static_handle, CURLOPT_VERBOSE, verboseOutput);

    return session_data;
}

CURL_Stream_Session_Data*
curlStreamInit(
    const char* username,
    const char* password,
    const char* basePath,
    CURL_SESSION_TYPE type,
    int EnableVerify,
    long verboseOutput)
{

    CURL* stream_handle;
    stream_handle       = curl_easy_init();
    char* username_copy = NULL;
    char* password_copy = NULL;
    char* basePath_copy = NULL;

#define STREAM_DATA_BUFFER_SIZE 100000

    char* streamBuffer;

    LogInfo("R700 : %s() enter", __FUNCTION__);

    streamBuffer = (char*)malloc(sizeof(char) * STREAM_DATA_BUFFER_SIZE);

    memset(streamBuffer, '\0', STREAM_DATA_BUFFER_SIZE);

    if (streamBuffer == NULL)
    {
        /* out of memory! */
        printf("not enough memory for cURL callback data buffer (malloc returned NULL)");
        return 0;
    }

    mallocAndStrcpy_s(&username_copy, username);
    mallocAndStrcpy_s(&password_copy, password);
    mallocAndStrcpy_s(&basePath_copy, basePath);

    // initialize session data structure
    CURL_Stream_Session_Data* session_data = malloc(sizeof(CURL_Stream_Session_Data));

    session_data->sessionType         = type;
    session_data->curlHandle          = stream_handle;
    session_data->threadData.stopFlag = 0;
    session_data->username            = username_copy;
    session_data->usernameLength      = strlen(username_copy);
    session_data->password            = password_copy;
    session_data->passwordLength      = strlen(password_copy);
    session_data->basePath            = basePath_copy;
    session_data->basePathLength      = strlen(basePath_copy);
    session_data->dataBuffer          = streamBuffer;
    session_data->dataBufferSize      = STREAM_DATA_BUFFER_SIZE;
    session_data->bufferReadIndex     = 0;
    session_data->bufferReadCounter   = 0;
    session_data->bufferWriteIndex    = 0;
    session_data->bufferWriteCounter  = 0;

    // apply curl settings with libcurl calls
    curl_easy_setopt(stream_handle, CURLOPT_USERNAME, session_data->username);
    curl_easy_setopt(stream_handle, CURLOPT_PASSWORD, session_data->password);
    curl_easy_setopt(stream_handle, CURLOPT_WRITEFUNCTION, curlStreamDataReadCallback);
    curl_easy_setopt(stream_handle, CURLOPT_WRITEDATA, (void*)session_data);
    curl_easy_setopt(stream_handle, CURLOPT_SSL_VERIFYPEER, EnableVerify);
    curl_easy_setopt(stream_handle, CURLOPT_SSL_VERIFYHOST, EnableVerify);
    curl_easy_setopt(stream_handle, CURLOPT_SSL_VERIFYSTATUS, EnableVerify);
    curl_easy_setopt(stream_handle, CURLOPT_VERBOSE, verboseOutput);

    char fullStreamUrl[1000] = "";

    strcat(fullStreamUrl, session_data->basePath);
    strcat(fullStreamUrl, "/data/stream");

    char* full_stream_endpoint = NULL;

    mallocAndStrcpy_s(&full_stream_endpoint, fullStreamUrl);

    curl_easy_setopt(session_data->curlHandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(session_data->curlHandle, CURLOPT_URL, full_stream_endpoint);
    LogInfo("R700 : STREAM Endpoint: %s", full_stream_endpoint);

    // initialize multi handle
    session_data->multiHandle = curl_multi_init();

    if (session_data->multiHandle == NULL)
    {
        LogError("R700 : Failed to create a multi handle");
    }
    CURLMcode multiRet = curl_multi_add_handle(session_data->multiHandle, session_data->curlHandle);

    if (multiRet != CURLM_OK)
    {
        LogError("R700 : Failed to add handle %d", multiRet);
    }

    if (full_stream_endpoint)
    {
        free(full_stream_endpoint);
    }

    return session_data;
}

char*
curlStaticGet(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    int* statusCode)
{
#ifdef DEBUG_CURL
    if (session_data->sessionType == Session_Static)
    {
        LogInfo("R700 : %s() %s", __FUNCTION__, endpoint);
    }
#endif
    CURL* static_handle = session_data->curlHandle;

    char* startPtr = session_data->readCallbackData;

    assert(statusCode != NULL);

    memset(startPtr, '\0', STATIC_READ_CALLBACK_DATA_BUFFER_SIZE * sizeof(char));   //re-initialize memory space

    session_data->readCallbackDataLength = 0;

    char fullurl[1000] = "";

    strcat(fullurl, session_data->basePath);
    strcat(fullurl, endpoint);

    char* full_endpoint = NULL;
    mallocAndStrcpy_s(&full_endpoint, fullurl);

#ifdef DEBUG_CURL
    if (session_data->sessionType == Session_Static)
    {
        LogInfo("R700 : GET Endpoint: '%s'", full_endpoint);   //DEBUG
    }
#endif

    curlSetOpt(static_handle, NULL, NULL, full_endpoint, "GET");

    CURLcode curlCode;

    curlCode = curl_easy_perform(static_handle);

    *statusCode = curlCheckResult(static_handle, curlCode, g_content_json);

    if (full_endpoint)
    {
        free(full_endpoint);
    }

    if (*statusCode == 204)   // No content
    {
        return NULL;
    }
    else
    {
        return session_data->readCallbackData;
    }
}

char*
curlStaticPut(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    char* putData,
    int* statusCode)
{
    CURL* static_handle = session_data->curlHandle;

#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s", __FUNCTION__, endpoint);
#endif

    // re-initialize callback data buffer
    memset(session_data->readCallbackData, '\0', STATIC_READ_CALLBACK_DATA_BUFFER_SIZE * sizeof(char));
    session_data->readCallbackDataLength = 0;

    memset(session_data->writeCallbackData, '\0', STATIC_WRITE_CALLBACK_DATA_BUFFER_SIZE * sizeof(char));
    session_data->writeCallbackDataLength = 0;

    memcpy(session_data->writeCallbackData, putData, strlen(putData) * sizeof(char));
    session_data->writeCallbackDataLength = strlen(putData);

    char fullurl[1000] = "";

    strcat(fullurl, session_data->basePath);
    strcat(fullurl, endpoint);

    char* full_endpoint = NULL;
    mallocAndStrcpy_s(&full_endpoint, fullurl);

    LogInfo("R700 : PUT Endpoint: %s", full_endpoint);

    curlSetOpt(static_handle, NULL, NULL, full_endpoint, "PUT");

    curl_easy_setopt(static_handle, CURLOPT_READFUNCTION, curlStaticDataWriteCallback);
    curl_easy_setopt(static_handle, CURLOPT_READDATA, (void*)session_data);

    if (putData)
    {
        curl_easy_setopt(static_handle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(static_handle, CURLOPT_POSTFIELDS, putData);
    }

    CURLcode curlCode = curl_easy_perform(static_handle);

    *statusCode = curlCheckResult(static_handle, curlCode, g_content_json);

    if (full_endpoint)
    {
        free(full_endpoint);
    }

    if (*statusCode == 204)   // No content
    {
        return NULL;
    }
    else
    {
        return session_data->readCallbackData;
    }
}

char*
curlStaticPost(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    char* postData,
    int* statusCode)
{
    CURL* static_handle = session_data->curlHandle;
    assert(statusCode != NULL);

#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s", __FUNCTION__, endpoint);
#endif
    // re-initialize callback data buffer
    char* startPtr = session_data->readCallbackData;
    memset(startPtr, '\0', STATIC_READ_CALLBACK_DATA_BUFFER_SIZE * sizeof(char));
    session_data->readCallbackDataLength = 0;

    char fullurl[1000] = "";

    strcat(fullurl, session_data->basePath);
    strcat(fullurl, endpoint);

    char* full_endpoint = NULL;
    mallocAndStrcpy_s(&full_endpoint, fullurl);
    LogInfo("R700 : POST Endpoint: %s", full_endpoint);

    curlSetOpt(static_handle, NULL, NULL, full_endpoint, "POST");

    if (postData != NULL)
    {
        curl_easy_setopt(static_handle, CURLOPT_POSTFIELDS, postData);
    }

    CURLcode curlCode;

    curlCode = curl_easy_perform(static_handle);

    *statusCode = curlCheckResult(static_handle, curlCode, g_content_json);

    if (full_endpoint)
    {
        free(full_endpoint);
    }

    if (*statusCode == 204)   // No content
    {
        return NULL;
    }
    else
    {
        return session_data->readCallbackData;
    }
}

char*
curlStaticDelete(
    CURL_Static_Session_Data* session_data,
    char* endpoint,
    int* statusCode)
{
    CURL* static_handle = session_data->curlHandle;
    long http_code;

#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s", __FUNCTION__, endpoint);
#endif
    // re-initialize callback data buffer
    char* startPtr = session_data->readCallbackData;
    memset(startPtr, '\0', STATIC_READ_CALLBACK_DATA_BUFFER_SIZE * sizeof(char));
    session_data->readCallbackDataLength = 0;

    char fullurl[1000] = "";

    strcat(fullurl, session_data->basePath);
    strcat(fullurl, endpoint);

    char* full_endpoint = NULL;
    mallocAndStrcpy_s(&full_endpoint, fullurl);

    LogInfo("R700 : DELETE Endpoint: %s", full_endpoint);

    curl_easy_setopt(static_handle, CURLOPT_URL, full_endpoint);
    curl_easy_setopt(static_handle, CURLOPT_POST, 0L);
    curl_easy_setopt(static_handle, CURLOPT_CUSTOMREQUEST, "DELETE");

    CURLcode res;

    res = curl_easy_perform(static_handle);
    curl_easy_getinfo(static_handle, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK)
    {
        LogError("R700 : curl_easy_perform() failed in %s(): %s (CurlCode %d HttpStatus %ld)",
                 __FUNCTION__,
                 curl_easy_strerror(res),
                 res,
                 http_code);
    }

    *statusCode = (int)http_code;

    if (full_endpoint)
    {
        free(full_endpoint);
    }

    if (http_code == 204)   // No content
    {
        return NULL;
    }
    else
    {
        return session_data->readCallbackData;
    }
}

size_t
curlStaticDataWriteCallback(
    void* write_data,
    size_t size,
    size_t nmemb,
    void* userp)
{
    CURL_Static_Session_Data* session_data = (CURL_Static_Session_Data*)userp;

    int offset = strlen(session_data->writeCallbackData) - session_data->writeCallbackDataLength;   // using writeCallbackDataLength as remaining data to write

    int curlWriteBufferSize = size * nmemb;

    // LogInfo("R700 :     PUT cURL buffer size (bytes): %d", curlWriteBufferSize);
    // LogInfo("R700 :        PUT cURL size: %d", (int)size);
    // LogInfo("R700 :        PUT cURL nmemb: %d", (int)nmemb);
    // LogInfo("R700 :     PUT data size (bytes): %d", session_data->writeCallbackDataLength);
    // LogInfo("R700 :     PUT data to write: %s", session_data->writeCallbackData + offset);

    if (session_data->writeCallbackDataLength * sizeof(char) >= curlWriteBufferSize)
    {   // data to write is larger than cURL buffer, write first chunk

        // LogInfo("R700 :        Data offset: %d", offset);

        memcpy(write_data, (session_data->writeCallbackData) + offset, curlWriteBufferSize);
        session_data->writeCallbackDataLength -= curlWriteBufferSize;   // update remaining data to write
        return curlWriteBufferSize;
    }

    else
    {   // data to write will fit in cURL buffer, write everything
        memcpy(write_data, session_data->writeCallbackData + offset, strlen(session_data->writeCallbackData) * sizeof(char) + 1);

        size_t handled_bytes = strlen(session_data->writeCallbackData) * sizeof(char);

        memset(session_data->writeCallbackData, '\0', STATIC_WRITE_CALLBACK_DATA_BUFFER_SIZE * sizeof(char));
        session_data->writeCallbackDataLength = 0;   // no more data to write

        return handled_bytes;   // must return number of bytes handled.  Return 0 to terminate.
    }
}

void
curlStaticCleanup(
    CURL_Static_Session_Data* session_data)
{
    curl_easy_cleanup(session_data->curlHandle);
    free(session_data->readCallbackData);
    free(session_data->writeCallbackData);

    if (session_data->username)
    {
        free(session_data->username);
    }

    if (session_data->password)
    {
        free(session_data->password);
    }

    if (session_data->basePath)
    {
        free(session_data->basePath);
    }

    free(session_data);
}

void
curlStreamCleanup(
    CURL_Stream_Session_Data* session_data)
{
    session_data->threadData.thread_ref = pthread_join(session_data->threadData.tid, NULL);
    curl_multi_remove_handle(session_data->multiHandle, session_data->curlHandle);
    curl_multi_cleanup(session_data->multiHandle);
    curl_easy_cleanup(session_data->curlHandle);

    if (session_data->username)
    {
        free(session_data->username);
    }

    if (session_data->password)
    {
        free(session_data->password);
    }

    if (session_data->basePath)
    {
        free(session_data->basePath);
    }

    if (session_data->dataBuffer)
    {
        free(session_data->dataBuffer);
    }
    free(session_data);
}

void
curlGlobalCleanup()
{
    curl_global_cleanup();
}

// Download file using HTTP GET
// Used to download upgrade firmware file
int
curlGetDownload(
    PUPGRADE_DATA UpgradeData)
{
#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s", __FUNCTION__, UpgradeData->urlData.url);
#endif
    CURL* curl;
    long http_code = 500;
    CURLcode curlCode;
    FILE* fp;

    curl = curl_easy_init();

    if (curl)
    {
        struct curl_slist* headers = NULL;

        // If the target already exists, remove it.
        if (access(UpgradeData->downloadFileName, F_OK) == 0)
        {
            // file exists.  Delete
#ifdef DEBUG_CURL
            LogInfo("R700 : Delete File %s", UpgradeData->downloadFileName);
#endif
            remove(UpgradeData->downloadFileName);
        }

        headers = curlSetOpt(curl, UpgradeData->urlData.username, UpgradeData->urlData.password, UpgradeData->urlData.url, "GET");

        fp = fopen(UpgradeData->downloadFileName, "wb");

        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

        LogInfo("R700 : Start downloading firmware file...");

        curlCode = curl_easy_perform(curl);

        http_code = curlCheckResult(curl, curlCode, g_content_octet_stream);

        if (headers != NULL)
        {
            //curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);

        fclose(fp);
    }

    return http_code;
}

char dummyBuffer[1024];   // To avoid output from curl

size_t
curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    char* outputString = (char*)userdata;
    strcat(outputString, ptr);
    return size * nmemb;
}

void
curlSetWriteCallback(
    CURL* Curl,
    char* Buffer)
{
    curl_easy_setopt(Curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);

    if (Buffer != NULL)
    {
        // use buffer provided by the caller
        curl_easy_setopt(Curl, CURLOPT_WRITEDATA, (void*)Buffer);
    }
    else
    {
        bzero(dummyBuffer, sizeof(dummyBuffer));
        curl_easy_setopt(Curl, CURLOPT_WRITEDATA, (void*)dummyBuffer);
    }
}

// Send GET request
int
curlGet(
    PURL_DATA GetData,
    char* Buffer)
{
#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s%s", __FUNCTION__, GetData->url, GetData->path);
#endif
    CURL* curl;
    long http_code = 500;
    CURLcode curlCode;

    curl = curl_easy_init();

    if (curl)
    {
        struct curl_slist* headers = NULL;

        headers = curlSetOpt(curl, GetData->username, GetData->password, GetData->url, "GET");

        curlSetWriteCallback(curl, Buffer);

        curlCode = curl_easy_perform(curl);

        http_code = curlCheckResult(curl, curlCode, g_content_json);

        if (headers != NULL)
        {
            //curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);
    }

    return http_code;
}

// Send POST request
// Used for rebooting the reader.
int
curlPost(
    PURL_DATA PostData)
{
#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s", __FUNCTION__, PostData->url);
#endif
    CURL* curl;
    long http_code = 500;
    CURLcode curlCode;

    curl = curl_easy_init();

    if (curl)
    {
        struct curl_slist* headers = NULL;

        headers = curlSetOpt(curl, PostData->username, PostData->password, PostData->url, "POST");

        curlSetWriteCallback(curl, NULL);

        curlCode = curl_easy_perform(curl);

        http_code = curlCheckResult(curl, curlCode, g_content_json);

        if (headers != NULL)
        {
            //curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);
    }

    return http_code;
}

// Send PUT request.
// Used to setup LLRP interface to REST
int
curlPut(
    PURL_DATA PutData,
    char* Payload)
{
#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s %s", __FUNCTION__, PutData->url, Payload);
#endif
    CURL* curl;
    long http_code = 500;
    CURLcode curlCode;

    curl = curl_easy_init();

    if (curl)
    {
        struct curl_slist* headers = NULL;

        headers = curlSetOpt(curl, PutData->username, PutData->password, PutData->url, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, Payload);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(Payload));

        curlCode = curl_easy_perform(curl);

        http_code = curlCheckResult(curl, curlCode, g_content_json);

        if (headers != NULL)
        {
            //curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);
    }

    return http_code;
}

static size_t
curlUploadCallback(void* dest, size_t size, size_t nmemb, void* userp)
{
    LogInfo("R700 : Upload Callback size %ld", size);

    UPLOAD_DATA* uploadData = (UPLOAD_DATA*)userp;
    if (uploadData->sizeleft == 0)
        return 0;

    size_t buffer_size = size * nmemb;
    size_t copySize    = uploadData->sizeleft;

    if (copySize > buffer_size)
    {
        copySize = buffer_size;
    }

    memcpy(dest, uploadData->readptr, copySize);

    uploadData->readptr += copySize;
    uploadData->sizeleft -= copySize;
    return copySize;
}

char*
curlPostUploadFile(
    CURL_Static_Session_Data* Session_data,
    char* Endpoint,
    PUPGRADE_DATA UpgradeData,
    int* StatusCode,
    char* Buffer)
{
    CURL* curl;
    char fullurl[1000]  = "";
    char* full_endpoint = NULL;
    FILE* fp;
    size_t len;
    UPLOAD_DATA callback_Data;
    *StatusCode = 400;

#ifdef DEBUG_CURL
    LogInfo("R700 : %s() %s", __FUNCTION__, Endpoint);
#endif

    curl = curl_easy_init();

    if (curl)
    {
        struct curl_slist* headers     = NULL;
        struct curl_httppost* formpost = NULL;
        struct curl_httppost* lastptr  = NULL;

        // make sure the upgrade file is available.
        if (access(UpgradeData->downloadFileName, F_OK) != 0)
        {
            LogError("R700 : Upload File missing");
            goto exit;
        }

        fp = fopen(UpgradeData->downloadFileName, "rb");
        fseek(fp, 0, SEEK_END);
        len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint8_t* buffer = (uint8_t*)calloc(1, len);
        fread(buffer, 1, len, fp);
        fclose(fp);

        strcat(fullurl, Session_data->basePath);
        strcat(fullurl, Endpoint);
        mallocAndStrcpy_s(&full_endpoint, fullurl);

        headers = curlSetOpt(curl, Session_data->username, Session_data->password, full_endpoint, "POST");

        curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME,
                     "cache-control:", CURLFORM_COPYCONTENTS, "no-cache",
                     CURLFORM_END);

        curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME,
                     "content-type:", CURLFORM_COPYCONTENTS, "multipart/form-data",
                     CURLFORM_END);


        curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME,
                     "file",
                     CURLFORM_BUFFER, "data", CURLFORM_BUFFERPTR, buffer,
                     CURLFORM_BUFFERLENGTH, len, CURLFORM_END);

        curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);

        callback_Data.readptr  = buffer;
        callback_Data.sizeleft = len;

        curl_easy_setopt(curl, CURLOPT_READFUNCTION, curlUploadCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, (void*)&callback_Data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)Buffer);

        LogInfo("R700 : POST Upload Endpoint: %s File %s Size %ld", full_endpoint, UpgradeData->downloadFileName, len);

        CURLcode res;

        res = curl_easy_perform(curl);

        *StatusCode = curlCheckResult(curl, res, g_content_json);

        if (headers != NULL)
        {
            //curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);
        curl_formfree(formpost);
    }

exit:

    if (full_endpoint)
    {
        free(full_endpoint);
    }

    return Buffer;
}
