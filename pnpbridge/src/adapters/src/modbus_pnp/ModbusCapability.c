#include "azure_c_shared_utility/xlogging.h"

#include "parson.h"

#include "ModbusPnp.h"
#include "ModbusCapability.h"
#include "ModbusConnection/ModbusConnection.h"
#include "azure_c_shared_utility/condition.h"

static bool ModbusPnP_ContinueReadTasks = false;
// This condition variable is used to signal when to stop polling
// and only serves to establish the event notification mechanism using 
// azure_c's shared utility library. The lock passed into the Condition_Wait
// function does not protect against any shared resource, and is instead created
// to comply with the cross-platform library's function signature

static COND_HANDLE StopPolling;

#pragma region Commands

static void ModbusPnp_SetCommandResponse(DIGITALTWIN_CLIENT_COMMAND_RESPONSE* pnpClientCommandResponseContext, const char* responseData, int status)
{
    if (NULL == responseData)
    {
        return;
    }

    size_t responseLen = strlen(responseData);
    memset(pnpClientCommandResponseContext, 0, sizeof(*pnpClientCommandResponseContext));
    pnpClientCommandResponseContext->version = DIGITALTWIN_CLIENT_COMMAND_RESPONSE_VERSION_1;

    // Allocate a copy of the response data to return to the invoker.  The PnP layer that invoked PnPSampleEnvironmentalSensor_Blink
    // takes responsibility for freeing this data.
    if ((pnpClientCommandResponseContext->responseData = malloc(responseLen + 1)) == NULL)
    {
        LogError("ModbusPnp: Unable to allocate response data");
        pnpClientCommandResponseContext->status = 500;
    }
    else
    {
        pnpClientCommandResponseContext->responseData = (unsigned char*)responseData;
        //strcpy_s((char*)pnpClientCommandResponseContext->responseData, responseData);
        pnpClientCommandResponseContext->responseDataLen = responseLen;
        pnpClientCommandResponseContext->status = status;
    }
}

const ModbusCommand* ModbusPnp_LookupCommand(SINGLYLINKEDLIST_HANDLE interfaceDefinitions, const char* commandName, int InterfaceId)
{
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(interfaceDefinitions);
    const  ModbusInterfaceConfig* interfaceDef;

    for (int i = 0; i < InterfaceId - 1; i++)
    {
        interfaceDefHandle = singlylinkedlist_get_next_item(interfaceDefHandle);
    }
    interfaceDef = singlylinkedlist_item_get_value(interfaceDefHandle);

    SINGLYLINKEDLIST_HANDLE commandList= interfaceDef->Commands;
    LIST_ITEM_HANDLE commandItemHandle = singlylinkedlist_get_head_item(commandList);
    const ModbusCommand* command = NULL;
    while (commandItemHandle != NULL) {
        command = singlylinkedlist_item_get_value(commandItemHandle);
        if (strcmp(command->Name, commandName) == 0) {
            return command;
        }
        commandItemHandle = singlylinkedlist_get_next_item(commandItemHandle);
    }
    return NULL;
}

void 
ModbusPnp_CommandHandler(
    const DIGITALTWIN_CLIENT_COMMAND_REQUEST* dtClientCommandContext,
    DIGITALTWIN_CLIENT_COMMAND_RESPONSE* dtClientCommandResponseContext,
    void* userContextCallback
    )
{
    PMODBUS_DEVICE_CONTEXT modbusDevice = (PMODBUS_DEVICE_CONTEXT) userContextCallback;
    const ModbusCommand* command = ModbusPnp_LookupCommand(modbusDevice->InterfaceDefinitions, dtClientCommandContext->commandName, 0);

    if (command == NULL)
    {
        return; // -1;
    }

    uint8_t resultedData[MODBUS_RESPONSE_MAX_LENGTH];
    memset(resultedData, 0x00, MODBUS_RESPONSE_MAX_LENGTH);

    CapabilityContext* capContext = calloc(1, sizeof(CapabilityContext));
    if (!capContext)
    {
        LogError("Could not allocate memory for modbus capability context.");
        return;
    }
    capContext->capability = (ModbusCommand*)command;
    capContext->hDevice = modbusDevice->hDevice;
    capContext->connectionType = modbusDevice->DeviceConfig->ConnectionType;
    capContext->hLock = modbusDevice->hConnectionLock;

    int resultLength = ModbusPnp_WriteToCapability(capContext, Command, (char*)dtClientCommandContext->requestData, resultedData);

    if (0 < resultLength) 
    {
        dtClientCommandResponseContext->responseData = calloc(resultLength + 1, sizeof(char));
        if (!dtClientCommandResponseContext->responseData)
        {
            LogError("Could not allocate memory for modbus command response context.");
            free(capContext);
            return;
        }
        memcpy((void*)dtClientCommandResponseContext->responseData, resultedData, resultLength);
        dtClientCommandResponseContext->responseDataLen = resultLength;
    }

    free(capContext);
    return;
}
#pragma endregion

