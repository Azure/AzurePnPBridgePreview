#include "pnp_property.h"

//MU_DEFINE_ENUM_STRINGS_WITHOUT_INVALID(R700_REST_REQUEST, R700_REST_REQUEST_VALUES);

/****************************************************************
GetDesiredJson retrieves JSON_Object* in the JSON tree
corresponding to the desired payload.
****************************************************************/
static JSON_Object*
GetDesiredJson(
    DEVICE_TWIN_UPDATE_STATE updateState,
    JSON_Value* rootValue)
{
    JSON_Object* rootObject = NULL;
    JSON_Object* desiredObject;

    if ((rootObject = json_value_get_object(rootValue)) == NULL)
    {
        LogError("Unable to get root object of JSON");
        desiredObject = NULL;
    }
    else
    {
        if (updateState == DEVICE_TWIN_UPDATE_COMPLETE)
        {
            // For a complete update, the JSON from IoTHub will contain both "desired" and "reported" - the full twin.
            // We only care about "desired" in this sample, so just retrieve it.
            desiredObject = json_object_get_object(rootObject, g_IoTHubTwinDesiredObjectName);
        }
        else
        {
            // For a patch update, IoTHub does not explicitly put a "desired:" JSON envelope.  The "desired-ness" is implicit
            // in this case, so here we simply need the root of the JSON itself.
            desiredObject = rootObject;
        }
    }

    return desiredObject;
}

