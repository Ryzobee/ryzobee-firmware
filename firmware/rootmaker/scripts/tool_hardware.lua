-- ryz-app/1
-- @author: RyzoBee
-- @version: 1.0.0
-- @description: Public Lua self test: pixels, touch, motion, RGB and battery status.
local ui,board = require('ui'),require('ryzobee')
local imu=require('imu')
local led=require('led')
local O,B,D,W,A,G,R,Y = 0xfb40,0x39c7,0x2124,0xdedb,0xa514,0x2e6b,0xfa27,0xfd84
local names = {'LCD / PIXELS','TOUCH / GRID','IMU / MOTION','RGB LED','BATTERY'}
local short = {'LCD','TOUCH','IMU','RGB LED','BATTERY'}
local results = {'WAIT','WAIT','WAIT','WAIT','WAIT'}
local page,mounted,gen = 'hub','',nil
local selected,fault,dirty = 0,'',true
local touched,touchCount = {},0
local imuOwned,imuPhase,sample,lastSample,lastPoll,sequence,stamp,ending
local low,high,samples,motion = {},{},0,false
local pixel,rgbPhase,rgbStage,ledClosing,ledTime,ledPoll,ledTarget
local rgb={{255,0,0,'RED',0xf800},{0,255,0,'GREEN',0x07e0},{0,0,255,'BLUE',0x001f}}
local sendLed,closeLed
local function complete()
  local n=0
  for _,r in ipairs(results) do if r~='WAIT' then n=n+1 end end
  return n
end
local function color(r)
  return r=='PASS' and G or r=='FAIL' and R or r=='CHECK' and Y or A
end
local function finish(result)
  ending=result
  if imuOwned then
    local ok,reason=imu.deinit()
    if not ok then sample,imuPhase,fault=nil,'closing','STOP: '..reason:upper(); return end
    imuOwned=false
  end
  if pixel then
    if ledClosing then closeLed() else sendLed(true) end
    return
  end
  if result~='MENU' then results[selected]=result end
  page,fault=complete()==5 and 'summary' or 'hub',''
end
sendLed=function(black)
  ledTarget=black and {0,0,0} or rgb[rgbStage]
  local ok,reason=pixel:write(ledTarget[1],ledTarget[2],ledTarget[3])
  rgbPhase,fault=ok and 'pending' or 'error',ok and '' or 'WRITE: '..reason:upper()
  ledTime,ledPoll=board.millis(),board.millis()
end
closeLed=function()
  ledClosing=true
  local ok,reason=pixel:close()
  if not ok then rgbPhase,fault='closing','CLOSE: '..reason:upper(); return end
  pixel,ledClosing=nil,false
  finish(ending)
end
local function startLed()
  if ledClosing then closeLed(); return end
  if not pixel then
    local opened,reason=led.open{board=true}
    if not opened then rgbPhase,fault='error','OPEN: '..reason:upper(); return end
    pixel=opened
  end
  sendLed(ending~=nil)
end
local function pollLed()
  if page~='rgb' or rgbPhase~='pending' then return end
  local now=board.millis()
  if now-ledPoll<40 then return end
  ledPoll=now
  local state,reason=pixel:status()
  if not state then rgbPhase,fault='error','STATUS: '..reason:upper()
  elseif state.state=='failed' then rgbPhase,fault='error','LED: FRAME FAILED'
  elseif state.state=='ready' then
    if not state.output_known or state.red~=ledTarget[1] or state.green~=ledTarget[2] or state.blue~=ledTarget[3] then
      rgbPhase,fault='error','LED: FRAME MISMATCH'
    elseif ending then closeLed()
    else rgbPhase='ready' end
  elseif now-ledTime>=1000 then rgbPhase,fault='error','LED: FRAME TIMEOUT' end
  dirty=true
end
local function startImu()
  sample,low,high,samples,motion,sequence,stamp=nil,{},{},0,false,nil,nil
  if imuOwned then
    local ok,reason=imu.deinit()
    if not ok then imuPhase,fault='error','STOP: '..reason:upper(); return end
    imuOwned=false
  end
  local ok,reason=imu.init()
  imuOwned,imuPhase=ok and true or false,ok and 'read' or 'error'
  fault=ok and '' or 'INIT: '..reason:upper()
  lastSample,lastPoll=board.millis(),board.millis()
