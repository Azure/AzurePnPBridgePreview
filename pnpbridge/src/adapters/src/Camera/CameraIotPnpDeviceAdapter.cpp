// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "pch.h"

#include "CameraIotPnpDeviceAdapter.h"
#include "NetworkCameraIotPnpDevice.h"
#include "CameraIotPnpDevice.h"

// static
std::unique_ptr<CameraIotPnpDeviceAdapter> CameraIotPnpDeviceAdapter::MakeUnique(
    const std::string& componentName,
    const std::string& cameraId)
{
    auto newCameraDevice = std::make_unique<CameraIotPnpDeviceAdapter>(
        cameraId, componentName);

    newCameraDevice->Initialize();
    return newCameraDevice;
}

CameraIotPnpDeviceAdapter::CameraIotPnpDeviceAdapter(
    const std::string& cameraId,
    const std::string& componentName) :
    m_cameraId(cameraId),
    m_componentName(componentName),
    m_deviceClient(nullptr)
{
}

void CameraIotPnpDeviceAdapter::Initialize()
{
    m_cameraDiscovery = std::make_unique<CameraPnpDiscovery>();
    m_cameraDiscovery->InitializePnpDiscovery();
    auto result = m_cameraDiscovery->RegisterForCameraArrivalRemovals(
        std::bind(&CameraIotPnpDeviceAdapter::OnCameraArrivalRemoval, this));

    if (result != S_OK)
    {
        throw std::exception("Failed to register for camera arrivals.");
    }

    // Manually trigger camera arrival/removal once to immediately query for the camera device
    OnCameraArrivalRemoval();
}

void CameraIotPnpDeviceAdapter::OnCameraArrivalRemoval()
{
    std::wstring cameraName;
    std::lock_guard<std::mutex> lock(m_cameraDeviceLock);
    if (m_cameraDiscovery->GetUniqueIdByCameraId(m_cameraId, cameraName) == S_OK && !m_cameraDevice)
    {
        LogInfo("Camera device has arrived");

        // For now, assume:
        //   - IP camera IDs begin with "uuid"
        //   - Other camera IDs begin with "USB\"
        std::unique_ptr<CameraIotPnpDeviceAdapter> newCameraDevice;
        if (m_cameraId.rfind("uuid", 0) == 0)
        {
            m_cameraDevice = std::make_unique<NetworkCameraIotPnpDevice>(cameraName, m_componentName);
        }
        else
        {
            m_cameraDevice = std::make_unique<CameraIotPnpDevice>(cameraName, m_componentName);
        }

        if (m_cameraDevice->Initialize() == S_OK)
        {
            LogInfo("Camera device with camera name %s initialized successfully", (const char*) cameraName.c_str());
        }
        else
        {
            LogInfo("Camera device with camera name %s has already been initialized", (const char*) cameraName.c_str());
        }
    }
    else if (m_cameraDiscovery->GetUniqueIdByCameraId(m_cameraId, cameraName) != S_OK && m_cameraDevice)
    {
        LogInfo("Camera device has been removed, stopping telemetry");

        m_cameraDevice->StopTelemetryWorker();
        m_cameraDevice->Shutdown();
        m_cameraDevice.reset();
    }
    else if (m_cameraDiscovery->GetUniqueIdByCameraId(m_cameraId, cameraName) == S_OK && m_cameraDevice && m_hasStartedReporting)
    {
        LogInfo("Camera device has started, starting telemetry");
        m_cameraDevice->SetIotHubDeviceClientHandle(m_deviceClient);
        m_cameraDevice->StartTelemetryWorker();
    }
}

HRESULT CameraIotPnpDeviceAdapter::Start()
{
    m_hasStartedReporting = true;
    OnCameraArrivalRemoval();
    return S_OK;
}

void CameraIotPnpDeviceAdapter::Stop()
{
    m_hasStartedReporting = false;

    m_cameraDiscovery->Shutdown();

    std::lock_guard<std::mutex> lock(m_cameraDeviceLock);
    if (m_cameraDevice)
    {
        m_cameraDevice->StopTelemetryWorker();
        m_cameraDevice->Shutdown();
    }
}

