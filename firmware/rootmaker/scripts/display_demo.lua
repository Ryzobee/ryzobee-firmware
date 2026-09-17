-- Color is RGB565. Draw into the canvas, then flush one complete frame.
-- @author: RyzoBee
-- @version: 0.1.0
-- @description: RGB565 canvas, text and color bar example.
local display = require("display")
display.clear(0x0841)
display.rect(0, 0, 240, 5, 0x07FF)
display.text(39, 22, "RYZOBEE", 0xFFFF, 4)
display.text(67, 65, "LUA READY", 0x07FF, 2)

display.rect(15, 105, 60, 52, 0xF800)
display.rect(90, 105, 60, 52, 0x07E0)
display.rect(165, 105, 60, 52, 0x001F)
display.text(39, 171, "R", 0xFFFF, 2)
display.text(114, 171, "G", 0xFFFF, 2)
display.text(189, 171, "B", 0xFFFF, 2)
display.text(19, 207, "ESP32-S3 / ST7789", 0xFFFF, 2)
assert(display.show())
print("DISPLAY_DEMO_READY", "RYZOBEE / LUA READY / red-green-blue blocks")
