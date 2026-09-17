-- Upload this file, then: lua --run-async --path workbench_demo.lua
-- @author: RyzoBee
-- @version: 0.1.0
-- @description: Async canvas and touch example for the workbench.
-- Stop explicitly with: lua --stop <job-id>
local board = require('ryzobee')
local display = require('display')
local touch = require('touch')
touch.demo(false)
local frames = 0
local next_log = 0
while true do
  local point = touch.read()
  display.clear(0x0841)
  display.text(18, 20, 'LUA WORKBENCH', 0xcff0, 2)
  display.text(18, 52, 'CONSOLE RUNNING', 0xffff, 2)
  display.text(18, 88, 'FRAME ' .. frames, 0x07ff, 2)
  if point.pressed then
    local x = math.min(math.max(point.x - 4, 0), 231)
    local y = math.min(math.max(point.y - 4, 0), 231)
    display.rect(x, y, 9, 9, 0xffe0)
  end
  display.show()
  frames = frames + 1
  if board.millis() >= next_log then
    print('frame', frames, 'pressed', point.pressed)
    next_log = board.millis() + 1000
  end
  board.sleep_ms(100)
end
