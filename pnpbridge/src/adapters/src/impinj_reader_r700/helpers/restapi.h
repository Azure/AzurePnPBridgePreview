// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full
// license information.

#pragma once

#ifndef R700_RESTAPI_H
#define R700_RESTAPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <parson.h>
#include "azure_c_shared_utility/xlogging.h"
#include "../curl_wrapper/curl_wrapper.h"
#include "../impinj_reader_r700.h"
#include "pnp_utils.h"

#define R700_STATUS_OK                  200
#define R700_STATUS_CREATED             201
#define R700_STATUS_ACCEPTED            202   // StatusResponse
#define R700_STATUS_NO_CONTENT          204
#define R700_STATUS_BAD_REQUEST         400   // ErrorResponse
#define R700_STATUS_FORBIDDEN           403   // ErrorResponse
#define R700_STATUS_NOT_FOUND           404   // ErrorResponse
#define R700_STATUS_NOT_ALLOWED         405   // ErrorResponse
#define R700_STATUS_CONFLICT            409   // ErrorResponse
#define R700_STATUS_INTERNAL_ERROR      500   // ErrorResponse
#define R700_STATUS_NOT_IMPLEMENTED     501   // ErrorResponse
#define R700_STATUS_SERVICE_UNAVAILABLE 503   // ErrorResponse

#define IsSuccess(code) ((code & 200) == 200)

static const char impinjReader_property_system_region_selectableRegions[] = "selectableRegions";

static const char g_presetSchemaFormat[]         = "{\"PresetScheme\":%s}";
static const char g_presetsFormat[]              = "{\"PresetsIds\":%s}";
static const char g_emptyCommandResponse[]       = "{}";
static const char g_successResponse[]            = "Operation Success";
static const char g_errorResponseToDescription[] = "%s %s %s";
static const char g_unsupportedApiResponse[]     = "{\"status\":\"API Not Supported\"}";
static const char g_notImplementedApiResponse[]  = "{\"status\":\"API Not Implemented\"}";
static const char g_rebootSuccessResponse[]      = "{\"message\":\"The system has begun the shutdown and startup process.\"}";
static const char g_rebootPendingResponse[]      = "{\"message\":\"Reboot Pending.\"}";
static const char g_shuttingDownResponse[]       = "{\"message\":\"Shutting down.\"}";
static const char g_restApiNotEnabled[]          = "{\"message\":\"RESTFul API not enabled.\"}";
static const char g_upgradeAccepted[]            = "{\"message\":\"The image file was uploaded successfully and is being installed on the reader.\"}";
static const char g_upgradeFailAlloc[]           = "{\"message\":\"Failed to allocate download data structure.\"}";
static const char g_upgradeFailDownload[]        = "{\"status\":\"Downlaod Failed\", \"message\":\"Failed to download upgrade firmware.\"}";
static const char g_upgradeNotReady[]            = "{\"status\":\"Not Ready\", \"message\":\"Reader is not ready for upgrade.\"}";

// Array in Kafka payload
static const char g_kafkaBootstraps[] = "bootstraps";

// For Error Response
static const char g_errorResponseMessage[]           = "message";
static const char g_errorResponseInvalidPropertyId[] = "invalidPropertyId";
static const char g_errorResponseDetail[]            = "detail";
static const char g_responseFormat[]                 = "{\"message\":\"%s\"}";
static const char g_NotSupportedMessage[]            = "\"Not Supported\"";
// For Status Response
static const char g_statusResponseMessage[] = "message";

// Preset ID command payload
static const char g_presetId[]         = "presetId";
static const char g_presetObjectJSON[] = "presetObjectJSON";

// Preset ID configuration
static const char g_presetObject[]                                = "presetObject";
static const char g_presetObjectAntennaConfigs[]                  = "presetObject.antennaConfigs";
static const char g_presetObjectAntennaConfigsTagAuthentication[] = "tagAuthentication";
static const char g_presetObjectAntennaConfigsPowerSweeping[]     = "powerSweeping";
static const char g_antennaConfigFiltering[]                      = "filtering";
static const char g_antennaConfigFilters[]                        = "filters";

// Upgrade Firmware
static const char g_upgradeFile[]       = "upgradeFile";
static const char g_upgradeAutoReboot[] = "autoReboot";


#define R700_PRESET_ID_LENGTH 128 + 32

