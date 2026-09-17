local display = require("display")
display.clear(0x0000)
display.text(10, 20, "HI", 0xFFFF, 2)
-- Independent glyph expectations: H has two stems and a middle bar.
assert(display.read_pixel(10, 20) == 0xFFFF)
assert(display.read_pixel(12, 20) == 0x0000)
assert(display.read_pixel(18, 20) == 0xFFFF)
assert(display.read_pixel(14, 26) == 0xFFFF)
assert(display.read_pixel(20, 20) == 0x0000)
-- I begins after the 6-column character advance, with an inset top bar.
assert(display.read_pixel(22, 20) == 0x0000)
assert(display.read_pixel(24, 20) == 0xFFFF)
assert(display.show())
print("DISPLAY_TEXT_PASS", "expected screen: white HI on black")
