-- ryz-app/1
local board = require('ryzobee')
local display = require('display')
local touch = require('touch')

math.randomseed(board.seed())
board.mark('seed', board.seed())
display.clear(0x0841)
display.text(8, 7, '2048', 0xffe0, 3)
display.show()

while true do
  local point = touch.read()
  if point.event ~= 'none' then
    board.mark('phase', point.event)
    board.mark('has_position', point.has_position)
    if point.x ~= nil then
      board.mark('pointer_x', point.x)
      board.mark('pointer_y', point.y)
      display.rect(point.x, point.y, 2, 2, point.pressed and 0x07e0 or 0xf800)
    end
    display.show()
  end
  board.sleep_ms(1)
end
