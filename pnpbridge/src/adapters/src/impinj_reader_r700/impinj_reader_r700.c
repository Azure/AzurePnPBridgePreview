#include "impinj_reader_r700.h"
#include "helpers/restapi.h"
#include "helpers/pnp_utils.h"
#include "helpers/pnp_property.h"
#include "helpers/pnp_command.h"
#include "helpers/pnp_telemetry.h"
#include "helpers/led.h"
#include <strings.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

MU_DEFINE_ENUM_STRINGS_WITHOUT_INVALID(R700_REST_VERSION, R700_REST_VERSION_VALUES);

/****************************************************************
Start worker threads
****************************************************************/
bool
ImpinjReader_StartWorkers(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    PIMPINJ_READER reader = PnpComponentHandleGetContext(PnpComponentHandle);
    curlStreamSpawnReaderThread(reader->curl_stream_session);

    if (ThreadAPI_Create(&reader->WorkerHandle, ImpinjReader_TelemetryWorker, PnpComponentHandle) != THREADAPI_OK)
    {
        LogError("R700 : ThreadAPI_Create failed");
        return false;
    }

    return true;
}
/****************************************************************
Send /system/status REST API
****************************************************************/
bool
ImpinjReader_IsHttpsReady(
    PIMPINJ_READER Reader)
{
    PUPGRADE_DATA upgradeData = NULL;
    int httpStatus            = R700_STATUS_INTERNAL_ERROR;
    bool ret                  = false;

    if (Reader->curl_static_session != NULL)
    {
        JSON_Value* jsonVal = ImpinjReader_RequestGet(Reader, &R700_REST_LIST[READER_STATUS_GET], NULL, &httpStatus);

        if (jsonVal)
        {
            json_value_free(jsonVal);
        }
    }
    else
    {
        upgradeData = ImpinjReader_Init_UpgradeData(Reader, R700_REST_LIST[READER_STATUS_GET].EndPoint);

        if (upgradeData)
        {
            httpStatus = curlGet(&upgradeData->urlData, NULL);

            free(upgradeData);
        }
    }

    return IsSuccess(httpStatus);
}

/****************************************************************
Wait for reader to be ready by sending GET /system REST API
****************************************************************/
bool
ImpinjReader_WaitForHttps(
    PIMPINJ_READER Reader,
    int MaxRetryCount)
{
    int retryCount     = 0;
    unsigned int delay = Reader->Flags.IsRebootPending == 1 ? 10000 : 3000;

    while (!ImpinjReader_IsHttpsReady(Reader))
    {
        if (retryCount == MaxRetryCount)
        {
            LogError("R700 : Reader Not ready after retry.  Exiting");
            return false;
        }

        if (Reader->Flags.IsRebootPending = 0 || (retryCount % 10 == 0))
        {
            LogInfo("R700 : Waiting for Reader. Retrying after %d sec", delay / 1000);
        }
        ThreadAPI_Sleep(delay);
        retryCount++;
    }

    LogInfo("R700 : Reader is ready");
    return true;
}

/****************************************************************
Send /system REST API
****************************************************************/
bool
ImpinjReader_IsReaderReady(
    PIMPINJ_READER Reader)
{
    PUPGRADE_DATA upgradeData = NULL;
    int httpStatus            = R700_STATUS_INTERNAL_ERROR;
    bool ret                  = false;

    if (Reader->curl_static_session != NULL)
    {
        JSON_Value* jsonVal = ImpinjReader_RequestGet(Reader, &R700_REST_LIST[SYSTEM], NULL, &httpStatus);

        if (jsonVal)
        {
            json_value_free(jsonVal);
        }
    }
    else
    {
        upgradeData = ImpinjReader_Init_UpgradeData(Reader, R700_REST_LIST[SYSTEM].EndPoint);

        if (upgradeData)
        {
            httpStatus = curlGet(&upgradeData->urlData, NULL);

            free(upgradeData);
        }
    }

    return IsSuccess(httpStatus);
}

