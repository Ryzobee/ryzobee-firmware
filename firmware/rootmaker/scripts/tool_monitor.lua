-- ryz-app/1
-- @author: RyzoBee
-- @version: 2.0.0
-- @description: Public UART/log APIs, Lua-owned history and RSPORT config.
-- Maintained source; build_factory_monitor.mjs produces editable factory Lua.
local ui, board = require('ui'), require('ryzobee')
local uart, log = require('uart'), require('log')
local O,B,D,W,A,R,Y = 0xfb40,0x39c7,0x18c3,0xdedb,0xa514,0xfa27,0xfd84
local M,N,F,T = 'mono_12','body_12','button_14','display_20'
local bauds = {9600,19200,38400,57600,115200,230400,460800,921600,1200,2400,4800}
local ports = {15,19,13,17}
local formats = {{8,'none',1,'8N1','8 / NONE / 1'},{8,'none',2,'8N2','8 / NONE / 2'},
  {8,'even',1,'8E1','8 / EVEN / 1'},{8,'odd',1,'8O1','8 / ODD / 1'},
  {7,'even',1,'7E1','7 / EVEN / 1'},{7,'odd',1,'7O1','7 / ODD / 1'}}
local page,mounted,gen = 'monitor','',nil
local current = {source='system',baud=115200,rx=15,tx=16,format=1}
local stream,op,pending,draft,pick,closing
local rows,colors,tail,cr,fault,gap = {},{},'',false,'',false
local head,count,scrollOffset,baudOffset,ticks = 0,0,0,0,0
local hold,frozen,frozenColors = false,{},{}
local overwritten,dirty,lastDrops,lastErrors,lastLoss,level = false,true,'0','0',false,W
local function reset()
  rows,colors,tail,cr,head,count,scrollOffset = {},{},'',false,0,0,0
  frozen,frozenColors,overwritten,gap,level = {},{},false,false,W
  dirty = true
end
local function line()
  -- Only SYSTEM has a documented severity prefix. Arbitrary UART bytes do not.
  if current.source == 'system' then
    local severity = tail:match('^([EWIDV]) %(%d+%) ')
    if severity then level = severity == 'E' and R or severity == 'W' and Y or W end
  else level = W end
  head = head%64+1
  rows[head],colors[head] = tail == '' and ' ' or tail,level
  if count == 64 then overwritten = true else count = count+1 end
  tail = ''
end
local function feed(bytes)
  for i=1,#bytes do
    local c = bytes:byte(i)
    if c == 10 then if not cr then line() end
    elseif c == 13 then line()
    else
      local text = c == 9 and '    ' or c >= 32 and c <= 126 and string.char(c) or string.format('[%02X]',c)
      for j=1,#text do
        if #tail == 28 then line() end
        tail = tail..text:sub(j,j)
      end
    end
    cr = c == 13
  end
end
local function copy(c)
  return {source=c.source,baud=c.baud,rx=c.rx,tx=c.tx,format=c.format}
end
local function begin(c)
  pending,op,fault,dirty = copy(c),stream and 'CLOSE' or 'OPEN','',true
end
local function advance()
  if not op then return end
  if op == 'CLOSE' then
    -- A failed close can leave a partially released native resource. Keep its
    -- ownership token only for retrying close, never for another read.
    closing = true
    local ok,reason = stream:close()
    if not ok then fault,op,dirty = 'CLOSE: '..reason:upper(),nil,true; return end
    stream,op,closing = nil,'OPEN',false
  else
    local opened,reason
    if pending.source == 'system' then opened,reason = log.open{level='verbose'}
    else
      local f = formats[pending.format]
      -- TX is intentionally omitted: this application only listens.
      opened,reason = uart.open{port=1,rx=pending.rx,baud=pending.baud,bits=f[1],parity=f[2],stop=f[3]}
    end
    if opened then
      stream,current,hold,page = opened,pending,false,'monitor'
      lastDrops,lastErrors,lastLoss = '0','0',false; reset()
    else fault = 'OPEN: '..reason:upper() end
    op,dirty = nil,true
  end
