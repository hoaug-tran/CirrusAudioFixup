import sys
import os
import struct
import argparse
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
    lines = [f"const uint8_t {name}[] = {{"]
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
    return "cs35l41"

def import_firmware(ssid_str, wmfw_path, bin_l_path, bin_r_path, spkid=1, codec="cs35l41", database_path=None):
    if ssid_str.lower().startswith('0x'):
        ssid_int = int(ssid_str, 16)
    else:
        ssid_int = int(ssid_str, 16)

    sub_vendor = (ssid_int >> 16) & 0xFFFF
    sub_device = ssid_int & 0xFFFF
    ssid_hex = f"{ssid_int:08x}"
    codec_name = codec.lower().strip()

    with open(wmfw_path, 'rb') as f:
        wmfw_bytes = f.read()

    with open(bin_l_path, 'rb') as f:
        bin_l_bytes = f.read()

    if bin_r_path and os.path.exists(bin_r_path):
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

    if database_path is None:
        root_dir = Path(__file__).resolve().parents[1]
        codec_dir = codec_name.upper()
        target_dir = root_dir / 'CirrusAudioFixup' / 'Devices' / codec_dir / 'Resources'
        if not target_dir.exists():
            target_dir = root_dir / 'CirrusAudioFixup' / 'Devices' / 'CS35L41' / 'Resources'
        database_path = target_dir / 'FirmwareDatabase.hpp'

    with open(database_path, 'r', encoding='utf-8') as f:
        content = f.read()

    wmfw_var_name = f"{codec_name}_dsp1_spk_prot_{ssid_hex}_wmfw"
    bin_l_var_name = f"{codec_name}_dsp1_spk_prot_{ssid_hex}_spkid{spkid}_l0_bin"
    bin_r_var_name = f"{codec_name}_dsp1_spk_prot_{ssid_hex}_spkid{spkid}_r0_bin"

    if wmfw_var_name in content:
        print(f"Notice: {wmfw_var_name} already present in database: {database_path}")

    table_marker = "const FirmwareResource firmwareTable[] = {"
    if table_marker not in content:
        print(f"ERROR: Could not find firmwareTable in {database_path}", file=sys.stderr)
        return False

    split_idx = content.find(table_marker)
    pre_table = content[:split_idx]
    post_table = content[split_idx:]

    arrays_code = []
    if wmfw_var_name not in pre_table:
        arrays_code.append(format_c_array(wmfw_var_name, wmfw_bytes))
    if bin_l_var_name not in pre_table:
        arrays_code.append(format_c_array(bin_l_var_name, bin_l_bytes))
    if bin_r_bytes == bin_l_bytes and bin_l_var_name in pre_table:
        pass
    elif bin_r_var_name not in pre_table and bin_r_bytes != bin_l_bytes:
        arrays_code.append(format_c_array(bin_r_var_name, bin_r_bytes))

    new_pre_table = pre_table
    if arrays_code:
        new_pre_table = pre_table + "\n" + "\n\n".join(arrays_code) + "\n\n"

    table_entry = (
        f"    {{ 0x{sub_vendor:04X}, 0x{sub_device:04X}, {spkid}, "
        f"\"{os.path.basename(wmfw_path)}\", \"{os.path.basename(bin_l_path)}\", "
        f"{wmfw_var_name}, sizeof({wmfw_var_name}), "
        f"{bin_l_var_name}, sizeof({bin_l_var_name}), "
        f"{bin_r_var_name if bin_r_bytes != bin_l_bytes else bin_l_var_name}, "
        f"sizeof({bin_r_var_name if bin_r_bytes != bin_l_bytes else bin_l_var_name}), false }}"
    )

    if f"0x{sub_vendor:04X}, 0x{sub_device:04X}" in post_table:
        print(f"Notice: Entry for Subsystem 0x{sub_vendor:04X}:0x{sub_device:04X} already exists in table")
    else:
        post_table = post_table.replace(
            table_marker + "\n",
            table_marker + "\n" + table_entry + ",\n"
        )

    updated_content = new_pre_table + post_table

    with open(database_path, 'w', encoding='utf-8') as f:
        f.write(updated_content)

    print(f"SUCCESS: Integrated Codec {codec_name.upper()} SSID 0x{ssid_hex} (Vendor=0x{sub_vendor:04X}, Device=0x{sub_device:04X}, spkid={spkid}) into {database_path}")
    return True

def main():
    parser = argparse.ArgumentParser(description="Smart Amplifier Firmware & Tuning Importer for macOS CirrusAudioFixup")
    parser.add_argument("--codec", required=False, default=None, help="Target smart amplifier codec family (e.g. cs35l41, cs35l45, cs35l51, cs35l56; default: auto-detect or cs35l41)")
    parser.add_argument("--ssid", required=True, help="Subsystem ID in hex (e.g. 0x17AA3847 or 17AA3847)")
    parser.add_argument("--wmfw", required=True, help="Path to .wmfw DSP firmware file")
    parser.add_argument("--bin-l", required=True, help="Path to primary channel speaker tuning .bin file")
    parser.add_argument("--bin-r", required=False, help="Path to secondary channel speaker tuning .bin file (optional, defaults to bin-l)")
    parser.add_argument("--spkid", type=int, default=1, help="Speaker hardware identifier (default: 1)")
    parser.add_argument("--database", required=False, help="Explicit path to FirmwareDatabase.hpp (optional)")
    parser.add_argument("--validate-only", action="store_true", help="Validate firmware and tuning binaries without modifying codebase")

    args = parser.parse_args()

    codec = args.codec
    if not codec:
        codec = detect_codec_from_path(args.wmfw)

    if args.validate_only:
        with open(args.wmfw, 'rb') as f:
            w_ok, w_msg = validate_wmfw(f.read())
            print(f"DSP Firmware validation ({codec.upper()}): {w_msg}")
        with open(args.bin_l, 'rb') as f:
            b_ok, b_msg = validate_bin(f.read())
            print(f"Primary Tuning validation: {b_msg}")
        if args.bin_r:
            with open(args.bin_r, 'rb') as f:
                br_ok, br_msg = validate_bin(f.read())
                print(f"Secondary Tuning validation: {br_msg}")
        sys.exit(0 if (w_ok and b_ok) else 1)

    success = import_firmware(args.ssid, args.wmfw, args.bin_l, args.bin_r, args.spkid, codec, args.database)
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()
