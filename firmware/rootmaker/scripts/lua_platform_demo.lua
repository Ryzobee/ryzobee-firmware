-- ryz-app/1
-- @author: RyzoBee
-- @version: 0.1.0
-- @description: Wi-Fi status, explicit IMU sampling and coroutines.
-- Optional example, not a factory/autostart script. Configure Wi-Fi in C UI.
local board = require('ryzobee')
local wifi = require('wifi')
local imu = require('imu')

local function await_wifi()
    while true do
        local connected, reason = wifi.is_connected()
        if connected then return true end
        if reason then print('Wi-Fi:', reason); return false end
        board.sleep_ms(200)
    end
end

local function sample_imu()
    while true do
        local sample, reason = imu.read()
        if sample then
            print(sample.sequence, sample.x_mg, sample.y_mg, sample.z_mg)
        elseif reason ~= 'not_ready' then
            error(reason)
        end
        coroutine.yield()
    end
end

if await_wifi() then
    local initialized, reason = imu.init()
    if not initialized then print('IMU:', reason); return end
    local sampler = coroutine.create(sample_imu)
    -- BOOT long press requests cancellation of the whole job, including sampler.
    while true do
        local ok, reason = coroutine.resume(sampler)
        if not ok then error(reason) end
        -- This is an explicit scheduler; sleep_ms does not schedule coroutines.
        board.sleep_ms(40)
    end
end
