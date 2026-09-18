-- ryz-app/1
-- @author: Unknown
-- @version: 0.1.0
-- @description: Print bounded BOOT click, double-click and long-press events; preserve system exit and report unavailable input.
local board = require('ryzobee')
local boot = require('boot')
local printed = 0
print('BOOT EVENTS: CLICK / DOUBLE_CLICK / LONG_PRESS')
print('A 3S LONG_PRESS CAN PRECEDE THE 5S SYSTEM EXIT')
for _ = 1, 3000 do
  local event, reason = boot.poll()
  if event then
    print(string.upper(event.type)..' HELD='..event.held_ms..
      'MS AT='..event.timestamp_ms)
    printed = printed + 1
  elseif reason == 'overflow' then
    print('BOOT OVERFLOW: RELEASE THE BUTTON AND TRY AGAIN')
    printed = printed + 1
  elseif reason and reason ~= 'busy' then
    print('BOOT ERROR: '..reason)
    return
  end
  if printed >= 24 then break end
  board.sleep_ms(20)
end
print('BOOT EVENT DEMO COMPLETE')