end
local function pollImu()
  if page~='imu' or imuPhase~='read' then return end
  local now=board.millis()
  if now-lastPoll<100 then return end
  lastPoll=now
  local value,reason=imu.read()
  if value and value.sequence~=sequence and value.timestamp_us~=stamp then
    sequence,stamp,lastSample,sample=value.sequence,value.timestamp_us,now,value
    local axes={value.x_mg,value.y_mg,value.z_mg}
    samples=samples+1
    for i,v in ipairs(axes) do low[i]=math.min(low[i] or v,v); high[i]=math.max(high[i] or v,v) end
    motion=samples>=6 and high[1]-low[1]>=250 and high[2]-low[2]>=250 and high[3]-low[3]>=250
    fault=''
  else
    sample=nil
    if reason and reason~='not_ready' then imuPhase,fault='error','READ: '..reason:upper()
    elseif now-lastSample>=1000 then imuPhase,fault='error','IMU: NO FRESH SAMPLE' end
  end
  dirty=true
end
local function enter(i)
  selected,results[i],fault=i,'WAIT',''
  page=({'lcd','touch','imu','rgb','battery'})[i]
  touched,touchCount={},0
  ending=nil
  if page=='rgb' then rgbPhase,rgbStage,ledClosing='idle',1,false end
  if page=='imu' then startImu() end
