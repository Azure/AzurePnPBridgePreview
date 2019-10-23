// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
#include "pch.h"
#include <fstream>
#include "NetworkCameraIotPnpDevice.h"
#include <pnpbridge_common.h>
#include <pnpbridge.h>

NetworkCameraIotPnpDevice::NetworkCameraIotPnpDevice(std::wstring& deviceName)
    : CameraIotPnpDevice(deviceName)
{
}

NetworkCameraIotPnpDevice::~NetworkCameraIotPnpDevice()
{
}

HRESULT NetworkCameraIotPnpDevice::Initialize(_In_ DIGITALTWIN_INTERFACE_CLIENT_HANDLE hPnpClientInterface) try
{
    CameraIotPnpDevice::Initialize(hPnpClientInterface);

    HRESULT hr = S_OK;
    ULONG cb = 0;
    std::unique_ptr<BYTE[]> pbURI;
    RETURN_IF_FAILED(CameraPnpDiscovery::GetDeviceProperty(m_deviceName.c_str(),
        &DEVPKEY_Device_LocationInfo,
        nullptr,
        0,
        &cb));
    pbURI = std::make_unique<BYTE[]>(cb);
    RETURN_IF_FAILED(CameraPnpDiscovery::GetDeviceProperty(m_deviceName.c_str(),
        &DEVPKEY_Device_LocationInfo,
        pbURI.get(),
        cb,
        &cb));
    m_URI = std::wstring((LPCWSTR)pbURI.get());
    return hr;
}
catch (std::exception e)
{
    LogInfo("Exception occurred initializing Camera (%s)", e.what());
    return E_UNEXPECTED;
}

HRESULT NetworkCameraIotPnpDevice::GetURIOp(std::string& strResponse) try
{
    HRESULT hr = S_OK;
    std::string loc(m_URI.begin(), m_URI.end());
    strResponse = "\"" + loc + "\"";
    return hr;
}
catch (std::exception e)
{
    LogInfo("Exception occurred in get URI operation (%s)", e.what());
    return E_UNEXPECTED;
}