int CameraIotPnpDeviceAdapter::CameraPnpCallback_ProcessCommandUpdate(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* CommandName,
    JSON_Value* CommandValue,
    unsigned char** CommandResponse,
    size_t* CommandResponseSize)
{
    AZURE_UNREFERENCED_PARAMETER(CommandValue);
    auto CameraDevice = static_cast<CameraIotPnpDeviceAdapter*>(PnpComponentHandleGetContext(PnpComponentHandle));

    std::lock_guard<std::mutex> lock(CameraDevice->m_cameraDeviceLock);
    if (CameraDevice->m_cameraDevice)
    {
        if (strcmp(CommandName, "TakePhoto") == 0)
        {
            return CameraDevice->TakePhoto(CommandResponse, CommandResponseSize);
        }
        else if (strcmp(CommandName, "TakeVideo") == 0)
        {
            return CameraDevice->TakeVideo(CommandResponse, CommandResponseSize);
        }
        else if (strcmp(CommandName, "GetURI") == 0)
        {
            return CameraDevice->GetURI(CommandResponse, CommandResponseSize);
        }
        else
        {
            LogError("Unknown command request");
            *CommandResponseSize = 0;
            return PNP_STATUS_INTERNAL_ERROR;
        }
    }
    else
    {
        LogError("Camera device was not available to process command");
        *CommandResponseSize = 0;
        return PNP_STATUS_INTERNAL_ERROR;
    }
}

// static
void CameraIotPnpDeviceAdapter::CameraPnpCallback_ProcessPropertyUpdate(
    PNPBRIDGE_COMPONENT_HANDLE /* PnpComponentHandle */,
    const char* /* PropertyName */,
    JSON_Value* /* PropertyValue */,
    int /* version */,
    void* /* userContextCallback */)
{
    // no-op
}

int CameraIotPnpDeviceAdapter::TakePhoto(
    unsigned char** CommandResponse,
    size_t* CommandResponseSize)
{
    int result = PNP_STATUS_SUCCESS;
    std::string strResponse;

    std::lock_guard<std::mutex> lock(m_cameraDeviceLock);
    if (m_cameraDevice)
    {
        if (m_hasStartedReporting)
        {
            m_cameraDevice->StartTelemetryWorker();
        }

        if (S_OK == m_cameraDevice->TakePhotoOp(strResponse))
        {
            mallocAndStrcpy_s((char**)CommandResponse, strResponse.c_str());
            *CommandResponseSize = strlen(strResponse.c_str());
        }
    }
    else
    {
        LogError("Error taking photo");
        *CommandResponseSize = 0;
        result = PNP_STATUS_INTERNAL_ERROR;
    }

    return result;
}

int CameraIotPnpDeviceAdapter::TakeVideo(
    unsigned char** CommandResponse,
    size_t* CommandResponseSize)
{
    int result = PNP_STATUS_SUCCESS;
    std::string strResponse;

    std::lock_guard<std::mutex> lock(m_cameraDeviceLock);
    if (m_cameraDevice)
    {
        if (m_hasStartedReporting)
        {
            m_cameraDevice->StartTelemetryWorker();
        }

        if (S_OK == m_cameraDevice->TakeVideoOp(10000, strResponse))
        {
            mallocAndStrcpy_s((char**)CommandResponse, strResponse.c_str());
            *CommandResponseSize = strlen(strResponse.c_str());
        }
    }
    else
    {
        LogError("Error taking video");
        *CommandResponseSize = 0;
        result = PNP_STATUS_INTERNAL_ERROR;
    }

    return result;
}

int CameraIotPnpDeviceAdapter::GetURI(
    unsigned char** CommandResponse,
    size_t* CommandResponseSize)
{

    int result = PNP_STATUS_SUCCESS;
    std::string strResponse;

    std::lock_guard<std::mutex> lock(m_cameraDeviceLock);
    if (m_cameraDevice && (S_OK == m_cameraDevice->GetURIOp(strResponse)))
    {
        mallocAndStrcpy_s((char**)CommandResponse, strResponse.c_str());
        *CommandResponseSize = strlen(strResponse.c_str());
    }
    else
    {
        LogError("Error taking photo");
        *CommandResponseSize = 0;
        result = PNP_STATUS_INTERNAL_ERROR;
    }

    return result;
}

void CameraIotPnpDeviceAdapter::SetIotHubDeviceClientHandle(
    IOTHUB_DEVICE_CLIENT_HANDLE DeviceClientHandle)
{
    m_deviceClient = DeviceClientHandle;
}
