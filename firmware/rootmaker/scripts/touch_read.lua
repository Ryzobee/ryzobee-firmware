-- A bounded sample loop, not a persistent background Lua task.
-- @author: RyzoBee
-- @version: 0.1.0
-- @description: Read and print a bounded set of touch samples.
local touch = require("touch")
local board = require("ryzobee")
for i = 1, 30 do
    local p = touch.read()
    if p.pressed then print(p.event, p.x, p.y) end
    board.sleep_ms(20)
end
print("TOUCH_SAMPLE_DONE")
