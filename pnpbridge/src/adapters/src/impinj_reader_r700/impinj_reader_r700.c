#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include "impinj_reader_r700.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/crt_abstractions.h"

#include "azure_c_shared_utility/const_defines.h"

#include "curl_wrapper/curl_wrapper.h"
#include <string.h>
#include "helpers/string_manipulation.h"

// Telemetry names for this interface
//
static const char *impinjReader_telemetry_eventData = "eventData";

//
// Read-only property, device state indiciating whether its online or not
//
static const char impinjReader_property_deviceStatus[] = "deviceStatus";
static const unsigned char impinjReader_property_deviceStatus_exampleData[] = "{\"status\":\"idle\",\"time\":\"2021-01-12T22:19:10.293609888Z\",\"serialNumber\":\"37020340091\",\"mqttBrokerConnectionStatus\":\"disconnected\",\"mqttTlsAuthentication\":\"none\",\"kafkaClusterConnectionStatus\":\"connected\"}";
static const int impinjReader_property_deviceStatus_DataLen = sizeof(impinjReader_property_deviceStatus_exampleData) - 1;
static const char g_reportedPropertyStringFormat[] = "\"%s\"";

//
// Callback command names for this interface.
//vb
static const char impinjReader_command_startPreset[] = "startPreset";
static const char impinjReader_command_stopPreset[] = "stopPreset";

//
// Command status codes
//
static const int commandStatusSuccess = 200;
static const int commandStatusPending = 202;
static const int commandStatusNotPresent = 404;
static const int commandStatusFailure = 500;

//
// Response to various commands [Must be valid JSON]
//
// static const unsigned char sampleEnviromentalSensor_BlinkResponse[] = "{ \"status\": 12, \"description\": \"leds blinking\" }";
// static const unsigned char sampleEnviromentalSensor_TurnOnLightResponse[] = "{ \"status\": 1, \"description\": \"light on\" }";
// static const unsigned char sampleEnviromentalSensor_TurnOffLightResponse[] = "{ \"status\": 1, \"description\": \"light off\" }";
static const unsigned char impinjReader_EmptyBody[] = "\" \"";

static const unsigned char impinjReader_OutOfMemory[] = "\"Out of memory\"";
static const unsigned char impinjReader_NotImplemented[] = "\"Requested command not implemented on this interface\"";

//
// Property names that are updatable from the server application/operator
//
static const char impinjReader_property_testPropertyString[] = "testPropertyString";

//
// Response description is an optional, human readable message including more information
// about the setting of the property
//
static const char g_ImpinjReaderPropertyResponseDescription[] = "success";

// Format of the body when responding to a targetTemperature
// static const char g_environmentalSensorBrightnessResponseFormat[] = "%.2d";

// ImpinjReader_SetCommandResponse is a helper that fills out a command response
static int ImpinjReader_SetCommandResponse(
    unsigned char** CommandResponse,
    size_t* CommandResponseSize,
    const unsigned char* ResponseData)
{
    int result = PNP_STATUS_SUCCESS;
    if (ResponseData == NULL)
    {
        LogError("Impinj Reader Adapter:: Response Data is empty");
        *CommandResponseSize = 0;
        return PNP_STATUS_INTERNAL_ERROR;
    }

    *CommandResponseSize = strlen((char*)ResponseData);
    memset(CommandResponse, 0, sizeof(*CommandResponse));

    // Allocate a copy of the response data to return to the invoker. Caller will free this.
    if (mallocAndStrcpy_s((char**)CommandResponse, (char*)ResponseData) != 0)
    {
        LogError("Impinj Reader Adapter:: Unable to allocate response data");
        result = PNP_STATUS_INTERNAL_ERROR;
    }

    return result;
}

static void ImpinjReader_PropertyCallback(
    int ReportedStatus,
    void* UserContextCallback)
{
    LogInfo("PropertyCallback called, result=%d, property name=%s", ReportedStatus, (const char*)UserContextCallback);
}

