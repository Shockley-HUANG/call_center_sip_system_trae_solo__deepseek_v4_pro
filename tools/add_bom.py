import sys, os
md_files = ['README.md', 'tasks.md', 'ISSUES_LOG.md']
for f in md_files:
    path = os.path.join('/mnt/d/Trae_Project/call_center_sip_system', f)
    data = open(path, 'rb').read()
    if data[:3] != b'\xef\xbb\xbf':
        open(path, 'wb').write(b'\xef\xbb\xbf' + data)
        print(f'{f}: BOM added')
    else:
        print(f'{f}: BOM OK')
    # verify
    v = open(path, 'rb').read(3)
    print(f'  verify: {list(v)}')