#define R700_REST_REQUEST_VALUES                       \
    READER_STATUS_GET,                                 \
        READER_STATUS,                                 \
        READER_STATUS_POLL,                            \
        HTTP_STREAM,                                   \
        MQTT,                                          \
        KAFKA,                                         \
        PROFILES,                                      \
        PROFILES_STOP,                                 \
        PROFILES_INVENTORY_PRESETS_SCHEMA,             \
        PROFILES_INVENTORY_PRESETS_IDS,                \
        PROFILES_INVENTORY_PRESETS_ID_GET,             \
        PROFILES_INVENTORY_PRESETS_ID_SET,             \
        PROFILES_INVENTORY_PRESETS_ID_SET_PASSTHROUGH, \
        PROFILES_INVENTORY_PRESETS_ID_DELETE,          \
        PROFILES_START,                                \
        PROFILES_INVENTORY_TAG,                        \
        SYSTEM,                                        \
        SYSTEM_HOSTNAME,                               \
        SYSTEM_IMAGE,                                  \
        SYSTEM_IMAGE_UPGRADE_UPLOAD,                   \
        SYSTEM_IMAGE_UPGRADE,                          \
        SYSTEM_IMAGE_UPGRADE_GET,                      \
        SYSTEM_NETORK_INTERFACES,                      \
        SYSTEM_POWER,                                  \
        SYSTEM_POWER_SET,                              \
        SYSTEM_REGION,                                 \
        SYSTEM_REGION_GET,                             \
        SYSTEM_REBOOT,                                 \
        SYSTEM_RFID_LLRP,                              \
        SYSTEM_RFID_INTERFACE,                         \
        SYSTEM_TIME,                                   \
        SYSTEM_TIME_GET,                               \
        SYSTEM_TIME_SET,                               \
        SYSTEM_TIME_NTP,                               \
        SYSTEM_TIME_NTP_SET,                           \
        R700_REST_MAX

MU_DEFINE_ENUM_WITHOUT_INVALID(R700_REST_REQUEST, R700_REST_REQUEST_VALUES);

typedef enum _R700_DTDL_TYPE
{
    READONLY,
    WRITABLE,
    COMMAND,
    DTDL_NONE,
} R700_DTDL_TYPE;

typedef enum _R700_REST_TYPE
{
    GET,
    PUT,
    POST,
    POST_UPLOAD,
    DELETE,
    UNSUPPORTED_REST,
} R700_REST_TYPE;

typedef enum _R700_UPGRADE_STATUS
{
    UNKNOWN = 0,
    READY,
    VERIFYING,
    INSTALLING,
    SUCCESSFUL,
    REBOOT_REQUIRED,
    FAILED
} R700_UPGRADE_STATUS;

#define R700_ENDPONT_LENGTH 48

typedef struct _IMPINJ_R700_REST
{
    R700_REST_VERSION ApiVersion;
    R700_REST_REQUEST Request;
    R700_DTDL_TYPE DtdlType;
    R700_REST_TYPE RestType;
    bool IsRestRequired;
    char EndPoint[R700_ENDPONT_LENGTH];
    char Name[64];
} IMPINJ_R700_REST, *PIMPINJ_R700_REST;

