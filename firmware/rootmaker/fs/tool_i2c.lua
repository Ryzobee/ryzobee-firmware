-- ryz-app/1
-- @author: RyzoBee
-- @version: 1.0.0
-- @description: Public I2C API; Lua scans, pin drafts and bounded result view.
local ui, board, i2c = require('ui'), require('ryzobee'), require('i2c')
local O,B,D,W,A,G,R,Y = 0xfb40,0x39c7,0x2124,0xdedb,0xa514,0x362b,0xfa27,0xfd84
local M,N,F = 'mono_14','body_12','button_14'
local page,mounted,gen = 'scan','',nil
local current, bus, phase, fault = {board=true},nil,'idle',''
local rows,oldRows,offset,address = {},{},0,8
local dirty,ticks = true,0
local pending,oldCurrent,draft,pick,resumePhase
local pinPage = 0
local pins = {13,14,15,16,17,18,21,33,34,35,36,37,38,47,48}
local function copy(c)
  if c.board then return {board=true} end
  return {sda=c.sda,scl=c.scl,frequency_hz=c.frequency_hz or 100000}
end
local function close()
  if not bus then return true end
  local ok,reason = bus:close()
  if not ok then phase,fault,dirty = 'cleanup','CLOSE: '..reason:upper(),true; return false end
  bus = nil; return true
end
local function start(c)
  if not close() then return end
  pending,oldCurrent = copy(c or current),copy(current)
  oldRows,rows,offset,address = rows,{},0,8
  phase,fault,dirty = 'opening','',true
