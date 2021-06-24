#include "pnp_command.h"
#include "restapi.h"
#include <unistd.h>

/****************************************************************
Creates command response payload
****************************************************************/
int
ImpinjReader_SetCommandResponse(
    unsigned char** CommandResponse,
    size_t* CommandResponseSize,
    const unsigned char* ResponseData)
{
    int result = PNP_STATUS_SUCCESS;

    if (ResponseData == NULL)
    {
        LogError("R700 : Response Data is empty");
        *CommandResponseSize = 0;
        return PNP_STATUS_INTERNAL_ERROR;
    }

    *CommandResponseSize = strlen((char*)ResponseData);
    memset(CommandResponse, 0, sizeof(*CommandResponse));

    // Allocate a copy of the response data to return to the invoker. Caller will free this.
    if (mallocAndStrcpy_s((char**)CommandResponse, (char*)ResponseData) != 0)
    {
        LogError("R700 : Unable to allocate response data");
        result = PNP_STATUS_INTERNAL_ERROR;
    }

    return result;
}

/****************************************************************
A callback for Direct Method
****************************************************************/
int
OnCommandCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* CommandName,
    JSON_Value* CommandValue,
    unsigned char** CommandResponse,
    size_t* CommandResponseSize)
{
    PIMPINJ_READER device = PnpComponentHandleGetContext(PnpComponentHandle);
    int i;
    int httpStatus                   = PNP_STATUS_INTERNAL_ERROR;
    char* restBody                   = NULL;
    JSON_Value* jsonVal_RestResponse = NULL;
    char* restResponse               = NULL;
    char* restParameter              = NULL;
    const char* responseString       = NULL;
    PIMPINJ_R700_REST r700_Request   = NULL;
    PUPGRADE_DATA upgradeData        = NULL;
    R700_UPGRADE_STATUS upgradeStatus = UNKNOWN;

    LogJsonPretty("R700 : %s() enter.  Command Name='%s'", CommandValue, __FUNCTION__, CommandName);

    for (i = 0; i < R700_REST_MAX; i++)
    {
        if (R700_REST_LIST[i].DtdlType != COMMAND)
        {
            continue;
        }
        else if (strcmp(CommandName, R700_REST_LIST[i].Name) == 0)   // find command name from list
        {
            r700_Request = &R700_REST_LIST[i];

            LogInfo("R700 : Found Command %s", r700_Request->Name);

            if (device->ApiVersion < r700_Request->ApiVersion)
            {
                LogError("R700 : Unsupported API. Please upgrade firmware");
                httpStatus = R700_STATUS_NOT_ALLOWED;
                break;
            }
            else if (device->Flags.IsRESTEnabled == 0 && R700_REST_LIST[i].IsRestRequired == true)
            {
                LogError("R700 : RESTFul API not enabled : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
                jsonVal_RestResponse = json_parse_string(g_restApiNotEnabled);
                httpStatus           = R700_STATUS_CONFLICT;
                break;
            }
            else if (device->Flags.IsRebootPending == 1)
            {
                LogInfo("R700 : Reboot Pending : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
                jsonVal_RestResponse = json_parse_string(g_rebootPendingResponse);
                httpStatus           = R700_STATUS_CONFLICT;
                break;
            }
            else if (device->Flags.IsShuttingDown == 1)
            {
                LogInfo("R700 : Shutting Down : %s", MU_ENUM_TO_STRING(R700_REST_REQUEST, r700_Request->Request));
                jsonVal_RestResponse = json_parse_string(g_rebootPendingResponse);
                httpStatus           = R700_STATUS_CONFLICT;
                break;
            }

            switch (r700_Request->Request)
            {
                case PROFILES_INVENTORY_PRESETS_ID_SET:
                    // Receive Preset ID
                    restParameter = (char*)GetStringFromPayload(CommandValue, g_presetId);
                    restBody      = PreProcessSetPresetIdPayload(CommandValue);
                    break;

                case PROFILES_INVENTORY_PRESETS_ID_SET_PASSTHROUGH:
                    // Receive Preset ID
                    restParameter = (char*)GetStringFromPayload(CommandValue, g_presetId);
                    restBody      = (char*)GetStringFromPayload(CommandValue, g_presetObjectJSON);
                    break;

                case PROFILES_INVENTORY_PRESETS_ID_GET:
                case PROFILES_INVENTORY_PRESETS_ID_DELETE:
                case PROFILES_START: {
                    // Receive Preset ID
                    restParameter = (char*)GetStringFromPayload(CommandValue, g_presetId);
                    break;
                }

                case PROFILES_INVENTORY_TAG:
                    restParameter = (char*)PreProcessTagPresenceResponse(CommandValue);
                    break;

                case SYSTEM_POWER_SET:
                case SYSTEM_TIME_SET:
                    // takes JSON payload for REST API Body
                    restBody = json_serialize_to_string(CommandValue);
                    break;

                case SYSTEM_REBOOT:
                    return ProcessReboot(PnpComponentHandle, CommandResponse, CommandResponseSize);   // never returns
                    break;

                case SYSTEM_IMAGE_UPGRADE_UPLOAD: {
                    upgradeStatus = CheckUpgardeStatus(device, NULL);

                    if (upgradeStatus != READY)
                    {
                        LogError("R700 : Reader is not ready for upgrade. Upgrade Status %d", upgradeStatus);
                        jsonVal_RestResponse = json_parse_string(g_upgradeNotReady);
                        httpStatus           = R700_STATUS_CONFLICT;
                    }
                    else
                    {
                        upgradeData = ProcessDownloadFirmware(device, CommandValue);
                    }
                    break;
                }
            }

            // Call REST API
            switch (r700_Request->RestType)
            {
                case GET:
                    jsonVal_RestResponse = ImpinjReader_RequestGet(device, r700_Request, restParameter, &httpStatus);
                    break;
                case PUT:
                    jsonVal_RestResponse = ImpinjReader_RequestPut(device, r700_Request, restParameter, restBody, &httpStatus);
                    break;
                case POST:
                    jsonVal_RestResponse = ImpinjReader_RequestPost(device, r700_Request, restParameter, &httpStatus);
                    break;
                case POST_UPLOAD:
                    if (upgradeStatus != READY)
                    {
                        break;
                    }
                    else if (upgradeData == NULL)
                    {
                        LogError("R700 : Failed to allocate download data");
                        httpStatus = R700_STATUS_INTERNAL_ERROR;
                        mallocAndStrcpy_s(&restResponse, g_upgradeFailAlloc);
                        break;
                    }
                    else if (upgradeData->statusCode != R700_STATUS_OK)
                    {
                        LogError("R700 : Failed to download firmware upgrade file.");
                        mallocAndStrcpy_s(&restResponse, g_upgradeFailDownload);
                        httpStatus = upgradeData->statusCode;
                    }
                    else
                    {
                        jsonVal_RestResponse = ImpinjReader_RequestUpgrade(PnpComponentHandle, r700_Request, upgradeData, &httpStatus);
                    }

                    if (access(upgradeData->downloadFileName, F_OK) == 0)
                    {
                        // file exists.  Delete
                        // LogInfo("R700 : Delete Firmware Upgrade File %s", download_Data->downloadFileName);
                        remove(upgradeData->downloadFileName);
                    }

                    break;

                case DELETE:
                    jsonVal_RestResponse = ImpinjReader_RequestDelete(device, r700_Request, restParameter, &httpStatus);
                    break;
            }
            break;
        }
    }

    if (r700_Request != NULL)
    {
        if (jsonVal_RestResponse)
        {
            LogJsonPretty("R700 : Command Response", jsonVal_RestResponse);
        }

        switch (httpStatus)
        {
            case R700_STATUS_OK:
            case R700_STATUS_CREATED:
                restResponse = json_serialize_to_string(jsonVal_RestResponse);
                break;

            case R700_STATUS_ACCEPTED:
                restResponse = (char*)ImpinjReader_ProcessResponse(r700_Request, jsonVal_RestResponse, httpStatus);
                break;

            case R700_STATUS_BAD_REQUEST:
            case R700_STATUS_FORBIDDEN:
            case R700_STATUS_NOT_FOUND:
            case R700_STATUS_CONFLICT:
            case R700_STATUS_INTERNAL_ERROR:
                restResponse = ImpinjReader_ProcessErrorResponse(jsonVal_RestResponse, r700_Request->DtdlType);
                break;
            case R700_STATUS_NOT_ALLOWED:
                mallocAndStrcpy_s(&restResponse, g_unsupportedApiResponse);
                break;
            case R700_STATUS_NOT_IMPLEMENTED:
                mallocAndStrcpy_s(&restResponse, g_notImplementedApiResponse);
                break;
        }
    }

    if (restResponse == NULL)
    {
        if (r700_Request->Request == SYSTEM_POWER_SET)
        {
            if (httpStatus == R700_STATUS_ACCEPTED)
            {
                responseString = g_powerSourceRequireReboot;
            }
            else if (httpStatus == R700_STATUS_NO_CONTENT)
            {
                responseString = g_powerSourceAlreadyConfigure;
            }
        }
        else if (httpStatus == R700_STATUS_NO_CONTENT)
        {
            if (jsonVal_RestResponse)
            {
                restResponse = json_serialize_to_string(jsonVal_RestResponse);
            }
            else
            {
                responseString = g_noContentResponse;
            }
        }
        else
        {
            responseString = g_emptyCommandResponse;
        }
    }
    else
    {
        responseString = restResponse;
    }

    if (ImpinjReader_SetCommandResponse(CommandResponse, CommandResponseSize, responseString) != PNP_STATUS_SUCCESS)
    {
        httpStatus = PNP_STATUS_INTERNAL_ERROR;
    }

    // clean up

    if (upgradeData)
    {
        free(upgradeData);
    }

    if (restBody)
    {
        json_free_serialized_string(restBody);
    }

    if (jsonVal_RestResponse)
    {
        json_value_free(jsonVal_RestResponse);
    }

    if (restResponse)
    {
        free(restResponse);
    }

    return httpStatus;
}

char*
PreProcessSetPresetIdPayload(
    JSON_Value* Payload)
{
    JSON_Object* jsonOjb_PresetId;
    JSON_Value* jsonVal_PresetId;
    JSON_Value* jsonVal_PresetObject    = NULL;
    JSON_Array* jsonArray_AntennaConfig = NULL;
    int i;

    if ((jsonOjb_PresetId = json_value_get_object(Payload)) == NULL)
    {
        LogError("R700 : Unable to retrieve command payload JSON object");
    }
    else
    {
        if ((jsonVal_PresetObject = json_object_get_value(jsonOjb_PresetId, g_presetObject)) == NULL)
        {
            LogError("R700 : Unable to retrieve %s for Set Preset Id payload JSON.", g_presetObject);
        }
        else if ((jsonArray_AntennaConfig = json_object_dotget_array(jsonOjb_PresetId, g_presetObjectAntennaConfigs)) != NULL)
        {
            int arrayCount = json_array_get_count(jsonArray_AntennaConfig);

            for (i = 0; i < arrayCount; i++)
            {
                JSON_Object* jsonObj_AntennaConfig = json_array_get_object(jsonArray_AntennaConfig, i);
                CleanAntennaConfig(jsonObj_AntennaConfig);
            }
        }
    }

    if (jsonVal_PresetObject)
    {
        return json_serialize_to_string(jsonVal_PresetObject);
    }
    return NULL;
}

char*
PreProcessTagPresenceResponse(
    JSON_Value* Payload)
{
    JSON_Object* jsonObj_Tag;
    size_t count;
    int i;
    char tagQueryBuffer[1024] = {0};
    char tagAntennaNumber[3]  = {0};
    char* tagQueryBufferPtr   = &tagQueryBuffer[0];
    char* returnBuffer        = NULL;

    if ((jsonObj_Tag = json_value_get_object(Payload)) == NULL)
    {
        LogError("R700 : Unable to retrieve Tag Presence payload JSON object");
    }
    else if ((count = json_object_get_count(jsonObj_Tag)) == 0)
    {
        LogInfo("R700 : Missing parameters");
    }
    else
    {
        for (i = 0; i < count; i++)
        {
            JSON_Value* jsonVal_TagParam;
            const char* tagName;
            const char* tagValue;
            int tagNumber;
            const char* prefix;

            if ((jsonVal_TagParam = json_object_get_value_at(jsonObj_Tag, i)) == NULL)
            {
                LogError("R700 : Unable to retrieve Inventory Tag parameter index %d", i);
            }
            else if ((tagName = json_object_get_name(jsonObj_Tag, i)) == NULL)
            {
                LogError("R700 : Unable to retrieve name for Inventory Tag index %d", i);
            }
            else
            {
                switch (json_value_get_type(jsonVal_TagParam))
                {
                    case JSONString:
                        if ((tagValue = json_object_get_string(jsonObj_Tag, tagName)) == NULL)
                        {
                            LogError("R700 : Unable to retrieve string for Inventory Tag index %d", i);
                            continue;
                        }

                        break;
                    case JSONNumber:
                        if ((tagNumber = (int)json_object_get_number(jsonObj_Tag, tagName)) == 0)
                        {
                            LogError("R700 : Unable to retrieve number for Inventory Tag index %d", i);
                            continue;
                        }

                        sprintf(tagAntennaNumber, "%d", tagNumber);
                        tagValue = tagAntennaNumber;

                        break;
                }

                if (i == 0)
                {
                    prefix = "?";
                }
                else
                {
                    prefix = "&";
                }
                sprintf(tagQueryBufferPtr, "%s%s=%s", prefix, tagName, tagValue);
                tagQueryBufferPtr = &tagQueryBuffer[0] + strlen(tagQueryBuffer);
            }
        }
        LogInfo("R700 : Inventory Tag Query %s", tagQueryBuffer);

        mallocAndStrcpy_s(&returnBuffer, tagQueryBuffer);
    }

    return returnBuffer;
}

PUPGRADE_DATA
ProcessDownloadFirmware(
    PIMPINJ_READER Reader,
    JSON_Value* Payload)
{
    const char* url                  = NULL;
    int isAutoReboot                 = -1;
    PUPGRADE_DATA upgradeData        = NULL;

    //LogInfo("R700 : %s()", __FUNCTION__);

    if (json_value_get_type(Payload) != JSONObject)
    {
        LogError("R700 : JSON field %s is not Object", g_upgradeFile);
    }
    else
    {
        JSON_Object* jsonObj_Upgrade = NULL;

        if ((jsonObj_Upgrade = json_value_get_object(Payload)) == NULL)
        {
            LogError("R700 : Unable to retrieve Upgrade Command payload JSON object");
        }
        else if ((url = json_object_get_string(jsonObj_Upgrade, g_upgradeFile)) == NULL)
        {
            LogError("R700 : Unable to retrieve '%s' JSON string", g_upgradeFile);
        }
        else
        {
            if ((isAutoReboot = json_object_get_boolean(jsonObj_Upgrade, g_upgradeAutoReboot)) == -1)
            {
                LogError("R700 : Unable to retrieve '%s' JSON boolean", g_upgradeAutoReboot);
                isAutoReboot = 0;
            }

            upgradeData = DownloadFile(url, isAutoReboot);
        }
    }

    return upgradeData;
}