/****************************************************************
Wait for reader to be ready by sending GET /system REST API
****************************************************************/
bool
ImpinjReader_WaitForReader(
    PIMPINJ_READER Reader,
    int MaxRetryCount)
{
    int retryCount     = 0;
    unsigned int delay = Reader->Flags.IsRebootPending == 1 ? 10000 : 3000;

    while (!ImpinjReader_IsReaderReady(Reader))
    {
        if (retryCount == MaxRetryCount)
        {
            LogError("R700 : Reader Not ready after retry.  Exiting");
            return false;
        }

        if (Reader->Flags.IsRebootPending = 0 || (retryCount % 10 == 0))
        {
            LogInfo("R700 : Waiting for Reader. Retrying after %d sec", delay / 1000);
        }
        ThreadAPI_Sleep(delay);
        retryCount++;
    }

    LogInfo("R700 : Reader is ready");
    return true;
}

/****************************************************************
Initialize CURL sessions
****************************************************************/
bool
ImpinjReader_Initialize_CurlSessions(
    PIMPINJ_READER Reader)
{
    bool ret = true;
    CURL_Static_Session_Data* curl_polling_session;
    CURL_Static_Session_Data* curl_static_session;
    CURL_Stream_Session_Data* curl_stream_session;

    curl_polling_session = curlStaticInit(Reader->username, Reader->password, Reader->baseUrl, Session_Polling, VERIFY_CERTS_OFF, VERBOSE_OUTPUT_OFF);
    curl_static_session  = curlStaticInit(Reader->username, Reader->password, Reader->baseUrl, Session_Static, VERIFY_CERTS_OFF, VERBOSE_OUTPUT_OFF);
    curl_stream_session  = curlStreamInit(Reader->username, Reader->password, Reader->baseUrl, Session_Stream, VERIFY_CERTS_OFF, VERBOSE_OUTPUT_OFF);

    if (curl_polling_session == NULL)
    {
        ret = false;
        LogError("R700 : Failed to initialize CURL Polling Session");
    }
    else
    {
        Reader->curl_polling_session = curl_polling_session;
    }

    if (curl_static_session == NULL)
    {
        ret = false;
        LogError("R700 : Failed to initialize CURL Static Session");
    }
    else
    {
        Reader->curl_static_session = curl_static_session;
    }

    if (curl_stream_session == NULL)
    {
        ret = false;
        LogError("R700 : Failed to initialize CURL Stream Session");
    }
    else
    {
        Reader->curl_stream_session = curl_stream_session;
    }

    return ret;
}

/****************************************************************
Uninitialize CURL sessions
****************************************************************/
bool
ImpinjReader_Uninitialize_CurlSessions(
    PIMPINJ_READER Reader)
{
    bool ret = false;

    Reader->Flags.IsShuttingDown = 1;

    if (Reader->curl_stream_session != NULL)
    {
        // Stop Stream worker thread before closing stream session
        curlStreamStopThread(Reader->curl_stream_session);
        curlStreamCleanup(Reader->curl_stream_session);
        Reader->curl_stream_session = NULL;
    }

    if (Reader->curl_static_session != NULL)
    {
        curlStaticCleanup(Reader->curl_static_session);
        Reader->curl_static_session = NULL;
    }

    // Stop woker thread before closing polling session.
    if (Reader->WorkerHandle)
    {
        ThreadAPI_Join(Reader->WorkerHandle, NULL);
        Reader->WorkerHandle = NULL;
    }

    if (Reader->curl_polling_session != NULL)
    {
        curlStaticCleanup(Reader->curl_polling_session);
        Reader->curl_polling_session = NULL;
    }

    //curlGlobalCleanup();
    return ret;
}

