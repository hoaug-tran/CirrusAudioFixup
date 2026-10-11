#!/usr/bin/env bash
# Read-only evidence collector; no bus probing, register reads, or driver changes.
set -u
if [ "$(id -u)" -ne 0 ]; then echo 'Run: sudo bash collect_linux.sh'; exit 1; fi
command -v python3 >/dev/null || { echo 'Install python3 first'; exit 1; }
python3 - <<'PY'
import os, pathlib, subprocess, datetime, shutil, hashlib, tarfile
P=pathlib.Path
out=P.cwd()/('HP-CS35L41-Linux-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
out.mkdir(exist_ok=False)
status=[]
def command(name,args):
    target=out/name; target.parent.mkdir(parents=True,exist_ok=True)
    try:
        r=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
        target.write_bytes(r.stdout); status.append(f'{name}: exit {r.returncode}')
    except Exception as e: target.write_text(str(e)); status.append(f'{name}: {e}')
def copy(src,dst):
    try:
        dst.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(src,dst)
        status.append(f'{src}: copied')
    except Exception as e: status.append(f'{src}: {e}')
for name,args in [
 ('uname.txt',['uname','-a']),('pci.txt',['lspci','-Dnnk']),('pci-detail.txt',['lspci','-Dvvnn']),
 ('dmesg.txt',['dmesg']),('journal-kernel.txt',['journalctl','-b','-k','--no-pager']),
 ('modules.txt',['lsmod']),('alsa-devices.txt',['aplay','-l']),('alsa-pcms.txt',['aplay','-L']),
 ('packages.txt',['dpkg-query','-W','linux-firmware','acpica-tools','alsa-utils']),
 ('dmi.txt',['dmidecode','-t','0','-t','2'])]: command(name,args)
# Full DMI output can contain identifiers; retain only model/BIOS fields here.
f=out/'dmi.txt'
if f.exists(): f.write_text('\n'.join(l for l in f.read_text(errors='replace').splitlines() if not any(x in l.lower() for x in ['serial number','asset tag','uuid'])))
for key in ['sys_vendor','product_name','product_version','board_vendor','board_name','board_version','bios_vendor','bios_version','bios_date']:
    copy(P('/sys/class/dmi/id')/key,out/'identity'/key)
for src in ['/etc/os-release','/proc/cmdline','/proc/interrupts','/proc/iomem','/proc/asound/cards','/proc/asound/pcm','/proc/asound/devices']:
    copy(P(src),out/'system'/src.lstrip('/'))
for module in ['spi_pxa2xx_platform','intel_lpss_pci','snd_hda_scodec_cs35l41','snd_hda_scodec_cs35l41_spi','snd_hda_scodec_cs35l41_i2c','snd_hda_intel','snd_sof_pci_intel_tgl']:
    command('modinfo/'+module+'.txt',['modinfo',module])
for card in P('/proc/asound').glob('card[0-9]*'):
    if not card.name[4:].isdigit(): continue
    for src in card.glob('codec#*'): copy(src,out/'alsa'/card.name/src.name)
    command('alsa/'+card.name+'-controls.txt',['amixer','-c',card.name[4:],'contents'])
    command('alsa/'+card.name+'-state-log.txt',['alsactl','-f',str(out/'alsa'/(card.name+'-state.conf')),'store',card.name[4:]])
# ACPI binaries preserve _CRS/_DSD and cross-table references. Exclude Windows license table.
for base in [P('/sys/firmware/acpi/tables'),P('/sys/firmware/acpi/tables/dynamic')]:
    if not base.exists(): continue
    for src in base.iterdir():
        if src.is_file() and src.name != 'MSDM': copy(src,out/'acpi'/('dynamic' if base.name=='dynamic' else 'raw')/(src.name+'.dat'))
raw=out/'acpi/raw'
if raw.exists() and shutil.which('iasl'):
    dst=out/'acpi/disassembled'; shutil.copytree(raw,dst)
    aml=[str(p) for p in dst.iterdir() if p.name.startswith(('DSDT','SSDT'))]
    if aml: command('acpi/iasl.log',['iasl','-da','-dl',*aml])
# Bound topology, controller resources, IRQ and runtime power state; no MMIO access.
for bus in ['spi','acpi','pci','platform']:
    base=P('/sys/bus')/bus/'devices'
    if not base.exists(): continue
    for dev in base.iterdir():
        interesting=bus=='spi' or 'CSC3551' in dev.name.upper() or 'cs35l41' in dev.name.lower()
        if bus=='pci':
            try: interesting=dev.joinpath('vendor').read_text().strip()=='0x8086' and dev.joinpath('device').read_text().strip()=='0x7aaa'
            except OSError: pass
        if not interesting: continue
        dst=out/'sysfs'/bus/dev.name; dst.mkdir(parents=True,exist_ok=True)
        links={}
        for key in ['driver','firmware_node','physical_node','subsystem','of_node']:
            node=dev/key
            if node.exists(): links[key]=str(node.resolve())
        (dst/'paths.txt').write_text('device='+str(dev.resolve())+'\n'+'\n'.join(k+'='+v for k,v in links.items()))
        for key in ['modalias','uevent','path','hid','uid','status','adr','vendor','device','subsystem_vendor','subsystem_device','irq','resource','driver_override','power/control','power/runtime_status','power/runtime_active_time','power/runtime_suspended_time']:
            node=dev/key
            if node.is_file(): copy(node,dst/key)
fw=P('/lib/firmware/cirrus')
if fw.exists():
    files=sorted(p for p in fw.iterdir() if 'cs35l41' in p.name.lower())
    (out/'firmware').mkdir(exist_ok=True)
    (out/'firmware/inventory.txt').write_text('\n'.join(str(p)+' -> '+str(p.resolve()) for p in files))
    for src in files:
        n=src.name.lower()
        if any(x in n for x in ['103c89c6','103c8ca4']) or n in ['cs35l41-dsp1-spk-prot.wmfw','cs35l41-dsp1-spk-prot.wmfw.zst','cs35l41-dsp1-spk-prot.wmfw.xz','cs35l41-dsp1-spk-prot.bin','cs35l41-dsp1-spk-prot.bin.zst','cs35l41-dsp1-spk-prot.bin.xz']:
            copy(src,out/'firmware'/src.name)
for src in P('/sys/firmware/efi/efivars').glob('CirrusSmartAmpCalibrationData-*'):
    copy(src,out/'calibration'/src.name)
# GPIO ownership only, when debugfs was already mounted. Never dump live regmap registers.
src=P('/sys/kernel/debug/gpio')
if src.is_file(): copy(src,out/'gpio.txt')
(out/'collection-status.txt').write_text('\n'.join(status))
(out/'README.txt').write_text('Read-only Ubuntu hardware evidence. Missing tools/files are recorded in collection-status.txt. No SPI transactions or register dumps were performed. ACPI MSDM and DMI serial/UUID fields omitted; logs may still contain identifying data. Record separately whether internal speakers work under this Ubuntu boot, and whether this capture was before/after playback. Host validation does not prove Linux runtime correctness.\n')
manifest=[]
for p in sorted(out.rglob('*')):
    if p.is_file(): manifest.append(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(out)))
(out/'SHA256SUMS').write_text('\n'.join(manifest)+'\n')
archive=P(str(out)+'.tar.gz')
with tarfile.open(archive,'w:gz') as tar: tar.add(out,arcname=out.name)
uid=int(os.environ.get('SUDO_UID','0')); gid=int(os.environ.get('SUDO_GID','0'))
for p in [*out.rglob('*'),out,archive]: os.chown(p,uid,gid)
print('Send this archive:',archive)
PY