end
local function poll()
  if not stream or closing or op then return end
  -- Bounded work each 20ms; reception continues while the display is paused.
  for _=1,8 do
    local bytes,status = stream:read(256,0)
    if not bytes then fault,dirty = 'READ: '..status:upper(),true; return end
    if fault:sub(1,5) == 'READ:' then fault,dirty = '',true end
    local dropped,errors = status.dropped_bytes or '0',status.error_events or '0'
    if dropped ~= lastDrops or errors ~= lastErrors or current.source == 'system' and status.loss_possible and not lastLoss then
      if not gap then dirty = true end
      gap,tail,cr = true,'',false
    end
    lastDrops,lastErrors,lastLoss = dropped,errors,status.loss_possible or false
    if #bytes == 0 then break end
    feed(bytes)
    if not hold and page == 'monitor' then dirty = true end
  end
end
local function freeze()
  frozen,frozenColors = {},{}
  for i=1,count do
    local k = (head-count+i-1)%64+1
    frozen[i],frozenColors[i] = rows[k],colors[k]
  end
  if #tail>0 then frozen[#frozen+1],frozenColors[#frozenColors+1] = tail,level end
  scrollOffset = 0
end
local function port(c)
  for i,v in ipairs(ports) do
    if c.rx == v and c.tx == v+1 or c.tx == v and c.rx == v+1 then return 'P'..i end
  end
  return 'CUSTOM'
