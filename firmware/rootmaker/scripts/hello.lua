-- @author: RyzoBee
-- @version: 0.1.0
-- @description: Basic Lua runtime and board information example.
local board = require("ryzobee")
local info = board.info()
print("Hello from a dynamically uploaded Lua script!")
print("Runtime:", _VERSION, info.idf)
print("Hardware:", info.chip, info.flash_bytes, info.psram_bytes)

local sum = 0
for _, value in ipairs({1, 2, 3, 4, 5}) do
    sum = sum + value
end
assert(sum == 15)
local started = board.millis()
board.sleep_ms(20)
assert(board.millis() - started >= 20)
print("LUA_SMOKE_PASS", "sum=" .. sum, "native_sleep=ok")
