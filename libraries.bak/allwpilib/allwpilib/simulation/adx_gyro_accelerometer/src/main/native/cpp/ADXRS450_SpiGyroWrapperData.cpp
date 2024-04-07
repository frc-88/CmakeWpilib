/*----------------------------------------------------------------------------*/
/* Copyright (c) 2017-2018 FIRST. All Rights Reserved.                        */
/* Open Source Software - may be modified and shared by FRC teams. The code   */
/* must be accompanied by the FIRST BSD license file in the root directory of */
/* the project.                                                               */
/*----------------------------------------------------------------------------*/

#include "ADXRS450_SpiGyroWrapperData.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MockData/NotifyCallbackHelpers.h"
#include "MockData/SPIData.h"

#ifdef _WIN32
#include "Winsock2.h"
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

using namespace hal;

const double ADXRS450_SpiGyroWrapper::kAngleLsb = 1 / 0.0125 / 0.0005;
const double ADXRS450_SpiGyroWrapper::kMaxAngleDeltaPerMessage = 0.1875;
const int ADXRS450_SpiGyroWrapper::kPacketSize = 4;

template <class T>
constexpr const T& clamp(const T& value, const T& low, const T& high) {
  return std::max(low, std::min(value, high));
}

static void ADXRS450SPI_ReadBufferCallback(const char* name, void* param,
                                           uint8_t* buffer, uint32_t count) {
  auto sim = static_cast<ADXRS450_SpiGyroWrapper*>(param);
  sim->HandleRead(buffer, count);
}

static void ADXRS450SPI_ReadAutoReceivedData(const char* name, void* param,
                                             uint8_t* buffer, int32_t numToRead,
                                             int32_t* outputCount) {
  auto sim = static_cast<ADXRS450_SpiGyroWrapper*>(param);
  sim->HandleAutoReceiveData(buffer, numToRead, *outputCount);
}

ADXRS450_SpiGyroWrapper::ADXRS450_SpiGyroWrapper(int port) : m_port(port) {
  m_readCallbackId = HALSIM_RegisterSPIReadCallback(
      port, ADXRS450SPI_ReadBufferCallback, this);
  m_autoReceiveReadCallbackId = HALSIM_RegisterSPIReadAutoReceivedDataCallback(
      port, ADXRS450SPI_ReadAutoReceivedData, this);
}

ADXRS450_SpiGyroWrapper::~ADXRS450_SpiGyroWrapper() {
  HALSIM_CancelSPIReadCallback(m_port, m_readCallbackId);
  HALSIM_CancelSPIReadAutoReceivedDataCallback(m_port,
                                               m_autoReceiveReadCallbackId);
}

void ADXRS450_SpiGyroWrapper::ResetData() {
  std::lock_guard<wpi::mutex> lock(m_dataMutex);
  m_angle = 0;
  m_angleDiff = 0;
  m_angleCallbacks = nullptr;
}

void ADXRS450_SpiGyroWrapper::HandleRead(uint8_t* buffer, uint32_t count) {
  int returnCode = 0x00400AE0;
  std::memcpy(&buffer[0], &returnCode, sizeof(returnCode));
}

void ADXRS450_SpiGyroWrapper::HandleAutoReceiveData(uint8_t* buffer,
                                                    int32_t numToRead,
                                                    int32_t& outputCount) {
  std::lock_guard<wpi::mutex> lock(m_dataMutex);
  int32_t messagesToSend = std::abs(
      m_angleDiff > 0 ? std::ceil(m_angleDiff / kMaxAngleDeltaPerMessage)
                      : std::floor(m_angleDiff / kMaxAngleDeltaPerMessage));

  // Zero gets passed in during the "How much data do I need to read" step.
  // Else it is actually reading the accumulator
  if (numToRead == 0) {
    outputCount = messagesToSend * kPacketSize;
    return;
  }

  int valuesToRead = numToRead / kPacketSize;
  std::memset(&buffer[0], 0, numToRead);

  int msgCtr = 0;

  while (msgCtr < valuesToRead) {
    double cappedDiff =
        clamp(m_angleDiff, -kMaxAngleDeltaPerMessage, kMaxAngleDeltaPerMessage);

    int32_t valueToSend =
        ((static_cast<int32_t>(cappedDiff * kAngleLsb) << 10) & (~0x0C00000E)) |
        0x04000000;
    valueToSend = ntohl(valueToSend);

    std::memcpy(&buffer[msgCtr * kPacketSize], &valueToSend,
                sizeof(valueToSend));

    m_angleDiff -= cappedDiff;
    msgCtr += 1;
  }
}

int32_t ADXRS450_SpiGyroWrapper::RegisterAngleCallback(
    HAL_NotifyCallback callback, void* param, HAL_Bool initialNotify) {
  // Must return -1 on a null callback for error handling
  if (callback == nullptr) return -1;
  int32_t newUid = 0;
  {
    std::lock_guard<wpi::mutex> lock(m_registerMutex);
    m_angleCallbacks =
        RegisterCallback(m_angleCallbacks, "Angle", callback, param, &newUid);
  }
  if (initialNotify) {
    // We know that the callback is not null because of earlier null check
    HAL_Value value = MakeDouble(GetAngle());
    callback("Angle", param, &value);
  }
  return newUid;
}

void ADXRS450_SpiGyroWrapper::CancelAngleCallback(int32_t uid) {
  m_angleCallbacks = CancelCallback(m_angleCallbacks, uid);
}

void ADXRS450_SpiGyroWrapper::InvokeAngleCallback(HAL_Value value) {
  InvokeCallback(m_angleCallbacks, "Angle", &value);
}

double ADXRS450_SpiGyroWrapper::GetAngle() {
  std::lock_guard<wpi::mutex> lock(m_dataMutex);
  return m_angle;
}

void ADXRS450_SpiGyroWrapper::SetAngle(double angle) {
  std::lock_guard<wpi::mutex> lock(m_dataMutex);
  if (m_angle != angle) {
    InvokeAngleCallback(MakeDouble(angle));

    m_angleDiff += angle - m_angle;
    m_angle = angle;
  }
}