#pragma region ReadWriteProperty

const ModbusProperty* ModbusPnp_LookupProperty(SINGLYLINKEDLIST_HANDLE interfaceDefinitions, const char* propertyName, int InterfaceId)
{
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(interfaceDefinitions);
    const ModbusInterfaceConfig* interfaceDef;

    for (int i = 0; i < InterfaceId - 1; i++)
    {
        interfaceDefHandle = singlylinkedlist_get_next_item(interfaceDefHandle);
    }
    interfaceDef = singlylinkedlist_item_get_value(interfaceDefHandle);

    SINGLYLINKEDLIST_HANDLE propertyList = interfaceDef->Properties;
    LIST_ITEM_HANDLE propertyItemHandle = singlylinkedlist_get_head_item(propertyList);
    const ModbusProperty* property;
    while (propertyItemHandle != NULL) {
        property = singlylinkedlist_item_get_value(propertyItemHandle);
        if (strcmp(property->Name, propertyName) == 0) {
            return property;
        }
        propertyItemHandle = singlylinkedlist_get_next_item(propertyItemHandle);
    }

    return NULL;
}

void ModbusPnp_PropertyHandler(const DIGITALTWIN_CLIENT_PROPERTY_UPDATE* dtClientPropertyUpdate, void* userContextCallback)
{
    PMODBUS_DEVICE_CONTEXT modbusDevice = (PMODBUS_DEVICE_CONTEXT)userContextCallback;
    const ModbusProperty* property = ModbusPnp_LookupProperty(modbusDevice->InterfaceDefinitions, dtClientPropertyUpdate->propertyName, 0);

    if (property == NULL) {
        return;
    }

    uint8_t resultedData[MODBUS_RESPONSE_MAX_LENGTH];
    memset(resultedData, 0x00, MODBUS_RESPONSE_MAX_LENGTH);

    CapabilityContext* capContext = calloc(1, sizeof(CapabilityContext));
    if (!capContext)
    {
        LogError("Could not allocate memory for capability context in property handler.");
        return;
    }
    capContext->capability = (ModbusProperty*) property;
    capContext->hDevice = modbusDevice->hDevice;
    capContext->connectionType = modbusDevice->DeviceConfig->ConnectionType;
    capContext->hLock= modbusDevice->hConnectionLock;
    
    if ((char*)dtClientPropertyUpdate->propertyDesired)
    {
        ModbusPnp_WriteToCapability(capContext, Property, (char*)dtClientPropertyUpdate->propertyDesired, resultedData);
    }

    free(capContext);
}
#pragma endregion

#pragma region ReadOnlyProperty

void ModbusPnP_ReportPropertyUpdatedCallback(DIGITALTWIN_CLIENT_RESULT pnpReportedStatus, void* userContextCallback)
{
    LogInfo("ModbusPnP_ReportPropertyUpdatedCallback called, result=%d, userContextCallback=%p", pnpReportedStatus, userContextCallback);
}

int ModbusPnP_ReportReadOnlyProperty(DIGITALTWIN_INTERFACE_CLIENT_HANDLE pnpInterface, char* propertyName, char* data)
{
    DIGITALTWIN_CLIENT_RESULT pnpClientResult = DIGITALTWIN_CLIENT_OK;

    if (pnpInterface == NULL) {
        return pnpClientResult;
    }

    if ((pnpClientResult = DigitalTwin_InterfaceClient_ReportPropertyAsync(pnpInterface, propertyName, (unsigned char*)data, strlen(data), NULL, ModbusPnP_ReportPropertyUpdatedCallback, (void*)propertyName)) != DIGITALTWIN_CLIENT_OK)
    {
        LogError("PnP_InterfaceClient_ReportReadOnlyPropertyStatusAsync failed, result=%d\n", pnpClientResult);
        return -1;
    }

    return DIGITALTWIN_CLIENT_OK;
}

