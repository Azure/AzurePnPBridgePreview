#include "pnp_telemetry.h"
#include "pnp_property.h"
#include "led.h"

char*
CreateTelemetryMessage(
    char* MessageData)
{
    JSON_Value* jsonVal_Message = NULL;
    JSON_Object* jsonObj_Message;
    JSON_Value* jsonVal_Message_Content = NULL;
    char* messageData                   = NULL;
    long count                          = 0;
    char ledVal                         = '0';

    if ((jsonVal_Message = json_value_init_object()) == NULL)
    {
        LogError("R700 : Cannot create JSON Value for Telemetry Message");
    }
    else if ((jsonObj_Message = json_value_get_object(jsonVal_Message)) == NULL)
    {
        LogError("R700 : Cannot create JSON Object for Telemetry Message");
    }
    else if ((jsonVal_Message_Content = json_parse_string(MessageData)) == NULL)
    {
        LogError("R700 : Cannot parse Message Data");
    }
    else if (json_object_set_value(jsonObj_Message, g_ReaderEvent, jsonVal_Message_Content) != JSONSuccess)
    {
        LogError("R700 : Cannot set ReaderEvent message JSON value");
    }
    else
    {
        LogJsonPretty("R700 : Telemetry", jsonVal_Message);
        messageData = json_serialize_to_string(jsonVal_Message);
    }

    if (jsonVal_Message_Content)
    {
        json_value_free(jsonVal_Message_Content);
    }

    return messageData;
}

IOTHUB_CLIENT_RESULT
ProcessReaderTelemetry(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    char* message;
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;

    // read curl stream
    int uSecInit   = clock();
    int uSecTimer  = 0;
    int uSecTarget = 5000;

    while (uSecTimer < uSecTarget)
    {
        if ((device->Flags.AsUSHORT & 0xc) != 0)
        {
            break;
        }

        CURL_Stream_Read_Data read_data = curlStreamReadBufferChunk(device->curl_stream_session);

        if (read_data.dataChunk == NULL)
        {
            // if no data in buffer, stop reading and return to calling function
            // LogInfo("No data returned from stream buffer.");
            break;
        }

        char* oneMessage = strtok(read_data.dataChunk, MESSAGE_SPLIT_DELIMITER);

        // split data chunk by \n\r in case multiple reader events are contained in the same chunk
        while (oneMessage != NULL)
        {
            message = CreateTelemetryMessage(oneMessage);

            if ((result = SendTelemetryMessages(PnpComponentHandle, message)) != IOTHUB_CLIENT_OK)
            {
                LogError("R700 : ProcessReaderTelemetry failed, error=%d", result);
            }

            // continue splitting until all messages are sent individually
            oneMessage = strtok(NULL, MESSAGE_SPLIT_DELIMITER);
        }
        uSecTimer = clock() - uSecInit;
    }
}

IOTHUB_CLIENT_RESULT
SendTelemetryMessages(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    char* Message)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_ERROR;
    IOTHUB_MESSAGE_HANDLE messageHandle;
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    PIMPINJ_R700_MESSAGE_CONTEXT callbackContext;
    PNP_BRIDGE_CLIENT_HANDLE clientHandle;

    if ((messageHandle = PnP_CreateTelemetryMessageHandle(device->SensorState->componentName, Message)) == NULL)
    {
        LogError("R700 : PnP_CreateTelemetryMessageHandle failed.");
    }
    else if ((clientHandle = PnpComponentHandleGetClientHandle(PnpComponentHandle)) == NULL)
    {
        LogError("R700 : Failed to retrieve client handle");
    }
    else if ((callbackContext = CreateMessageInstance(messageHandle, device)) == NULL)
    {
        LogError("R700 : Failed to allocate callback context");
    }
    else if (AddMessageProperty(messageHandle, Message) != IOTHUB_MESSAGE_OK)
    {
        LogError("R700 : Failed to add message property");
    }
    else if ((result = PnpBridgeClient_SendEventAsync(clientHandle,
                                                      messageHandle,
                                                      TelemetryCallback,
                                                      callbackContext)) != IOTHUB_CLIENT_OK)
    {
        LogError("IoTHub client call to _SendEventAsync failed with error code %d", result);
    }
    else
    {
        LogJsonPrettyStr("R700 : Sent Telemetry", Message);
    }

    if (result != IOTHUB_CLIENT_OK)
    {
        IoTHubMessage_Destroy(messageHandle);
        free(callbackContext);
    }

    return result;
}

