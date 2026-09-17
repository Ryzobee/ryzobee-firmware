-- PROTOTYPE: prove that the current raw Lua firmware can run a playable 2048.
-- @author: RyzoBee
-- @version: 0.1.0-prototype
-- @description: Experimental 2048 game using the raw display API.
-- Swipe the board to move. Tap the 2048 title to start a new game.
local board = require('ryzobee')
local display = require('display')
local touch = require('touch')

local cells, score, status = {}, 0, ''
local names = {'LEFT', 'RIGHT', 'UP', 'DOWN'}

local function slot(line, position, direction)
  if direction == 1 then return line * 4 + position + 1 end
  if direction == 2 then return line * 4 + (3 - position) + 1 end
  if direction == 3 then return position * 4 + line + 1 end
  return (3 - position) * 4 + line + 1
end

local function move(direction)
  local changed = false
  for line = 0, 3 do
    local values, merged = {}, {}
    for position = 0, 3 do
      local value = cells[slot(line, position, direction)]
      if value ~= 0 then values[#values + 1] = value end
    end
    local i = 1
    while i <= #values do
      if i < #values and values[i] == values[i + 1] then
        local value = values[i] * 2
        merged[#merged + 1] = value
        score = score + value
        i = i + 2
      else
        merged[#merged + 1] = values[i]
        i = i + 1
      end
    end
    while #merged < 4 do merged[#merged + 1] = 0 end
    for position = 0, 3 do
      local index = slot(line, position, direction)
      if cells[index] ~= merged[position + 1] then changed = true end
      cells[index] = merged[position + 1]
    end
  end
  return changed
end

local function moves_left()
  for i = 1, 16 do if cells[i] == 0 then return true end end
  for row = 0, 3 do
    for column = 0, 3 do
      local i = row * 4 + column + 1
      if column < 3 and cells[i] == cells[i + 1] then return true end
      if row < 3 and cells[i] == cells[i + 4] then return true end
    end
  end
  return false
end

local function won()
  for i = 1, 16 do if cells[i] >= 2048 then return true end end
  return false
end

local function spawn()
  local empty = {}
  for i = 1, 16 do if cells[i] == 0 then empty[#empty + 1] = i end end
  if #empty == 0 then return false end
  cells[empty[math.random(#empty)]] = math.random(10) == 1 and 4 or 2
  return true
end

local function load(values)
  for i = 1, 16 do cells[i] = values[i] or 0 end
  score, status = 0, ''
end

local function expect(values)
  for i = 1, 16 do assert(cells[i] == (values[i] or 0), 'CELL ' .. i) end
end

local function gesture(dx, dy)
  local ax, ay = math.abs(dx), math.abs(dy)
  if (ax < 24 and ay < 24) or ax == ay then return nil end
  if ax > ay then return dx < 0 and 1 or 2 end
  return dy < 0 and 3 or 4
end

local function selftest()
  load({2, 2, 2, 2})
  assert(move(1) and score == 8)
  expect({4, 4})

  load({4, 4, 4})
  assert(move(1) and score == 8)
  expect({8, 4})

  load({2, 2, 4, 4})
  assert(move(1) and score == 12)
  expect({4, 8})

  load({2, 0, 2, 2})
  assert(move(1) and score == 4)
  expect({4, 2})

  load({2, 2, 4, 4})
  assert(move(2) and score == 12)
  expect({0, 0, 4, 8})

  load({2, 4, 0, 0, 2, 4, 0, 0, 4, 8, 0, 0, 4, 8, 0, 0})
  assert(move(4) and score == 36)
  expect({0, 0, 0, 0, 0, 0, 0, 0, 4, 8, 0, 0, 8, 16, 0, 0})

  load({2, 4, 2, 4, 4, 2, 4, 2, 2, 4, 2, 4, 4, 2, 4, 2})
  assert(not moves_left() and not move(1))

  load({2, 4, 2, 4, 4, 2, 4, 2, 2, 4, 2, 4, 4, 2, 4, 0})
  assert(moves_left())
  local before = 0
  for i = 1, 16 do if cells[i] == 0 then before = before + 1 end end
  assert(spawn())
  local after = 0
  for i = 1, 16 do
    if cells[i] == 0 then after = after + 1
    else assert(cells[i] > 0 and cells[i] % 2 == 0) end
  end
  assert(after == before - 1)
  assert(gesture(-30, 2) == 1 and gesture(30, 2) == 2)
  assert(gesture(2, -30) == 3 and gesture(2, 30) == 4)
  assert(gesture(10, 10) == nil and gesture(30, 30) == nil)
  print('RYZ_2048_SELFTEST_PASS')
end

local colors = {
  [0] = 0x3186, [2] = 0xef7d, [4] = 0xe6d8, [8] = 0xfc40,
  [16] = 0xfb20, [32] = 0xf9e0, [64] = 0xf800, [128] = 0xfea0,
  [256] = 0xfe60, [512] = 0xfe20, [1024] = 0xfdc0, [2048] = 0xffe0,
}

local function draw()
  display.clear(0x0841)
  display.text(8, 7, '2048', 0xffe0, 3)
  display.text(92, 10, 'SCORE ' .. score, 0xffff, 1)
  if status ~= '' then display.text(188, 10, status, 0x07ff, 1) end
  for row = 0, 3 do
    for column = 0, 3 do
      local value = cells[row * 4 + column + 1]
      local x, y = 14 + column * 54, 28 + row * 54
      display.rect(x, y, 50, 50, colors[value] or 0xf81f)
      if value ~= 0 then
        local text = tostring(value)
        local scale = #text <= 2 and 3 or (#text <= 4 and 2 or 1)
        local width, height = (#text * 6 - 1) * scale, 7 * scale
        local ink = value <= 4 and 0x2104 or 0xffff
        display.text(x + (50 - width) // 2, y + (50 - height) // 2,
                     text, ink, scale)
      end
    end
  end
  assert(display.read_pixel(14, 28) == (colors[cells[1]] or 0xf81f))
  assert(display.show())
end

local function reset()
  load({})
  spawn()
  spawn()
  draw()
  print('GAME START', score)
end

local function play(direction)
  if not move(direction) then
    if not moves_left() and status ~= 'OVER' then
      status = 'OVER'
      draw()
      print('GAME OVER', score)
    end
    return
  end
  spawn()
  if won() then status = 'WIN'
  elseif not moves_left() then status = 'OVER'
  else status = '' end
  draw()
  print('MOVE', names[direction], 'SCORE', score, status)
end

math.randomseed(board.millis())
math.random(); math.random(); math.random()
selftest()
local touch_info = touch.info()
assert(touch_info.ready and touch_info.width == 240 and touch_info.height == 240)
touch.demo(false)
reset()

local active, start_x, start_y, last_x, last_y = false, 0, 0, 0, 0
while true do
  local point = touch.read()
  if point.pressed and point.x ~= nil and point.y ~= nil then
    if not active then
      active, start_x, start_y = true, point.x, point.y
    end
    last_x, last_y = point.x, point.y
  elseif active and not point.pressed then
    active = false
    local dx, dy = last_x - start_x, last_y - start_y
    local direction = gesture(dx, dy)
    if direction == nil and math.abs(dx) < 24 and math.abs(dy) < 24 then
      if start_y < 28 then reset() end
    elseif direction ~= nil then play(direction) end
  end
  board.sleep_ms(20)
end