end
local function paint()
  local objects={}
  local function obj(id,kind,x,y,w,h,text,font,fg,bg,align,enabled)
    local o={id=id,text=text,foreground=fg or W,background=bg or 0,enabled=enabled~=false}
    if mounted~=page then
      o.kind,o.x,o.y,o.width,o.height=kind,x,y,w,h
      o.font,o.align=font or 'body_12',align or 'left'
      o.line_height=(font=='mono_14' or font=='medium_14' or font=='button_14') and 20 or 16
      o.border_width=kind=='button' and 1 or 0
    end
    if kind=='button' then o.border=bg==O and O or B end
    objects[#objects+1]=o
    return o
  end
  local function box(id,x,y,w,h,bg) return obj(id,'box',x,y,w,h,nil,nil,nil,bg) end
  local function label(id,x,y,w,h,text,font,fg,align)
    return obj(id,'label',x,y,w,h,text,font,fg,nil,align)
  end
  local function button(id,x,y,w,h,text,active,enabled)
    return obj(id,'button',x,y,w,h,text,'button_14',active and 0 or W,active and O or D,'center',enabled)
  end
  if page=='hub' then
    label('title',8,12,160,18,'ROOTMAKER SELF TEST','mono_semibold_12',O)
    box('pill',184,13,48,16,D)
    label('count',184,13,48,16,complete()..' / 5','mono_semibold_12',O,'center')
    local first=true
    for i,name in ipairs(names) do
      local active=first and results[i]=='WAIT'
      if active then first=false end
      local row=button('h'..i,8,34+(i-1)*32,224,32,' ',false)
      row.background=active and D or 0; row.border=0
      label('name'..i,16,42+(i-1)*32,134,20,name,'medium_14',W)
      label('result'..i,150,42+(i-1)*32,74,20,active and 'START' or results[i],'button_14',active and O or color(results[i]),'right')
      box('rule'..i,8,65+(i-1)*32,224,1,B)
      if active then box('rail',8,34+(i-1)*32,3,32,O) end
    end
  elseif page=='summary' then
    box('pill',8,14,144,16,D)
    label('title',8,14,144,16,'SELF TEST COMPLETE','mono_semibold_12',G,'center')
    for i,name in ipairs(short) do
      label('name'..i,16,42+(i-1)*24,120,20,name,'medium_14',W)
      label('result'..i,148,42+(i-1)*24,76,20,results[i],'mono_semibold_12',color(results[i]),'right')
      box('rule'..i,8,61+(i-1)*24,224,1,B)
    end
    button('again',8,164,100,28,'RUN AGAIN')
    button('menu',112,164,120,28,'TEST MENU',true)
  else
    local captions={lcd='LCD / PIXELS',touch='TOUCH / GRID',battery='BATTERY / CHECK',imu='LIS2DW12 / LIVE',rgb='RGB LED / OPTICAL CHECK'}
    label('title',8,12,224,18,captions[page] or 'TEST','mono_semibold_12',O)
    if page=='lcd' then
      for i,c in ipairs({0xf800,0x07e0,0x001f,0xffff}) do box('color'..i,8+(i-1)*56,38,56,52,c) end
      box('frame',8,98,224,42,W)
      for i=0,13 do box('pixel'..i,9+i*16,99,8,40,0) end
      label('hint',8,146,224,20,'CHECK COLORS AND PIXELS','body_12',A,'center')
    elseif page=='touch' then
      label('coverage',170,12,62,18,touchCount..' / 9','mono_semibold_12',O,'right')
      for i=1,9 do
        local cell=button('g'..i,8+(i-1)%3*76,34+math.floor((i-1)/3)*44,72,40,touched[i] and 'OK' or tostring(i),touched[i])
        if touched[i] then cell.background,cell.border=G,G end
      end
    elseif page=='imu' then
      box('pill',176,13,56,16,D)
      label('state',176,13,56,16,sample and motion and 'MOTION' or 'WAIT','mono_semibold_12',sample and motion and G or A,'center')
      local axes=sample and {sample.x_mg,sample.y_mg,sample.z_mg} or {}
      for i,name in ipairs({'X','Y','Z'}) do
        local y=42+(i-1)*38
        label('axis'..i,12,y,20,26,name,'button_14',W)
        box('track'..i,36,y+9,128,8,D)
        local width=sample and math.max(1,math.min(128,math.floor(64+axes[i]*64/2000))) or 1
        local bar=box('bar'..i,36,y+9,width,8,i==3 and G or O)
        bar.width,bar.visible=width,sample~=nil
        label('value'..i,168,y,64,26,sample and string.format('%+.2f g',axes[i]/1000) or '--.-- g','mono_14',W,'right')
      end
      label('hint',24,150,192,20,'Tilt each axis by at least 0.25 g.','medium_12',A,'center')
    elseif page=='rgb' then
      box('swatch',8,38,224,70,rgbPhase=='idle' and D or ending and 0 or rgb[rgbStage][5])
      label('color',8,112,224,24,ending and 'LED OFF / RELEASE' or rgb[rgbStage][4]..' / '..rgbStage..' OF 3','button_14',O,'center')
      local hint=rgbPhase=='idle' and 'PRESS START TO DRIVE BOARD LED' or rgbPhase=='ready' and 'DOES THE LED MATCH THIS COLOR?' or rgbPhase=='pending' and 'WAITING FOR FRAME COMPLETION' or 'RETRY OR RECORD A FAILURE'
      label('hint',8,144,224,20,hint,'body_12',A,'center')
    elseif page=='battery' then
      label('notice',8,54,224,28,'NOT AVAILABLE','button_14',Y,'center')
      label('reason',12,92,216,20,'NO VERIFIED BATTERY API','mono_12',A,'center')
      label('boundary',12,116,216,20,'NO CHARGE LEVEL CLAIMED','mono_12',A,'center')
      label('hint',8,146,224,20,'RECORD CHECK OR SKIP','body_12',A,'center')
    end
    local primary=page=='battery' and 'CHECK' or page=='imu' and (imuPhase=='closing' and 'RETRY CLOSE' or imuPhase=='error' and 'RETRY' or 'MOTION OK') or 'CONFIRM PASS'
    if page=='rgb' then primary=rgbPhase=='idle' and 'START' or rgbPhase=='ready' and 'MATCH' or rgbPhase=='pending' and 'WAIT' or ledClosing and 'RETRY CLOSE' or 'RETRY' end
    local allowed=page=='touch' and touchCount==9 or page=='imu' and (imuPhase~='read' or sample~=nil and motion) or page~='touch' and page~='imu'
    if page=='rgb' then allowed=rgbPhase~='pending' end
    button('primary',52,170,136,28,primary,true,allowed)
    local leaving=page=='rgb' and ending~=nil or page=='imu' and imuPhase=='closing'
    button('fail',8,202,72,20,'FAIL',false,page~='battery' and not leaving)
    button('skip',84,202,72,20,'SKIP',false,not leaving)
    button('menu',160,202,72,20,'MENU',false,not leaving)
  end
  label('footer',8,222,224,18,fault~='' and fault or ' ','body_12',fault~='' and R or A)
  if mounted~=page then gen=ui.mount{id=page,background=0,objects=objects}; mounted=page
  else ui.update(gen,objects) end
  dirty=false
end
paint()
while true do
  local event=ui.poll()
  if event then
    local id=event.id
    if page=='hub' then enter(tonumber(id:sub(2)))
    elseif id=='again' then results={'WAIT','WAIT','WAIT','WAIT','WAIT'}; page='hub'
    elseif page=='summary' then page='hub'
    elseif id=='menu' then finish('MENU')
    elseif id=='fail' then finish('FAIL')
    elseif id=='skip' then finish('SKIP')
    elseif id=='primary' then
      if page=='imu' and imuPhase=='closing' then finish(ending)
      elseif page=='imu' and imuPhase=='error' then startImu()
      elseif page=='rgb' then
        if rgbPhase=='ready' then
          if rgbStage==3 then finish('PASS') else rgbStage=rgbStage+1; sendLed(false) end
        else startLed() end
      else finish(page=='battery' and 'CHECK' or 'PASS') end
    elseif page=='touch' then
      local i=tonumber(id:sub(2)); if not touched[i] then touched[i]=true; touchCount=touchCount+1 end
    end
    paint()
  end
  pollImu()
  pollLed()
  if dirty then paint() end
  board.sleep_ms(20)
end
