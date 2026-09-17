-- Native test UI continues after this short-lived Lua VM exits.
-- @author: RyzoBee
-- @version: 0.1.0
-- @description: Enable the native touch diagnostic screen.
-- Any subsequent display drawing disables the demo so it cannot overwrite you.
local touch = require("touch")
assert(touch.info().ready)
assert(touch.demo(true))
print("TOUCH_DEMO_READY")