IOTHUB_CLIENT_RESULT ImpinjReader_ReportDeviceStateAsync(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char * ComponentName)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    char * result = curlStaticGet(device->curl_static_session, "/status");
    return ImpinjReader_ReportPropertyAsync(PnpComponentHandle, ComponentName, "deviceStatus", result);
}

char * ImpinjReader_CreateJsonResponse(
    char * propertyName,
    char * propertyValue)
{
    char jsonFirstChar = '{';
    char jsonToSendStr[2000] = "";

    strcat(jsonToSendStr, "{ \"");
    strcat(jsonToSendStr, propertyName);
    strcat(jsonToSendStr, "\": ");
    if (propertyValue[0] != jsonFirstChar)
    {
        strcat(jsonToSendStr, "\"");
    }
    strcat(jsonToSendStr, propertyValue);
    if (propertyValue[0] != jsonFirstChar)
    {
        strcat(jsonToSendStr, "\"");
    }
    strcat(jsonToSendStr, " }");

    char * response = Str_Trim(jsonToSendStr).strPtr;

    // example JSON string: "{ \"status\": 12, \"description\": \"leds blinking\" }";

    return response;
}

IOTHUB_CLIENT_RESULT ImpinjReader_ReportPropertyAsync(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char * ComponentName,
    char * propertyName,
    char * propertyValue)
{
    IOTHUB_CLIENT_RESULT iothubClientResult = IOTHUB_CLIENT_OK;
    STRING_HANDLE jsonToSend = NULL;

    if ((jsonToSend = PnP_CreateReportedProperty(ComponentName, propertyName, propertyValue)) == NULL)
    {
        LogError("Unable to build reported property response for propertyName=%s, propertyValue=%s", propertyName, propertyValue);
    }
    else
    {
        const char* jsonToSendStr = STRING_c_str(jsonToSend);
        size_t jsonToSendStrLen = strlen(jsonToSendStr);

        if ((iothubClientResult = ImpinjReader_RouteReportedState(NULL, PnpComponentHandle, (const unsigned char*)jsonToSendStr, jsonToSendStrLen,
                                                                  ImpinjReader_PropertyCallback, (void*)propertyName)) != IOTHUB_CLIENT_OK)
        {
            LogError("Impinj Reader:: Unable to send reported state for property=%s, error=%d",
                     propertyName, iothubClientResult);
        }
        else
        {
            LogInfo("Impinj Reader:: Sending device information property to IoTHub. propertyName=%s", //, propertyValue=%s",
                    propertyName);                                                                    //, propertyValue);
        }

        STRING_delete(jsonToSend);
    }

    return iothubClientResult;
}

IOTHUB_CLIENT_RESULT ImpinjReader_RouteReportedState(
    void * ClientHandle,
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const unsigned char * ReportedState,
    size_t Size,
    IOTHUB_CLIENT_REPORTED_STATE_CALLBACK ReportedStateCallback,
    void * UserContextCallback)
{
    IOTHUB_CLIENT_RESULT iothubClientResult = IOTHUB_CLIENT_OK;

    PNP_BRIDGE_CLIENT_HANDLE clientHandle = (ClientHandle != NULL) ?
            (PNP_BRIDGE_CLIENT_HANDLE) ClientHandle : PnpComponentHandleGetClientHandle(PnpComponentHandle);

    if ((iothubClientResult = PnpBridgeClient_SendReportedState(clientHandle, ReportedState, Size,
                                                                ReportedStateCallback, UserContextCallback)) != IOTHUB_CLIENT_OK)
    {
        LogError("IoTHub client call to _SendReportedState failed with error code %d", iothubClientResult);
        goto exit;
    }
    else
    {
        LogInfo("IoTHub client call to _SendReportedState succeeded");
    }

exit:
    return iothubClientResult;
}