/****************************************************************
Allocates a context for callback
****************************************************************/
static PIMPINJ_R700_MESSAGE_CONTEXT
CreateMessageInstance(
    IOTHUB_MESSAGE_HANDLE MessageHandle,
    PIMPINJ_READER Reader)
{
    PIMPINJ_R700_MESSAGE_CONTEXT messageContext = (PIMPINJ_R700_MESSAGE_CONTEXT)malloc(sizeof(IMPINJ_R700_MESSAGE_CONTEXT));
    if (NULL == messageContext)
    {
        LogError("R700 : Failed allocating 'IMPINJ_R700_MESSAGE_CONTEXT'");
    }
    else
    {
        memset(messageContext, 0, sizeof(IMPINJ_R700_MESSAGE_CONTEXT));

        if ((messageContext->messageHandle = IoTHubMessage_Clone(MessageHandle)) == NULL)
        {
            LogError("R700 : Failed clone nessage handle");
            free(messageContext);
            messageContext = NULL;
        }
        else
        {
            messageContext->reader = Reader;
        }
    }

    return messageContext;
}

/****************************************************************
Adds EventType property to message for message routing
****************************************************************/
IOTHUB_MESSAGE_RESULT
AddMessageProperty(
    IOTHUB_MESSAGE_HANDLE MessageHandle,
    char* Message)
{
    IOTHUB_MESSAGE_RESULT result;
    JSON_Value* jsonVal_Message;
    JSON_Object* jsonObj_Message;
    JSON_Value* jsonVal_ReaderEvent;
    JSON_Object* jsonObj_ReaderEvent;

    R700_READER_EVENT eventType = EventNone;
    char buffer[3]              = {0};

    if ((jsonVal_Message = json_parse_string(Message)) == NULL)
    {
        LogError("R700 : Unable to parse message JSON");
    }
    else if ((jsonObj_Message = json_value_get_object(jsonVal_Message)) == NULL)
    {
        LogError("R700 : Cannot retrieve message JSON object");
    }
    else if (json_object_has_value_of_type(jsonObj_Message, g_ReaderEvent, JSONObject) == 0)
    {
        LogError("R700 : Message does not contain Reader Event Object");
    }
    else if ((jsonVal_ReaderEvent = json_object_get_value(jsonObj_Message, g_ReaderEvent)) == NULL)
    {
        LogError("R700 : Cannot retrieve Reader Event JSON value");
    }
    else if ((jsonObj_ReaderEvent = json_value_get_object(jsonVal_ReaderEvent)) == NULL)
    {
        LogError("R700 : Cannot retrieve Reader Event JSON object");
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_TagInventoryEvent) == 1)
    {
        eventType = TagInventory;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_AntennaConnectedEvent) == 1)
    {
        eventType = AntennaConnected;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_AntennaDisconnectedEvent) == 1)
    {
        eventType = AntennaDisconnected;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_InventoryStatusEvent) == 1)
    {
        eventType = InventoryStatus;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_InventoryTerminatedEvent) == 1)
    {
        eventType = InventoryTerminated;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_DiagnosticEvent) == 1)
    {
        eventType = Diagnostic;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_OverflowEvent) == 1)
    {
        eventType = Overflow;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_TagLocationEntryEvent) == 1)
    {
        eventType = TagLocationEntry;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_TagLocationUpdateEvent) == 1)
    {
        eventType = TagLocationUpdate;
    }
    else if (json_object_has_value(jsonObj_ReaderEvent, g_ReaderEvent_TagLocationExitEvent) == 1)
    {
        eventType = TagLocationExit;
    }

    if (eventType != EventNone)
    {
        sprintf(buffer, "%d", eventType);
        result = IoTHubMessage_SetProperty(MessageHandle, "EventType", buffer);
    }

    if (jsonVal_Message)
    {
        json_value_free(jsonVal_Message);
    }

    return result;
}

