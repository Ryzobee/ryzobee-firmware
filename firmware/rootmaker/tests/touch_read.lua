local touch = require("touch")
local board = require("ryzobee")
local info = touch.info()
local controllers = {[0xB5]="CST816T", [0xB6]="CST816D"}
assert(info.ready and controllers[info.chip_id] == info.controller)
assert(controllers[info.chip_id] and info.address == 0x15)
assert(info.width == 240 and info.height == 240)
assert(info.disable_auto_sleep == 1)
local events = {none=true, down=true, move=true, up=true}
for i = 1, 100 do
    local p = touch.read()
    assert(type(p.pressed) == "boolean" and events[p.event])
    assert(type(p.sampled_ms) == "number")
    assert(p.fingers == 0 or p.fingers == 1)
    assert(p.raw_event >= 0 and p.raw_event <= 3)
    assert(p.gesture >= 0 and p.gesture <= 255)
    if p.pressed then
        assert(p.fingers == 1 and (p.event == "down" or p.event == "move"))
        assert(p.x >= 0 and p.x < 240 and p.y >= 0 and p.y < 240)
    else
        assert(p.x == nil and p.y == nil)
    end
    board.sleep_ms(5)
end
local after = touch.info()
assert(after.reads >= info.reads + 100 and after.errors == info.errors)
print("TOUCH_READ_PASS", after.reads, after.errors)