int ModbusPnp_PollingSingleProperty(void *param)
{
    CapabilityContext* context = param;
    ModbusProperty* property = context->capability;
    LogInfo("Start polling task for property \"%s\".", property->Name);
    uint8_t resultedData[MODBUS_RESPONSE_MAX_LENGTH];
    memset(resultedData, 0x00, MODBUS_RESPONSE_MAX_LENGTH);

    LOCK_HANDLE lock;
    lock = Lock_Init();
    Lock(lock);

    while (ModbusPnP_ContinueReadTasks)
    {
        int resultLen = ModbusPnp_ReadCapability(context, Property, resultedData);
        if (resultLen > 0) {
            ModbusPnP_ReportReadOnlyProperty(PnpAdapterInterface_GetPnpInterfaceClient(property->InterfaceClient), (char*)property->Name, (char*)resultedData);
        }

        Condition_Wait(StopPolling, lock, property->DefaultFrequency);
    }

    Unlock(lock);
    Lock_Deinit(lock);
    LogInfo("Stopped polling task for property \"%s\".", property->Name);
    free(context);
    ThreadAPI_Exit(THREADAPI_OK);
    return DIGITALTWIN_CLIENT_OK;
}

#pragma endregion

#pragma region SendTelemetry

void ModbusPnp_ReportTelemetryCallback(DIGITALTWIN_CLIENT_RESULT pnpSendEventStatus, void* userContextCallback)
{
    LogInfo("ModbusPnp_ReportTelemetryCallback called, result=%d, userContextCallback=%p", pnpSendEventStatus, userContextCallback);
}

int ModbusPnp_ReportTelemetry(DIGITALTWIN_INTERFACE_CLIENT_HANDLE pnpInterface, char* eventName, char* data)
{
    DIGITALTWIN_CLIENT_RESULT pnpClientResult;

    if (pnpInterface == NULL) {
        return DIGITALTWIN_CLIENT_OK;
    }

    char telemetryMessageData[512] = {0};
    sprintf(telemetryMessageData, "{\"%s\":%s}", eventName, data);

    if ((pnpClientResult = DigitalTwin_InterfaceClient_SendTelemetryAsync(pnpInterface, (unsigned char*)telemetryMessageData, strlen(telemetryMessageData), ModbusPnp_ReportTelemetryCallback, (void*)eventName)) != DIGITALTWIN_CLIENT_OK)
    {
        LogError("PnP_InterfaceClient_SendTelemetryAsync failed, result=%d\n", pnpClientResult);
        return -1; // __FAILURE__;
    }

    return DIGITALTWIN_CLIENT_OK;
}

int ModbusPnp_PollingSingleTelemetry(void *param)
{
    CapabilityContext* context = param;
    ModbusTelemetry* telemetry = context->capability;
    LogInfo("Start polling task for telemetry \"%s\".", telemetry->Name);
    uint8_t resultedData[MODBUS_RESPONSE_MAX_LENGTH];

    LOCK_HANDLE lock;
    lock = Lock_Init();
    Lock(lock);
    while (ModbusPnP_ContinueReadTasks)
    {
        memset(resultedData, 0x00, MODBUS_RESPONSE_MAX_LENGTH);
        int resultLen = ModbusPnp_ReadCapability(context, Telemetry, resultedData);
        if (resultLen > 0) {
            ModbusPnp_ReportTelemetry(PnpAdapterInterface_GetPnpInterfaceClient(telemetry->InterfaceClient), (char*)telemetry->Name, (char*)resultedData);
        }

        Condition_Wait(StopPolling, lock, telemetry->DefaultFrequency);
    }
    Unlock(lock);
    Lock_Deinit(lock);

    LogInfo("Stopped polling task for telemetry \"%s\".", telemetry->Name);
    free(context);
    ThreadAPI_Exit(THREADAPI_OK);
    return DIGITALTWIN_CLIENT_OK;
}

#pragma endregion