/****************************************************************
Check if the app is running local or remote (e.g. IoT Edge)
****************************************************************/
bool
ImpinjReader_IsLocal(
    PIMPINJ_READER Reader)
{
    struct ifaddrs* ifAddrStruct = NULL;
    struct ifaddrs* ifa          = NULL;
    void* addressPtr             = NULL;

    Reader->Flags.IsRunningLocal = 0;

    if (strcasecmp(Reader->hostname, "localhost") == 0)
    {
        Reader->Flags.IsRunningLocal = 1;
    }
    else
    {
        // get network interface
        JSON_Value_Type jsonType              = JSONError;
        JSON_Value* jsonVal_NetworkInterfaces = NULL;
        JSON_Array* jsonArray_NetworkInterface;
        int arrayCount;
        int i;
        int httpStatus;
        const char* interface = NULL;
        char* jsonResult;

        jsonResult = curlStaticGet(Reader->curl_static_session, R700_REST_LIST[SYSTEM_NETORK_INTERFACES].EndPoint, &httpStatus);

        if ((jsonVal_NetworkInterfaces = json_parse_string(jsonResult)) == NULL)
        {
            LogInfo("R700 :  Unable to retrieve JSON Value for Network Interface from reader.  Check that HTTPS is enabled.");
        }
        else if ((jsonType = json_value_get_type(jsonVal_NetworkInterfaces)) == JSONError)
        {
            LogInfo("R700 : Unable to retrieve JSON type for network interface");
        }
        else if (jsonType == JSONNull)
        {
            LogInfo("R700 : Network Interface JSON is Null type");
        }
        else if (jsonType != JSONArray)
        {
            LogInfo("R700 : Network Interface JSON is not Array");
        }
        else if ((jsonArray_NetworkInterface = json_value_get_array(jsonVal_NetworkInterfaces)) == NULL)
        {
            LogInfo("R700 :  Unable to retrieve JSON Object for Network Interface. Check that HTTPS is enabled.");
        }
        else if ((arrayCount = json_array_get_count(jsonArray_NetworkInterface)) == 0)
        {
            LogError("R700 : Network Interface JSON Array empty");
        }
        else
        {
            // get network info
            getifaddrs(&ifAddrStruct);

            for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next)
            {
                if (ifa->ifa_addr->sa_family == AF_INET || ifa->ifa_addr->sa_family == AF_INET6)
                {
                    // loop through JSON
                    for (i = 0; i < arrayCount; i++)
                    {
                        JSON_Value* jsonVal_NetworkInterface   = NULL;
                        JSON_Object* jsonObj_NetworkInterface  = NULL;
                        JSON_Array* jsonArray_NetworkAddresses = NULL;
                        int networkAddressCount                = 0;
                        int j;

                        const char* interfaceName;
                        unsigned short sa_family;

                        if ((jsonVal_NetworkInterface = json_array_get_value(jsonArray_NetworkInterface, i)) == NULL)
                        {
                            LogError("R700 : Unable to retrieve JSON Value from Network Interface Array");
                            break;
                        }
                        else if ((jsonObj_NetworkInterface = json_value_get_object(jsonVal_NetworkInterface)) == NULL)
                        {
                            LogError("R700 : Unable to retrieve JSON Object from Network Interface Array");
                            break;
                        }
                        else if ((interfaceName = json_object_get_string(jsonObj_NetworkInterface, "interfaceName")) == NULL)
                        {
                            LogError("R700 : Unable to retrieve 'interfaceName' from Network Interface");
                            break;
                        }
                        else if ((interfaceName = json_object_get_string(jsonObj_NetworkInterface, "interfaceName")) == NULL)
                        {
                            LogError("R700 : Unable to retrieve 'interfaceName' from Network Interface");
                            break;
                        }
                        else if ((jsonArray_NetworkAddresses = json_object_get_array(jsonObj_NetworkInterface, "networkAddress")) == NULL)
                        {
                            LogError("R700 : Unable to retrieve 'networkAddress' array from Network Interface");
                            break;
                        }
                        else if ((networkAddressCount = json_array_get_count(jsonArray_NetworkAddresses)) == 0)
                        {
                            LogError("R700 : Network Address JSON Array empty");
                            break;
                        }
                        else
                        {
                            for (j = 0; j < networkAddressCount; j++)
                            {
                                JSON_Value* jsonVal_NetworkAddress  = NULL;
                                JSON_Object* jsonObj_NetworkAddress = NULL;
                                const char* protocol;
                                const char* address;

                                if ((jsonVal_NetworkAddress = json_array_get_value(jsonArray_NetworkAddresses, j)) == NULL)
                                {
                                    LogError("R700 : Unable to retrieve JSON Value from Network Address Array");
                                    break;
                                }
                                else if ((jsonObj_NetworkAddress = json_value_get_object(jsonVal_NetworkAddress)) == NULL)
                                {
                                    LogError("R700 : Unable to retrieve JSON Object from Network Address Array");
                                    break;
                                }
                                else if ((protocol = json_object_get_string(jsonObj_NetworkAddress, "protocol")) == NULL)
                                {
                                    LogError("R700 : Unable to retrieve 'protocol' from Network Interface");
                                    break;
                                }
                                else if ((address = json_object_get_string(jsonObj_NetworkAddress, "address")) == NULL)
                                {
                                    LogError("R700 : Unable to retrieve 'address' from Network Interface");
                                    break;
                                }
                                else
                                {
                                    // we have protocol (IPv4 vs IPv6), address, and interface name

                                    if ((ifa->ifa_addr->sa_family == AF_INET) && (strcmp(protocol, "ipv4") == 0))
                                    {
                                        addressPtr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                                        char addressBuffer[INET_ADDRSTRLEN];
                                        inet_ntop(AF_INET, addressPtr, addressBuffer, INET_ADDRSTRLEN);

                                        if (strcmp(addressPtr, address) == 0)
                                        {
                                            Reader->Flags.IsRunningLocal = 1;
                                            LogJsonPretty("R700 : Found matching network interface", jsonVal_NetworkAddress);
                                            goto exit;
                                        }
                                    }
                                    else if ((ifa->ifa_addr->sa_family == AF_INET6) && (strcmp(protocol, "ipv6") == 0))
                                    {
                                        addressPtr = &((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr;
                                        char addressBuffer[INET6_ADDRSTRLEN];
                                        inet_ntop(AF_INET6, addressPtr, addressBuffer, INET6_ADDRSTRLEN);

                                        if (strcmp(addressPtr, address) == 0)
                                        {
                                            Reader->Flags.IsRunningLocal = 1;
                                            LogJsonPretty("R700 : Found matching network interface", jsonVal_NetworkAddress);
                                            goto exit;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
exit:
    if (ifAddrStruct != NULL)
        freeifaddrs(ifAddrStruct);

    LogInfo("R700 : Device App is running '%s'", Reader->Flags.IsRunningLocal == 1 ? "Local" : "Remote");
}

/****************************************************************
A callback for Property Update (DEVICE_TWIN_UPDATE_COMPLETE)
****************************************************************/
bool
ImpinjReader_OnPropertyCompleteCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    JSON_Value* Payload,
    void* UserContextCallback)
{
    LogInfo("R700 : %s() enter", __FUNCTION__);
    return OnPropertyCompleteCallback(PnpComponentHandle, Payload, UserContextCallback);
}

/****************************************************************
A callback for Property Update (DEVICE_TWIN_UPDATE_PARTIAL)
****************************************************************/
void
ImpinjReader_OnPropertyPatchCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* PropertyName,
    JSON_Value* PropertyValue,
    int Version,
    void* ClientHandle)
{
    LogInfo("R700 : %s() enter", __FUNCTION__);
    OnPropertyPatchCallback(PnpComponentHandle, PropertyName, PropertyValue, Version, ClientHandle);
    return;
}

/****************************************************************
A callback for Direct Method
****************************************************************/
int
ImpinjReader_OnCommandCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* CommandName,
    JSON_Value* CommandValue,
    unsigned char** CommandResponse,
    size_t* CommandResponseSize)
{
    int ret;
    LogInfo("R700 : %s() enter", __FUNCTION__);

    ret = OnCommandCallback(PnpComponentHandle, CommandName, CommandValue, CommandResponse, CommandResponseSize);

    LogInfo("R700 : %s() exit : ret = %d", __FUNCTION__, ret);
    return ret;
}

/****************************************************************
PnP Bridge
****************************************************************/
IOTHUB_CLIENT_RESULT
ImpinjReader_StartPnpComponent(
    PNPBRIDGE_ADAPTER_HANDLE AdapterHandle,
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    AZURE_UNREFERENCED_PARAMETER(AdapterHandle);
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);

    // Store client handle before starting Pnp component
    device->ClientHandle = PnpComponentHandleGetClientHandle(PnpComponentHandle);

    // Set shutdown state
    device->Flags.IsShuttingDown = 0;
    device->RebootWorkerHandle   = NULL;
    device->UpgradeWorkerHandle  = NULL;

    LogInfo("R700 : Starting Pnp Component");

    PnpComponentHandleSetContext(PnpComponentHandle, device);

    if (!ImpinjReader_StartWorkers(PnpComponentHandle))
    {
        return IOTHUB_CLIENT_ERROR;
    }

    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT
ImpinjReader_StopPnpComponent(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);

    LogInfo("R700 : Stopping Pnp Component");

    if (device)
    {
        ImpinjReader_Uninitialize_CurlSessions(device);
    }

    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT
ImpinjReader_DestroyPnpComponent(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);

    LogInfo("R700 : Destroying Pnp Component");

    if (device != NULL)
    {
        if (device->RebootWorkerHandle != NULL)
        {
            ThreadAPI_Join(device->RebootWorkerHandle, NULL);
        }

        if (device->UpgradeWorkerHandle != NULL)
        {
            ThreadAPI_Join(device->UpgradeWorkerHandle, NULL);
        }

        if (device->SensorState != NULL)
        {
            if (device->SensorState->customerName != NULL)
            {
                free(device->SensorState->customerName);
            }
            free(device->SensorState);
        }

        if (device->username != NULL)
        {
            free((void*)device->username);
        }

        if (device->password != NULL)
        {
            free((void*)device->password);
        }

        if (device->baseUrl != NULL)
        {
            free((void*)device->baseUrl);
        }

        if (device->hostname != NULL)
        {
            free((void*)device->hostname);
        }

        free(device);

        PnpComponentHandleSetContext(PnpComponentHandle, NULL);
    }

    curlGlobalCleanup();
    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT
ImpinjReader_DestroyPnpAdapter(
    PNPBRIDGE_ADAPTER_HANDLE AdapterHandle)
{
    AZURE_UNREFERENCED_PARAMETER(AdapterHandle);

    writeLed(SYSTEM_GREEN, LED_OFF);
    curl_global_cleanup();   // cleanup cURL globally

    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT
ImpinjReader_CreatePnpAdapter(
    const JSON_Object* AdapterGlobalConfig,
    PNPBRIDGE_ADAPTER_HANDLE AdapterHandle)
{
    AZURE_UNREFERENCED_PARAMETER(AdapterGlobalConfig);
    AZURE_UNREFERENCED_PARAMETER(AdapterHandle);

    curl_global_init(CURL_GLOBAL_DEFAULT);   // initialize cURL globally

    // Enable HTTPS on reader RShell
    char * enableRShellCmd = "rshell -c \"config network https enable\"";
    system(enableRShellCmd);
    ThreadAPI_Sleep(1000);

    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT
ImpinjReader_CreatePnpComponent(
    PNPBRIDGE_ADAPTER_HANDLE AdapterHandle,
    const char* ComponentName,
    const JSON_Object* AdapterComponentConfig,
    PNPBRIDGE_COMPONENT_HANDLE BridgeComponentHandle)
{
    AZURE_UNREFERENCED_PARAMETER(AdapterComponentConfig);
    AZURE_UNREFERENCED_PARAMETER(AdapterHandle);
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    PIMPINJ_READER device       = NULL;

    /* print component creation message */
    char compCreateStr[128] = {0};

    char* compHostname = NULL;
    char* http_user    = NULL;
    char* http_pass    = NULL;

    curlGlobalInit();

    compHostname = (char*)json_object_dotget_string(AdapterComponentConfig, "hostname");
    http_user    = (char*)json_object_dotget_string(AdapterComponentConfig, "username");
    http_pass    = (char*)json_object_dotget_string(AdapterComponentConfig, "password");

    strcat(compCreateStr, "R700 : Creating Impinj Reader component: ");
    strcat(compCreateStr, ComponentName);
    strcat(compCreateStr, "\n       Hostname: ");
    strcat(compCreateStr, compHostname);

    LogInfo("%s", compCreateStr);

    if (strlen(ComponentName) > PNP_MAXIMUM_COMPONENT_LENGTH)
    {
        LogError("R700 : ComponentName=%s is too long.  Maximum length is=%d", ComponentName, PNP_MAXIMUM_COMPONENT_LENGTH);
        BridgeComponentHandle = NULL;
        result                = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    device = calloc(1, sizeof(IMPINJ_READER));
    if (NULL == device)
    {

        LogError("R700 : Unable to allocate memory for Impinj Reader component.");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    device->SensorState = calloc(1, sizeof(IMPINJ_READER_STATE));
    if (NULL == device)
    {
        LogError("R700 : Unable to allocate memory for Impinj Reader component state.");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    mallocAndStrcpy_s(&device->SensorState->componentName, ComponentName);

    /* initialize base HTTP strings */

    char str_http[]     = "https://";
    char str_basepath[] = "/api/v1";

    char build_str_url_always[100] = "";
    strcat(build_str_url_always, str_http);
    strcat(build_str_url_always, compHostname);
    strcat(build_str_url_always, str_basepath);

    mallocAndStrcpy_s((char**)&device->username, http_user);
    mallocAndStrcpy_s((char**)&device->password, http_pass);
    mallocAndStrcpy_s((char**)&device->baseUrl, build_str_url_always);
    mallocAndStrcpy_s((char**)&device->hostname, compHostname);

    device->Flags.AsUSHORT = 0;

    // Wait for reader to be available, up to 3 min
    if (!ImpinjReader_WaitForReader(device, 180))
    {
        LogError("R700 : Reader Not ready after retry.  Exiting");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    // Make sure REST API interface is enabled.
    // Do this before setting up Curl endpoints
    CheckRfidInterfaceType(device);

    // Wait for HTTPS interface to be available, up to 3 min
    if (!ImpinjReader_WaitForHttps(device, 180))
    {
        LogError("R700 : HTTPS interface NOT ready after retry.  Exiting");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    /* initialize cURL sessions */
    if (!ImpinjReader_Initialize_CurlSessions(device))
    {
        LogError("R700 : Unable to initialize curl sessions.");
        goto exit;
    }

    ImpinjReader_IsLocal(device);
    GetFirmwareVersion(device);

    PnpComponentHandleSetContext(BridgeComponentHandle, device);
    PnpComponentHandleSetPropertyPatchCallback(BridgeComponentHandle, ImpinjReader_OnPropertyPatchCallback);
    PnpComponentHandleSetPropertyCompleteCallback(BridgeComponentHandle, ImpinjReader_OnPropertyCompleteCallback);
    PnpComponentHandleSetCommandCallback(BridgeComponentHandle, ImpinjReader_OnCommandCallback);

exit:

    return result;
}

PNP_ADAPTER ImpinjReaderR700 = {
    .identity            = "impinj-reader-r700",
    .createAdapter       = ImpinjReader_CreatePnpAdapter,
    .createPnpComponent  = ImpinjReader_CreatePnpComponent,
    .startPnpComponent   = ImpinjReader_StartPnpComponent,
    .stopPnpComponent    = ImpinjReader_StopPnpComponent,
    .destroyPnpComponent = ImpinjReader_DestroyPnpComponent,
    .destroyAdapter      = ImpinjReader_DestroyPnpAdapter};