/****************************************************************
A callback for Property Update (DEVICE_TWIN_UPDATE_COMPLETE)
****************************************************************/
bool
OnPropertyCompleteCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    JSON_Value* JsonVal_Payload,
    void* UserContextCallback)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    bool bRet             = false;
    JSON_Object* jsonObj_Desired;
    JSON_Value* jsonVal_Version;
    int version;

    LogJsonPretty("R700 : %s() enter", JsonVal_Payload, __FUNCTION__);

    if ((jsonObj_Desired = GetDesiredJson(DEVICE_TWIN_UPDATE_COMPLETE, JsonVal_Payload)) == NULL)
    {
        LogError("R700 : Unable to  retrieve desired JSON object");
    }
    else if ((jsonVal_Version = json_object_get_value(jsonObj_Desired, g_IoTHubTwinDesiredVersion)) == NULL)
    {
        LogError("R700 : Unable to retrieve %s for desired property", g_IoTHubTwinDesiredVersion);
    }
    else if (json_value_get_type(jsonVal_Version) != JSONNumber)
    {
        // The $version must be a number (and in practice an int) A non-numerical value indicates
        // something is fundamentally wrong with how we've received the twin and we should not proceed.
        LogError("R700 : %s is not JSONNumber", g_IoTHubTwinDesiredVersion);
    }
    else if ((version = (int)json_value_get_number(jsonVal_Version)) == 0)
    {
        LogError("R700 : Unable to retrieve %s", g_IoTHubTwinDesiredVersion);
    }
    else
    {
        JSON_Object* jsonObj_Desired_Component = NULL;
        JSON_Object* jsonObj_Desired_Property  = NULL;
        int i;
        JSON_Value* jsonVal_Rest            = NULL;
        JSON_Value* jsonValue_Desired_Value = NULL;
        int status                          = PNP_STATUS_SUCCESS;

        // Device Twin may not have properties for this component right after provisioning
        if (device->SensorState->componentName != NULL && (jsonObj_Desired_Component = json_object_get_object(jsonObj_Desired, device->SensorState->componentName)) == NULL)
        {
            LogInfo("R700 : Unable to retrieve desired JSON Object for Component %s", device->SensorState->componentName);
        }

        for (i = 0; i < (sizeof(R700_REST_LIST) / sizeof(IMPINJ_R700_REST)); i++)
        {
            PIMPINJ_R700_REST r700_Request = &R700_REST_LIST[i];
            jsonVal_Rest                   = NULL;
            jsonValue_Desired_Value        = NULL;

            if (device->ApiVersion < r700_Request->ApiVersion)
            {
                LogError("R700 : Unsupported API. Please upgrade firmware");
                status = R700_STATUS_NOT_ALLOWED;
                break;
            }
            else if (device->Flags.IsRESTEnabled == 0 && R700_REST_LIST[i].IsRestRequired == true)
            {
                LogError("R700 : RESTFul API not enabled : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
                status = R700_STATUS_CONFLICT;
                break;
            }
            else if (device->Flags.IsRebootPending == 1)
            {
                LogInfo("R700 : Reboot Pending : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
                status = R700_STATUS_CONFLICT;
                break;
            }
            else if (device->Flags.IsShuttingDown == 1)
            {
                LogInfo("R700 : Shutting Down : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
                status = R700_STATUS_CONFLICT;
                break;
            }


            switch (r700_Request->DtdlType)
            {
                case COMMAND:
                    break;

                case READONLY:
                    jsonVal_Rest = ImpinjReader_RequestGet(device, r700_Request, NULL, &status);
                    UpdateReadOnlyReportProperty(PnpComponentHandle, device->SensorState->componentName, r700_Request->Name, jsonVal_Rest);
                    break;

                case WRITABLE:
                    if ((jsonObj_Desired_Component == NULL) ||
                        ((jsonObj_Desired_Property = json_object_get_object(jsonObj_Desired_Component, r700_Request->Name)) == NULL))
                    {
                        // No desired property for this property.  Send Reported Property with version = 1 with current values from reader
                        jsonVal_Rest = ImpinjReader_RequestGet(device, r700_Request, NULL, &status);
                        UpdateWritableProperty(PnpComponentHandle, r700_Request, jsonVal_Rest, status, "Value from Reader", 1);
                    }
                    else
                    {
                        // Desired Property found for this property.
                        jsonValue_Desired_Value = json_object_get_value(jsonObj_Desired_Component, r700_Request->Name);
                        OnPropertyPatchCallback(PnpComponentHandle, r700_Request->Name, jsonValue_Desired_Value, version, NULL);
                    }

                    break;
            }

            if (jsonVal_Rest)
            {
                json_value_free(jsonVal_Rest);
            }
        }
        bRet = true;
    }

    return bRet;
}

/****************************************************************
A callback for Property Update (DEVICE_TWIN_UPDATE_PARTIAL)
****************************************************************/
void
OnPropertyPatchCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* PropertyName,
    JSON_Value* JsonVal_Property,
    int Version,
    void* ClientHandle)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    int i;
    PIMPINJ_R700_REST r700_Request = NULL;
    JSON_Value* jsonVal_Rest       = NULL;
    char* payload                  = NULL;
    int httpStatus                 = PNP_STATUS_SUCCESS;

    LogJsonPretty("R700 : %s() enter", JsonVal_Property, __FUNCTION__);

    for (int i = 0; i < R700_REST_MAX; i++)
    {
        r700_Request = &R700_REST_LIST[i];

        if (r700_Request->DtdlType != WRITABLE)
        {
            continue;
        }
        else if (strcmp(PropertyName, r700_Request->Name) == 0)
        {
            LogInfo("R700 : Found property %s i = %d req=%s", r700_Request->Name, i, MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
            break;
        }
    }

    if (r700_Request != NULL)
    {
        if (device->ApiVersion < r700_Request->ApiVersion)
        {
            LogError("R700 : Unsupported API. Please upgrade firmware");
        }
        else if (device->Flags.IsRebootPending == 1)
        {
            LogInfo("R700 : Reboot Pending : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
        }
        else if (device->Flags.IsRESTEnabled == 0 && R700_REST_LIST[i].IsRestRequired == true)
        {
            LogError("R700 : RESTFul API not enabled : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
        }
        else if (device->Flags.IsShuttingDown == 1)
        {
            LogInfo("R700 : Shutting Down : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
        }
        else if ((payload = json_serialize_to_string(JsonVal_Property)) == NULL)
        {
            LogError("R700 : Unabled to serialize JSON for Property");
        }
        else
        {
            if (r700_Request->RestType == PUT)
            {
                jsonVal_Rest = ImpinjReader_RequestPut(device, r700_Request, NULL, payload, &httpStatus);
            }
            else if (r700_Request->RestType == POST)
            {
                jsonVal_Rest = ImpinjReader_RequestPost(device, r700_Request, payload, &httpStatus);
            }
            else if (r700_Request->RestType == DELETE)
            {
                jsonVal_Rest = ImpinjReader_RequestDelete(device, r700_Request, payload, &httpStatus);
            }
            else
            {
                LogError("R700 : Unknown Request %d", r700_Request->Request);
                assert(false);
            }

            UpdateWritableProperty(PnpComponentHandle,
                                   r700_Request,
                                   jsonVal_Rest,
                                   httpStatus,
                                   NULL,
                                   Version);
        }

        if (payload)
        {
            json_free_serialized_string(payload);
        }

        if (jsonVal_Rest)
        {
            json_value_free(jsonVal_Rest);
        }
    }

    return;
}

/****************************************************************
A callback from IoT Hub on Reported Property Update
****************************************************************/
static void
ReportedPropertyCallback(
    int ReportedStatus,
    void* UserContextCallback)
{
    LogInfo("R700 : PropertyCallback called, result=%d, property name=%s",
            ReportedStatus,
            (const char*)UserContextCallback);
}

/****************************************************************
Processes Read Only Property Update
****************************************************************/
IOTHUB_CLIENT_RESULT
UpdateReadOnlyReportPropertyEx(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* ComponentName,
    char* PropertyName,
    JSON_Value* JsonVal_Property,
    bool Verbose)
{
    IOTHUB_CLIENT_RESULT iothubClientResult = IOTHUB_CLIENT_OK;
    STRING_HANDLE jsonToSend                = NULL;
    char* propertyValue                     = NULL;

    if (Verbose)
    {
        LogJsonPretty("R700 : %s() enter", JsonVal_Property, __FUNCTION__);
    }

    if ((propertyValue = json_serialize_to_string(JsonVal_Property)) == NULL)
    {
        LogError("R700 : Unabled to serialize Read Only Property JSON payload");
    }
    else if ((jsonToSend = PnP_CreateReportedProperty(ComponentName, PropertyName, propertyValue)) == NULL)
    {
        LogError("R700 : Unable to build reported property response for Read Only Property %s, Value=%s",
                 PropertyName,
                 propertyValue);
    }
    else
    {
        PNP_BRIDGE_CLIENT_HANDLE clientHandle = PnpComponentHandleGetClientHandle(PnpComponentHandle);

        if ((iothubClientResult = PnpBridgeClient_SendReportedState(clientHandle,
                                                                    STRING_c_str(jsonToSend),
                                                                    STRING_length(jsonToSend),
                                                                    ReportedPropertyCallback,
                                                                    (void*)PropertyName)) != IOTHUB_CLIENT_OK)
        {
            LogError("R700 : Unable to send reported state for property=%s, error=%d",
                     PropertyName,
                     iothubClientResult);
        }
        else
        {
            LogInfo("R700 : Sent Read Only Property %s to IoT Hub", PropertyName);
        }

        STRING_delete(jsonToSend);
    }

    if (propertyValue)
    {
        json_free_serialized_string(propertyValue);
    }

    return iothubClientResult;
}

IOTHUB_CLIENT_RESULT
UpdateReadOnlyReportProperty(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* ComponentName,
    char* PropertyName,
    JSON_Value* JsonVal_Property)
{
    return UpdateReadOnlyReportPropertyEx(PnpComponentHandle, ComponentName, PropertyName, JsonVal_Property, true);
}
/****************************************************************
Processes Writable Property
****************************************************************/
IOTHUB_CLIENT_RESULT
UpdateWritableProperty(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    PIMPINJ_R700_REST RestRequest,
    JSON_Value* JsonVal_Property,
    int Ac,
    char* Ad,
    int Av)
{
    PIMPINJ_READER device                   = PnpComponentHandleGetContext(PnpComponentHandle);
    IOTHUB_CLIENT_RESULT iothubClientResult = IOTHUB_CLIENT_OK;
    STRING_HANDLE jsonToSend                = NULL;
    char* propertyName                      = RestRequest->Name;
    int httpStatus;
    JSON_Object* jsonObj_Property = NULL;
    JSON_Value* jsonVal_Message   = NULL;
    JSON_Value* jsonVal_Rest      = NULL;
    const char* descToSend        = Ad;
    char* propertyValue           = NULL;

    LogJsonPretty("R700 : %s() enter AC=%d AV=%d AD=%s", JsonVal_Property, __FUNCTION__, Ac, Av, Ad);

    // Check status (Ack Code)
    switch (Ac)
    {
        case R700_STATUS_NO_CONTENT:
            // Reader did not return any content
            // Read the current/new settings from HW
            assert(JsonVal_Property == NULL);
            jsonVal_Rest = ImpinjReader_RequestGet(device, RestRequest, NULL, &httpStatus);
            descToSend   = ImpinjReader_ProcessResponse(RestRequest, JsonVal_Property, Ac);
            break;

        case R700_STATUS_ACCEPTED:
            // Reader responded with some context
            assert(JsonVal_Property != NULL);
            jsonVal_Rest = ImpinjReader_RequestGet(device, RestRequest, NULL, &httpStatus);
            descToSend   = ImpinjReader_ProcessResponse(RestRequest, JsonVal_Property, Ac);
            break;

        case R700_STATUS_BAD_REQUEST:
        case R700_STATUS_FORBIDDEN:
        case R700_STATUS_NOT_FOUND:
        case R700_STATUS_CONFLICT:
        case R700_STATUS_INTERNAL_ERROR:
            // Errors
            assert(JsonVal_Property != NULL);
            jsonVal_Rest = ImpinjReader_RequestGet(device, RestRequest, NULL, &httpStatus);
            descToSend   = ImpinjReader_ProcessErrorResponse(JsonVal_Property, RestRequest->DtdlType);
            break;
    }

    if (jsonVal_Rest)
    {
        propertyValue = json_serialize_to_string(jsonVal_Rest);
    }
    else
    {
        propertyValue = json_serialize_to_string(JsonVal_Property);
    }

    if ((jsonToSend = PnP_CreateReportedPropertyWithStatus(device->SensorState->componentName,
                                                           propertyName,
                                                           propertyValue,
                                                           Ac,
                                                           descToSend,
                                                           Av)) == NULL)
    {
        LogError("R700 : Unable to build reported property response for Writable Property %s, Value=%s",
                 propertyName,
                 propertyValue);
    }
    else
    {
        PNP_BRIDGE_CLIENT_HANDLE clientHandle = PnpComponentHandleGetClientHandle(PnpComponentHandle);

        LogJsonPrettyStr("R700 : Sending Reported Property Json", (char*)STRING_c_str(jsonToSend));

        if ((iothubClientResult = PnpBridgeClient_SendReportedState(clientHandle,
                                                                    STRING_c_str(jsonToSend),
                                                                    STRING_length(jsonToSend),
                                                                    ReportedPropertyCallback,
                                                                    (void*)propertyName)) != IOTHUB_CLIENT_OK)
        {
            LogError("R700 : Unable to send reported state for Writable Property %s :: error=%d",
                     propertyName,
                     iothubClientResult);
        }
        else
        {
            LogInfo("R700 : Sent reported state for Writable Property %s", propertyName);
        }
    }

    // clean up
    if (jsonToSend)
    {
        STRING_delete(jsonToSend);
    }

    if (jsonVal_Rest)
    {
        json_value_free(jsonVal_Rest);
    }

    if (propertyValue)
    {
        json_free_serialized_string(propertyValue);
    }

    return iothubClientResult;
}