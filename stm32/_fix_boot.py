import re

p = r'C:\Users\26895\Desktop\OTA\stm32\bootloader\Projects\MDK-ARM\bootloader.uvprojx'
with open(p, 'r', encoding='utf-8') as f:
    c = f.read()

# Replace any garbled IncludePath with the correct one
good_inc = '..\\..\\User;..\\..\\..\\Drivers\\CMSIS\\Device\\ST\\STM32F1xx\\Include;..\\..\\..\\Drivers\\CMSIS\\Include;..\\..\\..\\Drivers'

# Find all IncludePath tags and fix them
# Pattern: <IncludePath>anything</IncludePath>
new_c = ''
pos = 0
while True:
    start = c.find('<IncludePath>', pos)
    if start == -1:
        new_c += c[pos:]
        break
    end = c.find('</IncludePath>', start) + len('</IncludePath>')
    new_c += c[pos:start]
    new_c += '<IncludePath>' + good_inc + '</IncludePath>'
    pos = end

with open(p, 'w', encoding='utf-8') as f:
    f.write(new_c)
print('Bootloader include paths fixed')