int ImpinjReader_TelemetryWorker(
    void* context)
{
    PNPBRIDGE_COMPONENT_HANDLE componentHandle = (PNPBRIDGE_COMPONENT_HANDLE) context;
    PIMPINJ_READER device = PnpComponentHandleGetContext(componentHandle);

    long count = 0;

    char * status = (char *)malloc(sizeof(char*)*1000);
    char * statusNoTimePrev = (char *)malloc(sizeof(char*)*1000);
    char * statusNoTime = (char *)malloc(sizeof(char*)*1000);

    // Report telemetry every 5 seconds till we are asked to stop
    while (true)
    {

        if (device->ShuttingDown)
        {
            return IOTHUB_CLIENT_OK;
        }

        // LogInfo("Telemetry Worker Iteration %d: ", count);
        count++;

        statusNoTimePrev = statusNoTime;

        status = curlStaticGet(device->curl_static_session, "/status");

        JSON_Value * jsonValueStatus = json_parse_string(status);
        JSON_Object * jsonObjectStatus = json_value_get_object(jsonValueStatus);
        json_object_remove(jsonObjectStatus, "time");  // remove "time" field before compare (time always changes)

        statusNoTime = json_serialize_to_string(jsonValueStatus);

        if (strcmp(statusNoTime, statusNoTimePrev) != 0)
        { // send status update only on change in status
            LogInfo("Status Update: %s", status);
            // ImpinjReader_ReportDeviceStateAsync(componentHandle, device->ComponentName);
        }

        int uSecInit = clock();
        int uSecTimer = 0;
        int uSecTarget = 10000;

        while (uSecTimer < uSecTarget)
        {
            if (device->ShuttingDown)
            {
                return IOTHUB_CLIENT_OK;
            }

            ImpinjReader_SendTelemetryMessagesAsync(componentHandle);
            // Sleep for X msec
            ThreadAPI_Sleep(100);
            uSecTimer = clock() - uSecInit;
            // LogInfo("Worker Thread Timer: %d, Target: %d", uSecTimer, uSecTarget);
        }
    }

    free(status);

    return IOTHUB_CLIENT_OK;
}

static void ImpinjReader_TelemetryCallback(
    IOTHUB_CLIENT_CONFIRMATION_RESULT TelemetryStatus,
    void* UserContextCallback)
{
    PIMPINJ_READER device = (PIMPINJ_READER) UserContextCallback;
    if (TelemetryStatus == IOTHUB_CLIENT_CONFIRMATION_OK)
    {
        LogInfo("Impinj Reader:: Successfully delivered telemetry message for <%s>", (const char*)device->SensorState->componentName);
    }
    else
    {
        LogError("Impinj Reader:: Failed delivered telemetry message for <%s>, error=<%d>", (const char*)device->SensorState->componentName, TelemetryStatus);
    }
}

IOTHUB_CLIENT_RESULT
ImpinjReader_RouteSendEventAsync(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    IOTHUB_MESSAGE_HANDLE EventMessageHandle,
    IOTHUB_CLIENT_EVENT_CONFIRMATION_CALLBACK EventConfirmationCallback,
    void * UserContextCallback)
{
    IOTHUB_CLIENT_RESULT iothubClientResult = IOTHUB_CLIENT_OK;
    PNP_BRIDGE_CLIENT_HANDLE clientHandle = PnpComponentHandleGetClientHandle(PnpComponentHandle);
    if ((iothubClientResult = PnpBridgeClient_SendEventAsync(clientHandle, EventMessageHandle,
                                                             EventConfirmationCallback, UserContextCallback)) != IOTHUB_CLIENT_OK)
    {
        LogError("IoTHub client call to _SendEventAsync failed with error code %d", iothubClientResult);
        goto exit;
    }
    else
    {
        LogInfo("IoTHub client call to _SendEventAsync succeeded");
    }

exit:
    return iothubClientResult;
}

