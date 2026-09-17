-- ryz-app/1
-- @author: RyzoBee
-- @version: 0.1.0
-- @description: Managed LVGL scene and button interaction example.
-- Managed LVGL scene demo. Run it as an async Workbench task, then tap a card.
local board = require("ryzobee")
local ui = require("ui")

local BLACK = 0x0000
local SURFACE = 0x1082
local ORANGE = 0xFB40
local WHITE = 0xFFFF

ui.mount({
  id = "home",
  background = BLACK,
  objects = {
    { id = "rail", kind = "box", x = 8, y = 8, width = 224, height = 224,
      background = BLACK, border = ORANGE, border_width = 1, radius = 4 },
    { id = "brand", kind = "label", x = 16, y = 18, width = 208, height = 30,
      text = "RYZOBEE", foreground = ORANGE, font = "title_24", align = "center" },
    { id = "status", kind = "label", x = 16, y = 49, width = 208, height = 22,
      text = "LVGL UI READY", foreground = WHITE, font = "body_16", align = "center" },
    { id = "apps", kind = "button", x = 20, y = 82, width = 200, height = 38,
      text = "APPS", foreground = ORANGE, background = SURFACE,
      border = ORANGE, border_width = 2, radius = 3, font = "body_16" },
    { id = "settings", kind = "button", x = 20, y = 128, width = 200, height = 38,
      text = "SETTINGS", foreground = WHITE, background = SURFACE,
      border = ORANGE, border_width = 1, radius = 3, font = "body_16" },
    { id = "about", kind = "button", x = 20, y = 174, width = 200, height = 38,
      text = "ABOUT", foreground = WHITE, background = SURFACE,
      border = ORANGE, border_width = 1, radius = 3, font = "body_16" },
  },
})

while true do
  local event = ui.poll()
  if event then
    board.mark("action", event.id)
    print("UI_ACTIVATE", event.id, event.generation)
    break
  end
  board.sleep_ms(20)
end
