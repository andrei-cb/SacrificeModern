"""Development-only PE and unchanged-installation checks. Requires pefile."""
from pathlib import Path
import hashlib
import json
import os
import sys
import pefile

source=Path(__file__).resolve().parents[1]
root=source.parent
package=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else source/'out/package'
forbidden={p.name.lower() for p in root.glob('*.dll')}
forbidden.update({'sacrifice.exe','scapex.exe','unins000.exe'})
files={p.name.lower():p for p in package.iterdir() if p.suffix.lower() in {'.exe','.dll'}}
imports={}
for name,path in files.items():
    assert name not in forbidden,f'Original binary in package: {name}'
    pe=pefile.PE(str(path))
    assert pe.FILE_HEADER.Machine==0x8664,f'Not x64: {name}'
    dlls=[entry.dll.decode('ascii').lower() for entry in getattr(pe,'DIRECTORY_ENTRY_IMPORT',[])]
    assert not forbidden.intersection(dlls),f'Original dependency: {name}'
    for dll in dlls:
        if dll.startswith(('msvcp','vcruntime','concrt')):
            assert dll in files,f'Missing packaged C++ runtime {dll}'
        elif not dll.startswith(('api-ms-','ext-ms-')):
            assert dll in files or (Path(os.environ['WINDIR'])/'System32'/dll).exists(),f'Missing dependency {dll}'
    imports[name]=dlls
binary_inventory=json.loads((root/'analysis/binary_inventory.json').read_text())
for item in binary_inventory:
    actual=hashlib.sha256((root/item['file']).read_bytes()).hexdigest()
    assert actual==item['sha256'],f"Original binary changed: {item['file']}"
inventory=json.loads((root/'analysis/file_inventory.json').read_text())
runtime_logs=[]
immutable_files=0
for item in inventory:
    p=root/item['file']
    # These are runtime output, not installation inputs. Keep their drift visible without requiring
    # users to restore or truncate their logs to validate a reconstruction package.
    if item['file'].lower() in {'error.err','thaum.err'}:
        runtime_logs.append({'file':item['file'], 'inventory_size':item['size'],
                             'current_size':p.stat().st_size if p.exists() else None,
                             'header_matches_inventory':p.read_bytes()[:64].hex()==item['header_hex'] if p.exists() else False})
        continue
    assert p.stat().st_size==item['size'],f'Installed file size changed: {p}'
    with p.open('rb') as f:
        assert f.read(64).hex()==item['header_hex'],f'Installed header changed: {p}'
    immutable_files+=1
report={'package':str(package),'x64_binaries':len(files),'imports':imports,
        'original_binary_hashes_unchanged':len(binary_inventory),
        'original_immutable_files_sizes_headers_unchanged':immutable_files,
        'runtime_logs':runtime_logs}
(source/'out/package-verification.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print(f"Verified {len(files)} x64 package binaries, no original game DLL imports, {len(binary_inventory)} original binary hashes and {immutable_files} unchanged installation inputs")
for item in runtime_logs:
    print(f"Runtime log {item['file']}: inventory size {item['inventory_size']}, current size {item['current_size']}, header matches: {item['header_matches_inventory']}")
