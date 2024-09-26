#include "MIO12V.hpp"

#include "pico/time.h"
#include "projdefs.h"
#include "task/BaseTask.hpp"

#include <cstdint>

namespace Fan
{

constexpr int MODBUS_ADDR = 1;
constexpr int SPEED_REG_ADDR = 0x0000;
constexpr int ROT_REG_ADDR = 0x0004;

MIO12V::MIO12V(std::shared_ptr<Modbus::Client> modbus,
               uint32_t stackDepth,
               Task::priority taskPriority) :
    Task::BaseTask("FanController", stackDepth, this, taskPriority),
    m_FanSpeed{0},
    m_FanSpeedRegister{modbus, MODBUS_ADDR, SPEED_REG_ADDR},
    m_FanRotationRegister{modbus, MODBUS_ADDR, ROT_REG_ADDR, false}
{
    m_FanSpeed = m_FanSpeedRegister.read();
    vTaskDelay(pdMS_TO_TICKS(5));
}

uint16_t MIO12V::getFanRotation()
{
    return m_FanRotationRegister.read();
    vTaskDelay(pdMS_TO_TICKS(5));
}

uint16_t MIO12V::getFanSpeed() { return m_FanSpeed; }

void MIO12V::setFanSpeed(int speed)
{
    if (speed < 0 || speed > 1000) { speed = speed < 1000 ? 0 : 1000; }

    m_FanSpeedRegister.write(speed);
    vTaskDelay(pdMS_TO_TICKS(5));
    m_FanSpeed = m_FanSpeedRegister.read();
    vTaskDelay(pdMS_TO_TICKS(5));
}

void MIO12V::run()
{
    uint16_t newSpeed = 0;

    while (true)
    {
        // TODO: block on queue waiting for new fan speed setting
        if (true) { setFanSpeed(newSpeed); }
    }
}

} // namespace Fan