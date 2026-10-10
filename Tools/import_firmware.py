import sys
import os
import struct
import argparse
import hashlib
import json
import re
from pathlib import Path

def validate_wmfw(data):
    if len(data) < 40:
        return False, "File too small (< 40 bytes)"
    if data[:4] != b'WMFW':
        return False, "Invalid magic, expected 'WMFW'"
    if len(data) % 4 != 0:
        return False, f"Not 4-byte aligned (length: {len(data)})"
    header_len, rev, core, ver = struct.unpack_from('<IHBB', data, 4)
    if ver < 2:
        return False, f"Unsupported wmfw version: {ver} (expected >= 2)"
    return True, f"Valid WMFW: rev=0x{rev:04X}, core=0x{core:02X}, ver={ver}, size={len(data)} bytes"

def validate_bin(data):
    if len(data) < 16:
        return False, "File too small (< 16 bytes)"
    if data[:4] != b'WMDR':
        return False, "Invalid magic, expected 'WMDR'"
    if len(data) % 4 != 0:
        return False, f"Not 4-byte aligned (length: {len(data)})"
    return True, f"Valid WMDR coefficient file: size={len(data)} bytes"

def format_c_array(name, data):
    lines = [f"// SHA256: {hashlib.sha256(data).hexdigest()}", f"const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        formatted = ", ".join(f"0x{b:02x}" for b in chunk)
        if i + 12 < len(data):
            lines.append(f"    {formatted},")
        else:
            lines.append(f"    {formatted}")
    lines.append("};")
    return "\n".join(lines)

def detect_codec_from_path(file_path):
    lower = str(file_path).lower()
    for candidate in ["cs35l41", "cs35l45", "cs35l51", "cs35l53", "cs35l56"]:
        if candidate in lower:
            return candidate
    return None

def validate_bincfg(data):
    # Mirror the runtime parser. Validate all records before producing C++ so
    # a malformed later entry cannot leave a partially imported gain policy.
    if len(data) < 16 or len(data) > 65536 or len(data) % 4:
        return False, "Invalid tuning size/alignment"
    signature, version, size, count = struct.unpack_from('<4I', data)
    if signature != 0x109A4A35 or version != 1 or size != len(data):
        return False, "Invalid tuning signature/version/declared size"
    if count > (size - 16) // 12:
        return False, "Invalid tuning entry count"
    offset, gain = 16, None
    for _ in range(count):
        if size - offset < 12:
            return False, "Truncated tuning record"
        _, kind, length = struct.unpack_from('<3I', data, offset)
        if length < 12 or length % 4 or length > size - offset:
            return False, "Invalid tuning record length"
        if kind == 0:
            if length != 16 or gain is not None:
                return False, "Invalid or duplicate gain record"
            gain = struct.unpack_from('<I', data, offset + 12)[0]
            if gain > 20:
                return False, "PCM gain outside CS35L41 range 0..20"
        offset += length
    if offset != size:
        return False, "Unclaimed tuning bytes"
    return True, f"Valid BINCFG: PCM gain code {17 if gain is None else gain}"


def import_firmware(ssid_str, wmfw_path, bin_l_path, bin_r_path, spkid=1, codec=None, firmware_path=None,
                    bincfg_l_path=None, bincfg_r_path=None):
    if ssid_str.lower().startswith('0x'):
        ssid_int = int(ssid_str, 16)
    else:
        ssid_int = int(ssid_str, 16)

    sub_vendor = (ssid_int >> 16) & 0xFFFF
    sub_device = ssid_int & 0xFFFF
    ssid_hex = f"{ssid_int:08x}"
    if not 0 <= ssid_int <= 0xFFFFFFFF or not 0 <= spkid <= 255:
        print("ERROR: SSID or speaker ID outside supported range", file=sys.stderr)
        return False
    codec_name = codec.lower().strip() if codec else detect_codec_from_path(wmfw_path)
    if not codec_name and firmware_path:
        codec_name = detect_codec_from_path(firmware_path)
    if not codec_name:
        print("ERROR: Could not detect codec from firmware path; pass codec explicitly", file=sys.stderr)
        return False
    if not re.fullmatch(r'cs35l(?:41|45|51|53|56)', codec_name):
        print("ERROR: Unsupported codec name", file=sys.stderr)
        return False
    if (bincfg_l_path or bincfg_r_path) and codec_name != 'cs35l41':
        print("ERROR: BINCFG parsing is implemented only for CS35L41", file=sys.stderr)
        return False

    tuning_bytes = []
    for path in (bincfg_l_path, bincfg_r_path):
        data = Path(path).read_bytes() if path else None
        if data is not None:
            valid, message = validate_bincfg(data)
            if not valid:
                print(f"ERROR validating {path}: {message}", file=sys.stderr)
                return False
        tuning_bytes.append(data)

    with open(wmfw_path, 'rb') as f:
        wmfw_bytes = f.read()

    with open(bin_l_path, 'rb') as f:
        bin_l_bytes = f.read()

    # An explicitly supplied right-channel file must exist. Only omission permits
    # intentional reuse of left-channel tuning; a typo must not change the profile.
    if bin_r_path:
        with open(bin_r_path, 'rb') as f:
            bin_r_bytes = f.read()
    else:
        bin_r_bytes = bin_l_bytes

    wmfw_ok, wmfw_msg = validate_wmfw(wmfw_bytes)
    if not wmfw_ok:
        print(f"ERROR validating WMFW firmware: {wmfw_msg}", file=sys.stderr)
        return False
    print(f"SUCCESS: {wmfw_msg}")

    bin_l_ok, bin_l_msg = validate_bin(bin_l_bytes)
    if not bin_l_ok:
        print(f"ERROR validating primary channel coefficient: {bin_l_msg}", file=sys.stderr)
        return False
    print(f"SUCCESS Primary Coefficient: {bin_l_msg}")

    bin_r_ok, bin_r_msg = validate_bin(bin_r_bytes)
    if not bin_r_ok:
        print(f"ERROR validating secondary channel coefficient: {bin_r_msg}", file=sys.stderr)
        return False
    print(f"SUCCESS Secondary Coefficient: {bin_r_msg}")

    if firmware_path is None:
        root_dir = Path(__file__).resolve().parents[1]
        codec_dir = codec_name.upper()
        target_dir = root_dir / 'CirrusAudioFixup' / 'Devices' / codec_dir / 'Resources'
        if not target_dir.exists():
            print(f"ERROR: Resource directory for codec {codec_dir} does not exist: {target_dir}", file=sys.stderr)
            return False
        firmware_path = target_dir / "Firmware.hpp"

    with open(firmware_path, 'r', encoding='utf-8') as f:
        content = f.read()

    wmfw_var_name = f"{codec_name}_dsp1_spk_prot_{ssid_hex}_wmfw"
    bin_l_var_name = f"{codec_name}_dsp1_spk_prot_{ssid_hex}_spkid{spkid}_l0_bin"
    bin_r_var_name = f"{codec_name}_dsp1_spk_prot_{ssid_hex}_spkid{spkid}_r0_bin"

    if wmfw_var_name in content:
        print(f"Notice: {wmfw_var_name} already present in resources: {firmware_path}")

    table_symbol = f"{codec_name}Firmware"
    table_marker = f"const FirmwareResource {table_symbol}[] = {{"
    if table_marker not in content:
        print(f"ERROR: Could not find {table_symbol} in {firmware_path}", file=sys.stderr)
        return False

    split_idx = content.find(table_marker)
    pre_table = content[:split_idx]
    post_table = content[split_idx:]

    # Existing profiles are deliberate source decisions. Refuse a second entry
    # rather than reporting success while leaving old arrays/selection active.
    identity = rf'\{{\s*0x{sub_vendor:04X}\s*,\s*0x{sub_device:04X}\s*,\s*{spkid}\s*,'
    if re.search(identity, post_table, re.I):
        print("ERROR: Board/speaker entry already exists; review and replace it explicitly", file=sys.stderr)
        return False
    for name in (wmfw_var_name, bin_l_var_name, bin_r_var_name):
        if name in pre_table:
            print(f"ERROR: Resource symbol already exists: {name}", file=sys.stderr)
            return False

    arrays_code = []
    if wmfw_var_name not in pre_table:
        arrays_code.append(format_c_array(wmfw_var_name, wmfw_bytes))
    if bin_l_var_name not in pre_table:
        arrays_code.append(format_c_array(bin_l_var_name, bin_l_bytes))
    if bin_r_bytes == bin_l_bytes and bin_l_var_name in pre_table:
        pass
    elif bin_r_var_name not in pre_table and bin_r_bytes != bin_l_bytes:
        arrays_code.append(format_c_array(bin_r_var_name, bin_r_bytes))

    tuning_fields = []
    tuning_names = []
    for channel, path, data in zip(('l0', 'r0'), (bincfg_l_path, bincfg_r_path), tuning_bytes):
        symbol = f"{codec_name}_dsp1_spk_prot_{ssid_hex}_spkid{spkid}_{channel}_bincfg"
        if data is None:
            tuning_fields.extend(('nullptr', '0'))
            tuning_names.append('nullptr')
        else:
            if symbol in pre_table:
                print(f"ERROR: Resource symbol already exists: {symbol}", file=sys.stderr)
                return False
            arrays_code.append(format_c_array(symbol, data))
            tuning_fields.extend((symbol, f'sizeof({symbol})'))
            tuning_names.append(json.dumps(os.path.basename(path)))

    new_pre_table = pre_table
    if arrays_code:
        new_pre_table = pre_table + "\n" + "\n\n".join(arrays_code) + "\n\n"

    table_entry = (
        f"    {{ 0x{sub_vendor:04X}, 0x{sub_device:04X}, {spkid}, "
        f"{json.dumps(os.path.basename(wmfw_path))}, {json.dumps(os.path.basename(bin_l_path))}, "
        f"{wmfw_var_name}, sizeof({wmfw_var_name}), "
        f"{bin_l_var_name}, sizeof({bin_l_var_name}), "
        f"{bin_r_var_name if bin_r_bytes != bin_l_bytes else bin_l_var_name}, "
        f"sizeof({bin_r_var_name if bin_r_bytes != bin_l_bytes else bin_l_var_name}), false, "
        + ', '.join(tuning_fields + tuning_names) + ' }'
    )

    post_table = post_table.replace(
            table_marker + "\n",
            table_marker + "\n" + table_entry + ",\n"
        )

    updated_content = new_pre_table + post_table

    with open(firmware_path, 'w', encoding='utf-8') as f:
        f.write(updated_content)

    print(f"SUCCESS: Integrated Codec {codec_name.upper()} SSID 0x{ssid_hex} (Vendor=0x{sub_vendor:04X}, Device=0x{sub_device:04X}, spkid={spkid}) into {firmware_path}")
    return True

def main():
    parser = argparse.ArgumentParser(description="Smart Amplifier Firmware & Tuning Importer for macOS CirrusAudioFixup")
    parser.add_argument("--codec", required=False, default=None, help="Target smart amplifier codec family (e.g. cs35l41, cs35l45, cs35l51, cs35l56; default: auto-detect from firmware path)")
    parser.add_argument("--ssid", required=True, help="Subsystem ID in hex (e.g. 0x17AA3847 or 17AA3847)")
    parser.add_argument("--wmfw", required=True, help="Path to .wmfw DSP firmware file")
    parser.add_argument("--bin-l", required=True, help="Path to primary channel speaker tuning .bin file")
    parser.add_argument("--bin-r", required=False, help="Path to secondary channel speaker tuning .bin file (optional, defaults to bin-l)")
    parser.add_argument("--bincfg-l", help="Matching left-channel CS35L41 gain tuning; omitted means Linux default")
    parser.add_argument("--bincfg-r", help="Matching right-channel CS35L41 gain tuning; never implicitly copied from left")
    parser.add_argument("--spkid", type=int, default=1, help="Speaker hardware identifier (default: 1)")
    parser.add_argument("--firmware", required=False, help="Explicit path to codec firmware header (optional)")
    parser.add_argument("--validate-only", action="store_true", help="Validate firmware and tuning binaries without modifying codebase")

    args = parser.parse_args()

    codec = args.codec
    if not codec:
        codec = detect_codec_from_path(args.wmfw)
    if not codec and args.firmware:
        codec = detect_codec_from_path(args.firmware)
    if not codec:
        print("ERROR: Could not detect codec from firmware path; pass --codec explicitly", file=sys.stderr)
        sys.exit(1)

    if args.validate_only:
        with open(args.wmfw, 'rb') as f:
            w_ok, w_msg = validate_wmfw(f.read())
            print(f"DSP Firmware validation ({codec.upper()}): {w_msg}")
        with open(args.bin_l, 'rb') as f:
            b_ok, b_msg = validate_bin(f.read())
            print(f"Primary Tuning validation: {b_msg}")
        br_ok = True
        if args.bin_r:
            with open(args.bin_r, 'rb') as f:
                br_ok, br_msg = validate_bin(f.read())
                print(f"Secondary Tuning validation: {br_msg}")
        tuning_ok = True
        if (args.bincfg_l or args.bincfg_r) and codec.lower() != 'cs35l41':
            tuning_ok = False
        for path in (args.bincfg_l, args.bincfg_r):
            if path:
                valid, message = validate_bincfg(Path(path).read_bytes())
                print(f"Gain tuning validation: {message}")
                tuning_ok = tuning_ok and valid
        sys.exit(0 if (w_ok and b_ok and br_ok and tuning_ok) else 1)

    success = import_firmware(args.ssid, args.wmfw, args.bin_l, args.bin_r, args.spkid, codec, args.firmware,
                              args.bincfg_l, args.bincfg_r)
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()