end
local function paint()
  local list = {}
  local ready = stream ~= nil and not closing and not op
  local live = ready
  local function obj(id,kind,x,y,w,h,text,font,fg,bg,align,enabled,parent,extra)
    local o = {id=id,text=text,foreground=fg or W,background=bg or 0,enabled=enabled ~= false}
    if kind == 'button' then o.border = bg == O and O or B end
    if mounted ~= page then
      o.kind,o.x,o.y,o.width,o.height = kind,x,y,w,h
      o.font,o.align,o.parent = font or N,align or 'left',parent
      o.line_height = font == M and 16 or font == T and 18 or font == F and 18 or font == 'button_12' and 16 or (font == 'body_14' or font == 'button_16') and 20 or 16
      if kind == 'button' then o.border_width = bg == 0 and 0 or 1 end
      if kind == 'viewport' then o.content_height = extra
      elseif kind == 'image' then o.asset = extra end
    end
    list[#list+1] = o
    return o
  end
  local function box(id,x,y,w,h,color) return obj(id,'box',x,y,w,h,nil,nil,nil,color) end
  local function label(id,x,y,w,h,text,font,fg,align)
    return obj(id,'label',x,y,w,h,text,font,fg,nil,align)
  end
  local function button(id,x,y,w,h,text,active,enabled,font,parent)
    return obj(id,'button',x,y,w,h,text,font or F,active and 0 or 0xffff,active and O or D,'center',enabled,parent)
  end
  local function scrollbar(y,h,total,offset)
    box('track',234,y,2,h,B)
    local size = math.max(6,math.floor(h*h/math.max(h,total)))
    local thumb = box('thumb',234,y+math.floor((h-size)*offset/math.max(1,total-h)),2,size,O)
    thumb.y,thumb.height = y+math.floor((h-size)*offset/math.max(1,total-h)),size
  end
  if page == 'monitor' then
    local c = current
    local status = op and 'WAIT' or fault ~= '' and 'ERR' or hold and 'PAUSED' or live and 'LIVE' or 'IDLE'
    label('title',4,0,76,18,'MONITOR',T,0xffff)
    local name = c and port(c)
    label('state',80,0,156,18,(c and c.source == 'uart' and ((name == 'CUSTOM' and 'UART' or name)..' '..c.baud) or 'SYSTEM LOG')..' / '..status,N,fault ~= '' and R or O,'right')
    obj('divider','image',4,18,232,2,nil,nil,nil,nil,nil,nil,nil,'monitor_divider')
    local console = box('console',4,20,232,176,D)
    console.border = B
    if mounted ~= page then console.border_width = 1 end
    box('rail',4,20,2,176,O)
    box('logs',8,20,224,176,D)
    local shown = hold and #frozen or count+(#tail > 0 and 1 or 0)
    scrollOffset = hold and math.min(scrollOffset,math.max(0,shown-11)*16) or 0
    local first = math.max(0,shown-11-math.floor(scrollOffset/16))
    for i=1,11 do
      local k = first+i
      local index = (head-count+k-1)%64+1
      local text = hold and frozen[k] or not hold and (k <= count and rows[index] or k == count+1 and #tail>0 and tail)
      local color = hold and frozenColors[k] or not hold and (k<=count and colors[index] or level)
      if shown == 0 then text = i == 1 and 'WAITING FOR DATA' or ' ' end
      label('log'..i,8,20+(i-1)*16,224,16,text or ' ',M,color or W)
    end
    scrollbar(20,176,math.max(176,shown*16),first*16)
    list[#list-1].visible,list[#list].visible = hold or false,hold or false
    button('pause',4,198,76,28,live and (hold and 'RESUME' or 'PAUSE') or 'RETRY',true,not op)
    button('clear',82,198,76,28,'CLEAR',false,ready)
    button('config',160,198,76,28,'CONFIG',false,not op)
  elseif page == 'config' then
    box('pill',8,14,112,18,0x2124)
    label('caption',8,15,112,18,'SERIAL CONFIG','mono_semibold_12',O,'center')
    local uart = draft.source == 'uart'
    local names = {'MODE','PORT','BAUD','FORMAT'}
    local ids = {'source','port','baud','format'}
    local values = {uart and 'RSPORT RX >' or 'SYSTEM >',uart and (port(draft)..' '..draft.rx..'/'..draft.tx..' >') or 'NOT USED',uart and (draft.baud..' >') or 'NOT USED',uart and (formats[draft.format][4]..' >') or 'NOT USED'}
    for i=1,4 do
      local enabled = not op and (i == 1 or uart)
      obj(ids[i],'button',8,32+(i-1)*32,224,31,' ',F,W,0,'left',enabled)
      label('key'..i,12,38+(i-1)*32,70,20,names[i],'body_14',enabled and A or 0x632c)
      local value = label('value'..i,84,38+(i-1)*32,144,20,values[i],'body_14',W,'right')
      value.foreground = enabled and W or 0x632c
      box('rule'..i,8,63+(i-1)*32,224,1,B)
    end
    button('cancel',8,172,72,44,'CANCEL',false,not op)
    local resetButton = button('reset',84,172,72,44,'RESET',false,not op)
    resetButton.background,resetButton.border = 0x2124,0x2124
    button('apply',160,172,72,44,op and 'WAIT' or 'APPLY',true,not op)
  else
    local captions = {port='RSPORT / RX ONLY',baud='COMMON BAUD',format='DATA / PARITY / STOP',source='CAPTURE SOURCE'}
    label('caption',8,11,224,18,captions[page],N,O)
    if page == 'port' then
      obj('board','image',76,42,88,88,nil,nil,nil,nil,nil,nil,nil,'rootmaker_a_face')
      for i,v in ipairs(ports) do
        button('p'..i,i%2 == 1 and 8 or 170,i < 3 and 32 or 88,62,50,
          'P'..i..(i == 2 and ' / USB' or '')..'\n'..v..' / '..(v+1),port(pick) == 'P'..i,i ~= 2,'button_12')
      end
      label('ttl',76,129,88,16,'3.3V TTL',N,A,'center')
      box('rule',8,142,224,1,B)
      label('rx',8,147,58,20,'RX '..pick.rx,'medium_14',O)
      label('tx',68,147,58,20,'TX '..pick.tx,'medium_14',W)
      button('swap',136,144,96,28,'SWAP',false,port(pick) ~= 'CUSTOM' and port(pick) ~= 'P2')
    elseif page == 'baud' then
      label('count',136,11,96,18,'11 PRESETS',N,A,'right')
      local region = obj('presets','viewport',8,30,224,138,nil,nil,nil,0,nil,nil,nil,284)
      region.scroll_y = baudOffset
      for i,v in ipairs(bauds) do
        button('b'..i,(i-1)%2*116,math.floor((i-1)/2)*48,108,44,tostring(v),pick.baud == v,true,F,'presets')
      end
      scrollbar(30,138,284,baudOffset)
    elseif page == 'format' then
      for i,v in ipairs(formats) do
        local x,y = 8+(i-1)%2*116,30+math.floor((i-1)/2)*48
        local color = pick.format == i and 0 or 0xffff
        button('f'..i,x,y,108,44,' ',pick.format == i,true)
        label('ft'..i,x+4,y+2,100,22,v[4],'button_16',color,'center')
        label('fd'..i,x+4,y+23,100,18,v[5],'button_12',color,'center')
      end
    else
      button('uart',8,34,224,44,'RSPORT RX',pick.source == 'uart')
      label('desc1',12,80,216,20,'External serial / RX only','body_14',A)
      button('system',8,106,224,44,'SYSTEM LOG',pick.source == 'system')
      label('desc2',12,152,216,20,'ESP diagnostic logs','body_14',A)
    end
    button('cancel',8,172,92,44,'CANCEL')
    button('use',104,172,128,44,'USE '..page:upper(),true,page ~= 'port' or port(pick) ~= 'P2')
  end
  local warning = gap and 'CAPTURE LOSS POSSIBLE' or overwritten and not hold and 'HISTORY OVERWRITTEN'
  local footer = fault ~= '' and fault or op and (op..' PENDING') or page ~= 'monitor' and ' ' or warning or hold and 'DRAG LOG / RESUME: LATEST' or 'PAUSE TO SCROLL HISTORY'
  local hint = label('footer',page == 'monitor' and 4 or 8,page == 'monitor' and 226 or 222,page == 'monitor' and 232 or 224,page == 'monitor' and 14 or 18,footer:sub(1,32),N,fault ~= '' and R or (op or hold or warning) and O or A,page == 'monitor' and 'center' or 'left')
  if mounted ~= page and page == 'monitor' then hint.line_height = 14 end
  if mounted ~= page then gen = ui.mount({id=page,background=0,objects=list}); mounted = page
  else ui.update(gen,list) end
  dirty = false
end
local function activate(id)
  if id == 'config' or id == 'reset' then
    draft,page = copy(current),'config'
    if not closing then fault = '' end
  elseif id == 'cancel' then
    page = page == 'config' and 'monitor' or 'config'
    if not closing then fault = '' end
  elseif id == 'use' then draft,page = pick,'config'
  elseif id == 'pause' then
    fault = ''
    if stream and not closing then
      hold = not hold
      if hold then freeze() else frozen,frozenColors,scrollOffset = {},{},0 end
    else begin(current) end
  elseif id == 'clear' then
    reset(); fault = ''
  elseif id == 'apply' then
    begin(draft)
  elseif page == 'config' then
    pick,page,baudOffset = copy(draft),id,0
    if id == 'baud' then
      for i,v in ipairs(bauds) do
        if v == pick.baud and i > 6 then baudOffset = math.floor((i-1)/2)*48+44-138 end
      end
    end
  elseif id == 'uart' or id == 'system' then pick.source = id
  elseif id == 'swap' then pick.rx,pick.tx = pick.tx,pick.rx
  elseif page == 'port' then
    local v = ports[tonumber(id:sub(2))]; pick.rx,pick.tx = v,v+1
  elseif page == 'baud' then pick.baud = bauds[tonumber(id:sub(2))]
  elseif page == 'format' then pick.format = tonumber(id:sub(2)) end
end
begin(current); paint()
-- Only the C-owned BOOT cancellation path exits this application.
while true do
  advance(); poll()
  local event = ui.poll(page == 'monitor' and hold and 'logs' or page == 'baud' and 'presets' or nil)
  if event then
    if event.kind == 'scroll' then
      if page == 'baud' then baudOffset = math.max(0,math.min(146,baudOffset-event.dy))
      else scrollOffset = math.max(0,scrollOffset+event.dy) end
    else activate(event.id) end
    paint()
  end
  board.sleep_ms(20); ticks = ticks+1
  if ticks == 5 then ticks = 0; if dirty then paint() end end
end