void StopPollingTasks()
{
    ModbusPnP_ContinueReadTasks = false;
    if (Condition_Post(StopPolling) != COND_OK)
    {
        LogError("Condition variable could not be signalled.");
    }
}

int ModbusPnp_StartPollingAllTelemetryProperty(void* context)
{
    PMODBUS_DEVICE_CONTEXT deviceContext = (PMODBUS_DEVICE_CONTEXT)context;
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(deviceContext->InterfaceDefinitions);
    const ModbusInterfaceConfig* interfaceConfig = singlylinkedlist_item_get_value(interfaceDefHandle);

    SINGLYLINKEDLIST_HANDLE telemetryList = interfaceConfig->Events;
    int telemetryCount = ModbusPnp_GetListCount(interfaceConfig->Events);
    LIST_ITEM_HANDLE telemetryItemHandle = NULL;

    SINGLYLINKEDLIST_HANDLE propertyList = interfaceConfig->Properties;
    int propertyCount = ModbusPnp_GetListCount(interfaceConfig->Properties);
    LIST_ITEM_HANDLE propertyItemHandle = NULL;


    //Initialize the polling tasks for telemetry and properties
    deviceContext->PollingTasks = NULL;
    if (telemetryCount > 0 || propertyCount > 0)
    {
        deviceContext->PollingTasks = calloc(telemetryCount + propertyCount, sizeof(THREAD_HANDLE));
        if (NULL == deviceContext->PollingTasks) {
            return -1;
        }
    }

    StopPolling = Condition_Init();
    ModbusPnP_ContinueReadTasks = true;

    for (int i = 0; i < telemetryCount; i++)
    {
        if (NULL == telemetryItemHandle)
        {
            telemetryItemHandle = singlylinkedlist_get_head_item(telemetryList);
        }
        else
        {
            telemetryItemHandle = singlylinkedlist_get_next_item(telemetryItemHandle);
        }

        const ModbusTelemetry* telemetry = singlylinkedlist_item_get_value(telemetryItemHandle);
        CapabilityContext* pollingPayload = calloc(1, sizeof(CapabilityContext));
        if (!pollingPayload)
        {
            LogError("Could not allocate memory for telemetry polling capability context.");
            continue;
        }
        pollingPayload->hDevice = deviceContext->hDevice;
        pollingPayload->capability = (void*)telemetry;
        pollingPayload->hLock = deviceContext->hConnectionLock;
        pollingPayload->connectionType = deviceContext->DeviceConfig->ConnectionType;

        if (ThreadAPI_Create(&(deviceContext->PollingTasks[i]), ModbusPnp_PollingSingleTelemetry, (void*)pollingPayload) != THREADAPI_OK)
        {
#ifdef WIN32
            LogError("Failed to create worker thread for telemetry \"%s\", 0x%x", telemetry->Name, GetLastError());
#else
            LogError("Failed to create worker thread for telemetry \"%s\".", telemetry->Name);
#endif
        }
    }

    for (int i = 0; i < propertyCount; i++)
    {
        if (NULL == propertyItemHandle)
        {
            propertyItemHandle = singlylinkedlist_get_head_item(propertyList);
        }
        else
        {
            propertyItemHandle = singlylinkedlist_get_next_item(propertyItemHandle);
        }

        const ModbusProperty* property = singlylinkedlist_item_get_value(propertyItemHandle);
        CapabilityContext* pollingPayload = calloc(1, sizeof(CapabilityContext));
        if (!pollingPayload)
        {
            LogError("Could not allocate memory for property polling capability context.");
            continue;
        }
        pollingPayload->hDevice = deviceContext->hDevice;
        pollingPayload->capability = (void*)property;
        pollingPayload->hLock = deviceContext->hConnectionLock;
        pollingPayload->connectionType = deviceContext->DeviceConfig->ConnectionType;

        if (ThreadAPI_Create(&(deviceContext->PollingTasks[telemetryCount + i]), ModbusPnp_PollingSingleProperty, (void*)pollingPayload) != THREADAPI_OK)
        {
#ifdef WIN32
            LogError("Failed to create worker thread for property \"%s\", 0x%x", property->Name, GetLastError());
#else
            LogError("Failed to create worker thread for property \"%s\".", property->Name);
#endif
        }
    }

    return DIGITALTWIN_CLIENT_OK;
}
