-- ryz-app/1
-- @author: Unknown
-- @version: 0.1.0
-- @description: Persistent file counter using the public fs module. Saves in this script's own namespace; reports errors instead of resetting corrupt data.

local fs = require('fs')
local value, reason = fs.read('counter.txt')
local count = 0
if value == nil then
  if reason ~= 'not_found' then error('Cannot load counter: '..reason) end
else
  if not value:match('^%d+$') then error('Invalid counter; not overwriting it') end
  count = tonumber(value)
  if not count or math.type(count) ~= 'integer' or count < 0 or count >= 1000000 then
    error('Counter outside demo range; not overwriting it')
  end
end

local next_value = tostring(count + 1)
local ok, why = fs.write('counter.txt', next_value)
if not ok then error('Save not confirmed: '..why..'; read before retrying') end
local saved, read_error = fs.read('counter.txt')
if saved ~= next_value then error('Read-back failed: '..(read_error or 'mismatch')) end
print('Persistent counter: '..saved)

-- Generic files can hold binary bytes, including NUL, or be completely empty.
assert(fs.write('scratch.bin', string.char(0, 255, 65)))
assert(fs.read('scratch.bin') == string.char(0, 255, 65))
assert(fs.write('scratch.bin', ''))
assert(fs.read('scratch.bin') == '')
assert(fs.remove('scratch.bin'))

local files, list_error = fs.list()
if not files then error('List failed: '..list_error) end
for _, file in ipairs(files) do print(file.name..': '..file.size..' bytes') end
local info, info_error = fs.info()
if not info then error('Info failed: '..info_error) end
print('App data: '..info.used_bytes..' / '..info.quota_bytes..' bytes')
print('Run this same file again to increment the saved counter.')
