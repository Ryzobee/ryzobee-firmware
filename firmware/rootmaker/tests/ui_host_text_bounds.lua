-- ryz-app/1
local ui = require("ui")

ui.mount({
  id = "text_bounds",
  background = 0x0000,
  objects = {
    { id = "long_right", kind = "label", x = 10, y = 10, width = 12, height = 7,
      text = "TOO LONG", foreground = 0xFB40, font = "body_16", align = "right" },
    { id = "short_height", kind = "label", x = 30, y = 30, width = 40, height = 7,
      text = "HI", foreground = 0xFFFF, font = "title_24", align = "left" },
    { id = "too_short", kind = "button", x = 30, y = 50, width = 40, height = 6,
      text = "HIDE", foreground = 0xFFFF, background = 0x0000,
      border = 0xFB40, border_width = 1, font = "body_16" },
  },
})