/****************************************************************
Callback for IoT Hub message
****************************************************************/
static void
TelemetryCallback(
    IOTHUB_CLIENT_CONFIRMATION_RESULT TelemetryStatus,
    void* UserContextCallback)
{
    PIMPINJ_R700_MESSAGE_CONTEXT messageContext = (PIMPINJ_R700_MESSAGE_CONTEXT)UserContextCallback;
    PIMPINJ_READER device                       = messageContext->reader;
    if (TelemetryStatus == IOTHUB_CLIENT_CONFIRMATION_OK)
    {
        LogInfo("R700 : Successfully delivered telemetry message for <%s>", (const char*)device->SensorState->componentName);
    }
    else
    {
        LogError("R700 : Failed delivered telemetry message for <%s>, error=<%d>", (const char*)device->SensorState->componentName, TelemetryStatus);
    }

    IoTHubMessage_Destroy(messageContext->messageHandle);
    free(messageContext);
}

int
ImpinjReader_TelemetryWorker(
    void* context)
{
    PNPBRIDGE_COMPONENT_HANDLE componentHandle = (PNPBRIDGE_COMPONENT_HANDLE)context;
    PIMPINJ_READER device                      = PnpComponentHandleGetContext(componentHandle);
    int count                                  = 0;
    char ledVal                                = '0';
    JSON_Value* jsonValueStatus                = NULL;
    JSON_Object* jsonObjectStatus              = NULL;
    JSON_Value* jsonValueStatusNotime          = NULL;
    JSON_Object* jsonObjectStatusNotime        = NULL;
    PIMPINJ_R700_REST r700_GetStatusRequest    = &R700_REST_LIST[READER_STATUS_POLL];

    char* statusNoTimePrev = NULL;
    char* statusNoTime     = NULL;

    LogInfo("R700 : %s() enter", __FUNCTION__);

    while (true)
    {
        if (count % 2)
        {
            ledVal = LED_ON;
        }
        else
            ledVal = LED_OFF;

        writeLed(SYSTEM_GREEN, ledVal);
        count++;

        if (statusNoTimePrev)
        {
            json_free_serialized_string(statusNoTimePrev);
        }

        statusNoTimePrev = statusNoTime;
        statusNoTime     = NULL;

        int httpStatus;

        jsonValueStatus = ImpinjReader_RequestGet(device, r700_GetStatusRequest, NULL, &httpStatus);   // Get Reader Status from Reader.

        if (httpStatus < R700_STATUS_BAD_REQUEST)
        {
            jsonObjectStatus       = json_value_get_object(jsonValueStatus);
            jsonValueStatusNotime  = json_value_deep_copy(jsonValueStatus);
            jsonObjectStatusNotime = json_value_get_object(jsonValueStatusNotime);

            json_object_remove(jsonObjectStatusNotime, "time");   // remove "time" field before compare (time always changes)

            statusNoTime = json_serialize_to_string(jsonValueStatusNotime);

            json_value_free(jsonValueStatusNotime);
            jsonValueStatusNotime = NULL;

            if (statusNoTime != NULL && statusNoTimePrev != NULL && strcmp(statusNoTime, statusNoTimePrev) != 0)
            {   // send status update only on change in status
                LogJsonPretty("Status Update: ", jsonValueStatus);
                UpdateReadOnlyReportProperty(componentHandle, device->SensorState->componentName, r700_GetStatusRequest->Name, jsonValueStatus);
            }
        }

        if (jsonValueStatus)
        {
            json_value_free(jsonValueStatus);
            jsonValueStatus = NULL;
        }

        if ((device->Flags.AsUSHORT & 0x1c) != 0)
        {
            writeLed(SYSTEM_GREEN, LED_OFF);
            LogInfo("R700 : Exit Stream Thread Falg = 0x%x", device->Flags.AsUSHORT);
            goto exit;
        }
        int uSecInit   = clock();
        int uSecTimer  = 0;
        int uSecTarget = 10000;

        while (uSecTimer < uSecTarget)
        {
            if (device->Flags.IsShuttingDown == 1)
            {
                writeLed(SYSTEM_GREEN, LED_OFF);
                break;
            }

            ProcessReaderTelemetry(componentHandle);
            ThreadAPI_Sleep(100);
            uSecTimer = clock() - uSecInit;
        }
    }

exit:
    if (statusNoTime)
    {
        json_free_serialized_string(statusNoTime);
    }

    if (jsonValueStatus)
    {
        json_value_free(jsonValueStatus);
    }

    LogInfo("R700 : %s() exit", __FUNCTION__);
    ThreadAPI_Exit(0);
    return IOTHUB_CLIENT_OK;
}