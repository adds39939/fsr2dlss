import pefile
p = r"E:\Games\Steam\steamapps\common\Lies of P\LiesofP\Binaries\Win64\LOP-Win64-Shipping.exe"
pe = pefile.PE(p, fast_load=True)
pe.parse_data_directories(directories=[
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT'],
])
def hit(name): return 'fidelityfx' in name.lower()
print("== static imports referencing fidelityfx ==")
found=False
for d in getattr(pe,'DIRECTORY_ENTRY_IMPORT',[]):
    if hit(d.dll.decode()):
        found=True; print(" ", d.dll.decode(), "->", [i.name.decode() for i in d.imports if i.name][:8])
print("  (none)" if not found else "")
print("== delay imports referencing fidelityfx ==")
found=False
for d in getattr(pe,'DIRECTORY_ENTRY_DELAY_IMPORT',[]):
    nm = d.dll.decode()
    if hit(nm):
        found=True; print(" ", nm, "->", [i.name.decode() for i in d.imports if i.name][:8])
print("  (none)" if not found else "")