IOTHUB_CLIENT_RESULT
ImpinjReader_SendTelemetryMessagesAsync(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle
    )
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    IOTHUB_MESSAGE_HANDLE messageHandle = NULL;
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);

    // read curl stream

    int uSecInit = clock();
    int uSecTimer = 0;
    int uSecTarget = 5000;

    while (uSecTimer < uSecTarget) {  // pull messages out of buffer for target time, then return to calling function

        CURL_Stream_Read_Data read_data = curlStreamReadBufferChunk(device->curl_stream_session);

        if (read_data.dataChunk == NULL) {  // if no data in buffer, stop reading and return to calling function
            // LogInfo("No data returned from stream buffer.");
            IoTHubMessage_Destroy(messageHandle);
            return IOTHUB_CLIENT_OK;
        }

#define MESSAGE_SPLIT_DELIMITER "\n\r"

        char * oneMessage = strtok(read_data.dataChunk, MESSAGE_SPLIT_DELIMITER);  // split data chunk by \n\r in case multiple reader events are contained in the same chunk

        int count = 0;

        while (oneMessage != NULL) {  // send each event individually

            count++;

            LogInfo("TELEMETRY Message %d: %s", count, oneMessage);

            char * currentMessage = ImpinjReader_CreateJsonResponse("streamReadEvent", oneMessage); // TODO: Parameterize this property name

            if ((messageHandle = PnP_CreateTelemetryMessageHandle(device->SensorState->componentName, currentMessage)) == NULL)
            {
                LogError("Impinj Reader Adapter:: PnP_CreateTelemetryMessageHandle failed.");
            }
            else if ((result = ImpinjReader_RouteSendEventAsync(PnpComponentHandle, messageHandle,
                                                                ImpinjReader_TelemetryCallback, device)) != IOTHUB_CLIENT_OK)
            {
                LogError("Impinj Reader Adapter:: SampleEnvironmentalSensor_RouteSendEventAsync failed, error=%d", result);
            }

            oneMessage = strtok(NULL, MESSAGE_SPLIT_DELIMITER);  // continue splitting until all messages are sent individually
        }

        uSecTimer = clock() - uSecInit;
        // LogInfo("Stream Read Timer: %d, Stream Read Target %d", (int)uSecTimer, (int)uSecTarget);
        // LogInfo(" TELEMETRY: %s", read_data.dataChunk);
    }

    IoTHubMessage_Destroy(messageHandle);

    return result;
}