end
local function advance()
  if phase == 'opening' then
    local opened,reason = i2c.open(pending)
    if opened then bus,current,phase,page = opened,pending,'scanning','scan'
    else phase,fault = 'failed','OPEN: '..reason:upper() end
    dirty = true
  elseif phase == 'scanning' then
    local before = board.millis()
    local ack,reason = bus:probe(address,10)
    if ack == nil then
      phase,fault = 'failed',(#rows>0 and 'PARTIAL: ' or 'SCAN: ')..reason:upper()
      close()
    else
      if ack then rows[#rows+1] = {address,math.max(0,board.millis()-before)} end
      address = address+1
      if address > 119 then phase = 'done'; close() end
    end
    dirty = true
  end
end
local function paint()
  local list = {}
  local function obj(id,kind,x,y,w,h,text,font,fg,bg,align,parent)
    local o = {id=id,text=text,foreground=fg or W,background=bg or 0,visible=true,enabled=true}
    if mounted ~= page then
      o.kind,o.x,o.y,o.width,o.height = kind,x,y,w,h
      o.font,o.align,o.parent = font or N,align or 'left',parent
      o.line_height = (font == M or font == 'mono_semibold_14' or font == F) and 20 or 16
      if kind == 'viewport' then o.content_height = 126 end
      if kind == 'button' then o.radius = 2 end
    end
    list[#list+1] = o; return o
  end
  local function box(id,x,y,w,h,c,parent) return obj(id,'box',x,y,w,h,nil,nil,nil,c,nil,parent) end
  local function label(id,x,y,w,h,t,f,c,align,parent) return obj(id,'label',x,y,w,h,t,f,c,nil,align,parent) end
  local function button(id,x,y,w,h,t,active,enabled)
    local o = obj(id,'button',x,y,w,h,t,F,active and 0 or W,active and O or D,'center')
    o.enabled = enabled ~= false; return o
  end
  local running = phase == 'scanning' or phase == 'opening'
  if page == 'scan' then
  label('title',8,12,104,18,current.board and 'INTERNAL I2C0' or 'CUSTOM I2C1','mono_semibold_12',O)
  box('pill',168,13,64,16,D)
  label('found',168,13,64,16,#rows..(#rows<100 and ' FOUND' or ' ACK'),'mono_semibold_12',G,'center')
  local names = {'SDA / SCL','CLOCK','MODE'}
  local mode = phase == 'scanning' and string.format('SCAN 0x%02X',math.min(address,119)) or
    phase == 'opening' and 'OPENING BUS' or phase == 'cancelled' and 'CANCELLED' or
    phase == 'failed' and 'SCAN FAILED' or phase == 'cleanup' and 'CLOSE FAILED' or 'READ-ONLY SCAN'
  local values = {current.board and 'GPIO41 / GPIO40' or ('GPIO'..current.sda..' / GPIO'..current.scl),
    current.board and '100 kHz' or math.floor(current.frequency_hz/1000)..' kHz',mode}
  for i=1,3 do
    local y = 34+(i-1)*24
    box('line'..i,8,y+23,224,1,B)
    label('key'..i,12,y,i==1 and 68 or 62,24,names[i],i==1 and 'mono_semibold_12' or 'mono_semibold_14',A)
    label('value'..i,i==1 and 84 or 76,y,i==1 and 144 or 152,24,values[i],M,W,'right')
  end
  local view = obj('results','viewport',8,112,224,56,nil,nil,nil,0)
  view.scroll_y = offset%42
  local first = math.floor(offset/42)
  local empty = running and 'SCANNING...' or (phase == 'failed' or phase == 'cleanup') and 'SCAN INCOMPLETE' or phase == 'cancelled' and 'NO PREVIOUS DATA' or 'NO ADDRESSES'
  for i=1,3 do
    local row,y = rows[first+i],(i-1)*42
    box('row'..i,0,y,224,42,D,'results').visible = row ~= nil
    box('rail'..i,0,y,3,42,G,'results').visible = row ~= nil
    label('address'..i,8,y+2,50,20,row and string.format('0x%02X',row[1]) or ' ','mono_medium_13',G,nil,'results').visible = row ~= nil
    label('name'..i,64,y+2,152,20,row and 'ADDRESS' or i==1 and empty or ' ','mono_semibold_12',row and 0xffff or A,'right','results').visible = row ~= nil or i==1 and #rows==0
    label('ack'..i,64,y+22,152,16,row and ('ACK / '..(row[2]==0 and '<1' or row[2])..' ms') or ' ','mono_12',A,'right','results').visible = row ~= nil
  end
  local total = math.max(56,#rows*42)
  local thumbHeight = math.max(6,math.floor(56*56/total))
  local thumbY = 112+math.floor((56-thumbHeight)*offset/math.max(1,total-56))
  local thumb = box('thumb',230,thumbY,2,thumbHeight,O)
  thumb.y,thumb.height,thumb.visible = thumbY,thumbHeight,total>56
  button('scan',8,172,128,44,running and 'CANCEL' or phase == 'cleanup' and 'RETRY CLOSE' or 'SCAN AGAIN',true)
  button('pins',140,172,92,44,'PINS',false,not running and phase ~= 'cleanup')
  elseif page == 'pins' then
    box('pill',8,14,88,16,D)
    label('title',8,14,88,16,'CUSTOM BUS','mono_semibold_12',O,'center')
    label('warning',104,14,128,16,'Avoid internal devices','medium_12',Y,'right')
    local internal = button('internal',8,38,224,52,' ',false,not running)
    internal.background = D
    box('rail',8,38,3,52,Y)
    label('note1',16,40,208,16,'INTERNAL: SDA41 / SCL40','medium_12',W)
    label('note2',16,56,208,16,'Shared by touch and IMU','medium_12',W)
    label('note3',16,72,208,16,'TAP TO USE INTERNAL BUS','medium_12',A)
    local keys,ids = {'SDA PIN','SCL PIN','CLOCK'},{'sda','scl','clock'}
    local values = {draft.sda and ('GPIO'..draft.sda) or '--',draft.scl and ('GPIO'..draft.scl) or '--',math.floor(draft.frequency_hz/1000)..' kHz'}
    for i=1,3 do
      local y = 92+(i-1)*22
      local row = button(ids[i],8,y,224,22,' ',false,not running)
      row.background = 0
      box('line'..i,8,y+21,224,1,B)
      label('key'..i,12,y,62,22,keys[i],'mono_semibold_14',A)
      label('value'..i,76,y,152,22,values[i],M,i<3 and O or W,'right')
    end
    button('cancel',8,172,88,44,'CANCEL',false,not running)
    button('submit',100,172,132,44,'SCAN PINS',true,not running and draft.sda ~= nil and draft.scl ~= nil and draft.sda ~= draft.scl)
  else
    -- Figma has no picker frames: these local-only drafts complete its rows.
    label('title',8,12,224,20,page == 'clock' and 'SELECT CLOCK' or ('SELECT '..page:upper()..' PIN'),'mono_semibold_14',O)
    label('warning',8,32,224,16,'CHECK EXTERNAL WIRING','medium_12',Y)
    if page == 'clock' then
      for i,v in ipairs({100000,400000}) do
        button('c'..math.floor(v/1000),8,56+(i-1)*48,224,40,math.floor(v/1000)..' kHz',pick==v,true)
      end
    else
      for i=1,6 do
        local pin = pins[pinPage*6+i]
        local allowed = pin and pin ~= draft[page == 'sda' and 'scl' or 'sda']
        button(pin and 'p'..pin or 'empty'..i,8+(i-1)%2*114,52+math.floor((i-1)/2)*28,110,26,pin and ('GPIO'..pin) or ' ',pick==pin,allowed).visible = pin ~= nil
      end
      button('prev',8,140,110,28,'< PREV',false,pinPage>0)
      button('next',122,140,110,28,'NEXT >',false,(pinPage+1)*6<#pins)
    end
    button('cancel',8,172,88,44,'CANCEL',false,true)
    button('use',100,172,132,44,'USE',true,pick ~= nil)
  end
  label('footer',8,222,224,18,fault ~= '' and fault or phase == 'cancelled' and 'PREVIOUS RESULTS RESTORED' or ' ',N,fault ~= '' and R or A)
  if mounted ~= page then gen = ui.mount{id=page,background=0,objects=list}; mounted = page
  else ui.update(gen,list) end
  dirty = false
end
local function activate(id)
  if page == 'pins' then
    if id == 'cancel' then
      page,phase,fault = 'scan',resumePhase,''
      if pending then rows,current = oldRows,oldCurrent end
    elseif id == 'internal' then start({board=true})
    elseif id == 'submit' then start(draft)
    elseif id == 'sda' or id == 'scl' or id == 'clock' then
      page,pick,pinPage = id,id == 'clock' and draft.frequency_hz or draft[id],0
    end
  elseif page ~= 'scan' then
    if id == 'cancel' then page = 'pins'
    elseif id == 'use' then
      if page == 'clock' then draft.frequency_hz = pick else draft[page] = pick end
      page = 'pins'
    elseif id == 'next' then pinPage,mounted = pinPage+1,''
    elseif id == 'prev' then pinPage,mounted = pinPage-1,''
    elseif page == 'clock' then pick = tonumber(id:sub(2))*1000
    else pick = tonumber(id:sub(2)) end
  elseif id == 'pins' then
    draft = current.board and {frequency_hz=100000} or copy(current)
    page,resumePhase,fault,pending = 'pins',phase,'',nil
  elseif id == 'scan' then
    if phase == 'scanning' or phase == 'opening' then
      if close() then rows,current,offset,phase,fault = oldRows,oldCurrent,0,'cancelled','' end
    elseif phase == 'cleanup' then
      if close() then phase,fault = 'failed','RELEASED / SCAN AGAIN' end
    else start() end
    dirty = true
  end
end
start(); paint()
-- Only the firmware's physical BOOT cancellation exits the application.
while true do
  local event = ui.poll(page == 'scan' and 'results' or nil)
  if event then
    if event.kind == 'scroll' then offset = math.max(0,math.min(math.max(0,#rows*42-56),offset-event.dy))
    else activate(event.id) end
    paint()
  end
  advance()
  if mounted ~= page then paint() end
  board.sleep_ms(20); ticks = ticks+1
  if ticks==5 then ticks=0; if dirty then paint() end end
end
