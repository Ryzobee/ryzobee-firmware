-- ryz-app/1
-- @author: RyzoBee
-- @version: 1.0.0
-- @description: UI update diagnostic; not a factory hardware test.
local board = require('ryzobee')
local ui = require('ui')
local ticks = 0
board.mark('updates', 0)
board.mark('exited', false)
local generation = ui.mount({
  id = 'live_update', background = 0x0000,
  objects = {
    {id='frame',kind='box',x=8,y=8,width=224,height=224,
      background=0x1082,border=0xfb40,border_width=1},
    {id='title',kind='label',x=16,y=16,width=208,height=34,
      text='LIVE UPDATE',font='title_24',foreground=0xffff,align='center'},
    {id='hint',kind='label',x=16,y=62,width=208,height=28,
      text='UI DIAGNOSTIC',font='body_16',foreground=0xa514,align='center'},
    {id='counter',kind='label',x=16,y=101,width=208,height=28,
      text='UPDATES 0',font='body_16',foreground=0xfb40,align='center'},
    {id='instruction',kind='label',x=16,y=140,width=208,height=26,
      text='HOLD EXIT, THEN RELEASE',font='body_16',foreground=0xffff,align='center'},
    {id='exit',kind='button',x=24,y=180,width=192,height=40,
      text='EXIT',font='body_16',foreground=0x0000,background=0xfb40,
      border=0xfb40,border_width=1},
  },
})
local loops = 0
while true do
  local event = ui.poll()
  if event and event.id == 'exit' then
    board.mark('exited', true)
    board.mark('scene_generation', event.generation)
    break
  end
  loops = loops + 1
  if loops == 10 then
    loops = 0
    ticks = (ticks + 1) % 10000
    board.mark('updates', ticks)
    assert(ui.update(generation, {{id='counter',text='UPDATES '..ticks}}) == generation)
  end
  board.sleep_ms(20)
end