IOTHUB_CLIENT_RESULT ImpinjReader_CreatePnpAdapter(
    const JSON_Object* AdapterGlobalConfig,
    PNPBRIDGE_ADAPTER_HANDLE AdapterHandle)
{
    AZURE_UNREFERENCED_PARAMETER(AdapterGlobalConfig);
    AZURE_UNREFERENCED_PARAMETER(AdapterHandle);

    curl_global_init(CURL_GLOBAL_DEFAULT);  // initialize cURL globally

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
    PIMPINJ_READER device = NULL;

    /* print component creation message */
    char compCreateStr[] = "Creating Impinj Reader component: ";

    char* compHostname;
    char* http_user;
    char* http_pass;

    compHostname = (char*)json_object_dotget_string(AdapterComponentConfig, "hostname");
    http_user = (char*)json_object_dotget_string(AdapterComponentConfig, "username");
    http_pass = (char*)json_object_dotget_string(AdapterComponentConfig, "password");

    strcat(compCreateStr, ComponentName);
    strcat(compCreateStr, "\n       Hostname: ");
    strcat(compCreateStr, compHostname);

    LogInfo("%s", compCreateStr);

    if (strlen(ComponentName) > PNP_MAXIMUM_COMPONENT_LENGTH)
    {
        LogError("ComponentName=%s is too long.  Maximum length is=%d", ComponentName, PNP_MAXIMUM_COMPONENT_LENGTH);
        BridgeComponentHandle = NULL;
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    /* initialize base HTTP strings */

    char str_http[] = "https://";
    char str_basepath[] = "/api/v1";

    char build_str_url_always[100] = "";
    strcat(build_str_url_always, str_http);
    strcat(build_str_url_always, compHostname);
    strcat(build_str_url_always, str_basepath);

    char* http_basepath = Str_Trim(build_str_url_always).strPtr;

    /* initialize cURL sessions */
    CURL_Static_Session_Data *curl_static_session = curlStaticInit(http_user, http_pass, http_basepath, VERIFY_CERTS_OFF, VERBOSE_OUTPUT_OFF);
    CURL_Stream_Session_Data *curl_stream_session = curlStreamInit(http_user, http_pass, http_basepath, VERIFY_CERTS_OFF, VERBOSE_OUTPUT_OFF);

    device = calloc(1, sizeof(IMPINJ_READER));
    if (NULL == device)
    {

        LogError("Unable to allocate memory for Impinj Reader component.");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    device->SensorState = calloc(1, sizeof(IMPINJ_READER_STATE));
    if (NULL == device)
    {
        LogError("Unable to allocate memory for Impinj Reader component state.");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

    mallocAndStrcpy_s(&device->SensorState->componentName, ComponentName);

    device->curl_static_session = curl_static_session;
    device->curl_stream_session = curl_stream_session;
    device->ComponentName = ComponentName;

    PnpComponentHandleSetContext(BridgeComponentHandle, device);
    PnpComponentHandleSetPropertyUpdateCallback(BridgeComponentHandle, ImpinjReader_OnPropertyCallback);
    PnpComponentHandleSetCommandCallback(BridgeComponentHandle, ImpinjReader_OnCommandCallback);

exit:
    return result;
}

char* ImpinjReader_RequestGet(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    PIMPINJ_READER device,
    GET_REQUEST getRequest,
    bool bSendProperty)
{
    char *jsonString;
    JSON_Value* root_value;
    JSON_Object* json_object;
    JSON_Value_Type json_type;
    char hostnameBuffer[63+1];
    char* endpoint = GetRequests[getRequest][0];
    char* propertyName = GetRequests[getRequest][1];

    LogInfo("GET Sending..: %s", endpoint);
    jsonString = curlStaticGet(device->curl_static_session, endpoint);
    LogInfo("GET Received : %s", jsonString);

    if (bSendProperty)
    {
        if ((root_value = json_parse_string(jsonString)) == NULL)
        {
            LogError("json_parse_string failed");
        }

        json_type = json_value_get_type(root_value);

        switch (json_type)
        {
            case JSONArray:
                // This is an array 
                // e.g. ["string1", "string2"]
                ImpinjReader_ReportPropertyAsync(PnpComponentHandle, device->ComponentName, propertyName, jsonString);
                break;

            case JSONObject:
                // This is an object 
                // e.g. {"key" : "value"}
                if ((json_object = json_value_get_object(root_value)) == NULL)
                {
                    LogError("json_value_get_object failed");
                } 
                else if (json_object_get_count(json_object) == 1)
                {
                    char* name = (char *)json_object_get_name(json_object, 0);
                    JSON_Value* value = json_object_get_value(json_object, name);
                    char* valueString = (char *)json_value_get_string(value);

                    if (json_value_get_type(value) == JSONString)
                    {
                        char stringBuffer[63 + 1]; // big enough for HostName
                        snprintf(stringBuffer, sizeof(stringBuffer), g_reportedPropertyStringFormat, valueString);
                        ImpinjReader_ReportPropertyAsync(PnpComponentHandle, device->ComponentName, name, stringBuffer);
                    }

                } else {
                    ImpinjReader_ReportPropertyAsync(PnpComponentHandle, device->ComponentName, propertyName, jsonString);
                }
                break;

            default:
                LogInfo("========= Not sure %d", json_type);
                break;
        }


        json_value_free(root_value);
    }

    return jsonString;
}

IOTHUB_CLIENT_RESULT ImpinjReader_StartPnpComponent(
    PNPBRIDGE_ADAPTER_HANDLE AdapterHandle,
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    AZURE_UNREFERENCED_PARAMETER(AdapterHandle);
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);

    // Store client handle before starting Pnp component
    device->ClientHandle = PnpComponentHandleGetClientHandle(PnpComponentHandle);

    // Set shutdown state
    device->ShuttingDown = false;
    LogInfo("Impinj Reader: Starting Pnp Component");

    PnpComponentHandleSetContext(PnpComponentHandle, device);

    // Get system information
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_STATUS, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_HTTP_STREAM, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_PROFILES, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_PROFILES_INVENTORY_PRESETS, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_HOSTNAME, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_IMAGE, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_IMAGE_UPGRADE, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_NETORK_INTERFACES, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_POWER, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_REGION, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_RFID_LLRP, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_RFID_INTERFACE, true);
    ImpinjReader_RequestGet(PnpComponentHandle, device, GET_SYSTEM_TIME, true);

    curlStreamSpawnReaderThread(device->curl_stream_session);

    // Report Device State Async
    // result = ImpinjReader_ReportPropertyAsync(PnpComponentHandle, device->SensorState->componentName);

    // Create a thread to periodically publish telemetry
    if (ThreadAPI_Create(&device->WorkerHandle, ImpinjReader_TelemetryWorker, PnpComponentHandle) != THREADAPI_OK)
    {
        LogError("ThreadAPI_Create failed");
        return IOTHUB_CLIENT_ERROR;
    }
    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT ImpinjReader_StopPnpComponent(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);

    if (device)
    {
        device->ShuttingDown = true;
        curlStreamStopThread(device->curl_stream_session);
        ThreadAPI_Join(device->WorkerHandle, NULL);
    }
    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT ImpinjReader_DestroyPnpComponent(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    if (device != NULL)
    {
        if (device->SensorState != NULL)
        {
            if (device->SensorState->customerName != NULL)
            {
                if (device->curl_static_session != NULL)
                {
                    curlStreamCleanup(device->curl_stream_session);
                    curlStaticCleanup(device->curl_static_session);
                    free(device->curl_static_session);
                }
                free(device->SensorState->customerName);
            }
            free(device->SensorState);
        }
        free(device);

        PnpComponentHandleSetContext(PnpComponentHandle, NULL);
    }

    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT ImpinjReader_DestroyPnpAdapter(
    PNPBRIDGE_ADAPTER_HANDLE AdapterHandle)
{
    AZURE_UNREFERENCED_PARAMETER(AdapterHandle);

    curl_global_cleanup();  // cleanup cURL globally

    return IOTHUB_CLIENT_OK;
}

void ImpinjReader_ProcessProperty(
    void* ClientHandle, 
    const char* PropertyName, 
    JSON_Value* PropertyValue, 
    int version,
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    STRING_HANDLE reportedJson = NULL;
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    IOTHUB_CLIENT_RESULT iothubClientResult;
    const char *reportedPropertyValueString = json_value_get_string(PropertyValue);
    char reportedPropertyStringBuffer[32];

    snprintf(reportedPropertyStringBuffer, sizeof(reportedPropertyStringBuffer), g_reportedPropertyStringFormat, reportedPropertyValueString);

    LogInfo("Proccesing Property Update: %s", PropertyName);
    LogInfo("   New Property Value: %s", reportedPropertyValueString);

    if ((reportedJson = PnP_CreateReportedPropertyWithStatus(device->ComponentName, PropertyName, reportedPropertyStringBuffer, 200, "Wriable Property Ack Sample", version)) == NULL)
    {
        LogError("Unable to build reported property response");
    }
    else
    {
        const char *reportedJsonStr = STRING_c_str(reportedJson);
        LogInfo("Reported Property %s", reportedJsonStr);
        size_t reportedJsonStrLen = strlen(reportedJsonStr);

        if ((iothubClientResult = PnpBridgeClient_SendReportedState(ClientHandle, (const unsigned char *)reportedJsonStr, reportedJsonStrLen, NULL, NULL)) != IOTHUB_CLIENT_OK)
        {
            LogError("Unable to send reported state, error=%d", iothubClientResult);
        }
        else
        {
            LogInfo("Sending acknowledgement of property to IoTHub for component=%s", device->ComponentName);
        }
    }
}

void ImpinjReader_OnPropertyCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* PropertyName,
    JSON_Value* PropertyValue,
    int version,
    void* ClientHandle
{
    LogInfo("Processing Property: handle %p name %s value %s ver %d", PnpComponentHandle, PropertyName, json_value_get_string(PropertyValue), version);
    if (strcmp(PropertyName, impinjReader_property_testPropertyString) == 0)
    {
        ImpinjReader_ProcessProperty(ClientHandle, PropertyName, PropertyValue, version, PnpComponentHandle);
    }
    else
    {
        // If the property is not implemented by this interface, presently we only record a log message but do not have a mechanism to report back to the service
        LogError("Impinj Reader Adapter:: Property name <%s> is not associated with this interface", PropertyName);
    }
}

int ImpinjReader_ProcessCommand(
    PIMPINJ_READER ImpinjReader,
    const char* CommandName,
    JSON_Value* CommandValue,
    unsigned char** CommandResponse,
    size_t* CommandResponseSize)
{
    if (strcmp(CommandName, "startPreset") == 0)
    {

        char startPresetEndpoint_build[100] = "";
        strcat(startPresetEndpoint_build, "/profiles/inventory/presets/");
        strcat(startPresetEndpoint_build, json_value_get_string(CommandValue));
        strcat(startPresetEndpoint_build, "/start");
        char *startPresetEndpoint = Str_Trim(startPresetEndpoint_build).strPtr;

        char* res = curlStaticPost(ImpinjReader->curl_static_session, startPresetEndpoint, "");

        char * response = ImpinjReader_CreateJsonResponse("cmdResponse", res);

        LogInfo("Sending %s Response: %s", CommandName, response);
        // char * response = "{ \"status\": 12, \"description\": \"leds blinking\" }";

        return ImpinjReader_SetCommandResponse(CommandResponse, CommandResponseSize, response);
    }
    else if (strcmp(CommandName, "stopPreset") == 0)
    {

        char* res = curlStaticPost(ImpinjReader->curl_static_session, "/profiles/stop", "");

        char * response = ImpinjReader_CreateJsonResponse("cmdResponse", res);

        LogInfo("Sending %s Response: %s", CommandName, response);

        // char * response = "{ \"status\": 12, \"description\": \"leds blinking\" }";

        return ImpinjReader_SetCommandResponse(CommandResponse, CommandResponseSize, response);
    }
    else
    {
        // If the command is not implemented by this interface, by convention we return a 404 error to server.
        LogError("Impinj Reader Adapter:: Command name <%s> is not associated with this interface", CommandName);
        return 0; // SampleEnvironmentalSensor_SetCommandResponse(CommandResponse, CommandResponseSize, sampleEnviromentalSensor_NotImplemented);
    }
}

int ImpinjReader_OnCommandCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* CommandName,
    JSON_Value* CommandValue,
    unsigned char** CommandResponse,
    size_t* CommandResponseSize
)
{
    LogInfo("Processing Command: %s", CommandName);
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    return ImpinjReader_ProcessCommand(device, CommandName, CommandValue, CommandResponse, CommandResponseSize);
}

PNP_ADAPTER ImpinjReaderR700 = {
    .identity = "impinj-reader-r700",
    .createAdapter = ImpinjReader_CreatePnpAdapter,
    .createPnpComponent = ImpinjReader_CreatePnpComponent,
    .startPnpComponent = ImpinjReader_StartPnpComponent,
    .stopPnpComponent = ImpinjReader_StopPnpComponent,
    .destroyPnpComponent = ImpinjReader_DestroyPnpComponent,
    .destroyAdapter = ImpinjReader_DestroyPnpAdapter
};