static IMPINJ_R700_REST R700_REST_LIST[] = {
    {V1_0, READER_STATUS_GET, COMMAND, GET, true, "/status", "GetReaderStatus"},
    {V1_0, READER_STATUS, READONLY, GET, true, "/status", "ReaderStatus"},
    {V1_0, READER_STATUS_POLL, READONLY, GET, true, "/status", "ReaderStatus"},
    {V1_0, HTTP_STREAM, WRITABLE, PUT, true, "/http-stream", "StreamConfiguration"},
    {V1_0, MQTT, WRITABLE, PUT, true, "/mqtt", "MqttConfiguration"},
    {V1_2, KAFKA, WRITABLE, PUT, true, "/kafka", "KafkaConfiguration"},
    {V1_0, PROFILES, READONLY, GET, true, "/profiles", "Profiles"},
    {V1_0, PROFILES_STOP, COMMAND, POST, true, "/profiles/stop", "StopPreset"},
    {V1_0, PROFILES_INVENTORY_PRESETS_SCHEMA, COMMAND, GET, true, "/profiles/inventory/presets-schema", "PresetsSchema"},
    {V1_0, PROFILES_INVENTORY_PRESETS_IDS, COMMAND, GET, true, "/profiles/inventory/presets", "Presets"},
    {V1_0, PROFILES_INVENTORY_PRESETS_ID_GET, COMMAND, GET, true, "/profiles/inventory/presets/%s", "GetPresetId"},
    {V1_0, PROFILES_INVENTORY_PRESETS_ID_SET, COMMAND, PUT, true, "/profiles/inventory/presets/%s", "SetPresetId"},
    {V1_0, PROFILES_INVENTORY_PRESETS_ID_SET_PASSTHROUGH, COMMAND, PUT, true, "/profiles/inventory/presets/%s", "SetPresetIdPassthrough"},
    {V1_0, PROFILES_INVENTORY_PRESETS_ID_DELETE, COMMAND, DELETE, true, "/profiles/inventory/presets/%s", "DeletePresetId"},
    {V1_0, PROFILES_START, COMMAND, POST, true, "/profiles/inventory/presets/%s/start", "StartPreset"},
    {V1_2, PROFILES_INVENTORY_TAG, COMMAND, GET, true, "/profiles/inventory/tag%s", "TagPresenceResponse"},
    {V1_3, SYSTEM, READONLY, GET, false, "/system", "SystemInfo"},
    {V1_3, SYSTEM_HOSTNAME, WRITABLE, PUT, false, "/system/hostname", "Hostname"},
    {V1_3, SYSTEM_IMAGE, READONLY, GET, false, "/system/image", "SystemImage"},
    {V1_3, SYSTEM_IMAGE_UPGRADE_UPLOAD, COMMAND, POST_UPLOAD, false, "/system/image/upgrade", "UpgradeUpload"},
    {V1_3, SYSTEM_IMAGE_UPGRADE, READONLY, GET, false, "/system/image/upgrade", "UpgradeStatus"},
    {V1_3, SYSTEM_IMAGE_UPGRADE_GET, COMMAND, GET, false, "/system/image/upgrade", "GetUpgradeStatus"},
    {V1_3, SYSTEM_NETORK_INTERFACES, READONLY, GET, false, "/system/network/interfaces", "NetworkInterface"},
    {V1_3, SYSTEM_POWER, READONLY, GET, false, "/system/power", "PowerConfiguration"},
    {V1_3, SYSTEM_POWER_SET, COMMAND, PUT, false, "/system/power", "SetPowerConfiguration"},
    {V1_3, SYSTEM_REGION, WRITABLE, PUT, false, "/system/region", "RegionInfo"},
    {V1_3, SYSTEM_REGION_GET, COMMAND, GET, false, "/system/region", "GetRegionInfo"},
    {V1_3, SYSTEM_REBOOT, COMMAND, POST, false, "/system/reboot", "Reboot"},
    {V1_3, SYSTEM_RFID_LLRP, READONLY, GET, false, "/system/rfid/llrp", "LlrpStatus"},
    {V1_3, SYSTEM_RFID_INTERFACE, WRITABLE, PUT, false, "/system/rfid/interface", "RfidInterface"},
    {V1_3, SYSTEM_TIME, READONLY, GET, false, "/system/time", "TimeInfo"},
    {V1_3, SYSTEM_TIME_GET, COMMAND, GET, false, "/system/time", "GetTimeInfo"},
    {V1_3, SYSTEM_TIME_SET, COMMAND, PUT, false, "/system/time", "SetTimeInfo"}};

JSON_Value*
ImpinjReader_RequestDelete(
    PIMPINJ_READER Device,
    PIMPINJ_R700_REST R700_Request,
    const char* Parameter,
    int* HttpStatus);

JSON_Value*
ImpinjReader_RequestGet(
    PIMPINJ_READER Device,
    PIMPINJ_R700_REST R700_Request,
    const char* Parameter,
    int* HttpStatus);

JSON_Value*
ImpinjReader_RequestPut(
    PIMPINJ_READER Device,
    PIMPINJ_R700_REST R700_Request,
    const char* Parameter,
    const char* Body,
    int* HttpStatus);

JSON_Value*
ImpinjReader_RequestPost(
    PIMPINJ_READER Device,
    PIMPINJ_R700_REST R700_Request,
    const char* Parameter,
    int* HttpStatus);

R700_UPGRADE_STATUS
CheckUpgardeStatus(
    PIMPINJ_READER Reader,
    JSON_Value** JsonValue);

JSON_Value*
ImpinjReader_RequestUpgrade(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    PIMPINJ_R700_REST R700_Request,
    PUPGRADE_DATA Upgrade_Data,
    int* HttpStatus);

const char*
ImpinjReader_ProcessResponse(
    IMPINJ_R700_REST* RestRequest,
    JSON_Value* JsonVal_Response,
    int HttpStatus);

char*
ImpinjReader_ProcessErrorResponse(
    JSON_Value* JsonVal_ErrorResponse,
    R700_DTDL_TYPE DtdlType);

PUPGRADE_DATA
ImpinjReader_Init_UpgradeData(
    PIMPINJ_READER Reader,
    char* Endpoint);

JSON_Value*
ImpinjReader_Convert_UpgradeStatus(
    char* Json_String);

#ifdef __cplusplus
}
#endif

#endif /* R700_RESTAPI_H */