"""Exhaustively reboot across settings migration/save byte cuts.

Run: python tests/check_settings.py (requires a native C compiler).
Optional: --image path/to/256-byte-eeprom.bin smoke-migrates a temporary copy.
No device access; the supplied image is never modified.
"""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "src/app/BSP/EEPROM"
USER = ROOT / "src/app/User"
TESTS = ROOT / "tests/test_settings"
SUPPORT = ROOT / "tests/support/settings"
CALIBRATION = bytes.fromhex("0000903f0000603f0000883f0000803e000000c00000c842")


def legacy_image():
    data = bytearray(b"\xff" * 256)
    data[:2] = b"\x55\xaa"
    data[0x10:0x15] = b"\x50\x01\x00\x00\x00"
    data[0x20:0x28] = b"-nope-\r\n"
    data[0x40:0x48] = bytes((50, 1, 1, 0, 180, 0, 2, 0))
    data[0x48:0x60] = CALIBRATION
    return data


def run_checks(executable, directory, external_image):
    path = directory / "eeprom.bin"

    def boot(image, action="load", cut=-1):
        path.write_bytes(image)
        process = subprocess.run([str(executable), str(path), action, str(cut)],
                                 text=True, capture_output=True)
        assert process.returncode in (0, 99), process.stderr or process.stdout
        result = json.loads(process.stdout) if process.returncode == 0 else None
        return result, bytearray(path.read_bytes())

    original = legacy_image()
    first, migrated = boot(original)
    migration_writes = first["written"]
    saved, _ = boot(migrated, "save")
    save_writes = saved["written"]

    for cut in range(migration_writes):
        interrupted, torn = boot(original, cut=cut)
        assert interrupted is None, cut
        recovered, repaired = boot(torn)
        assert recovered["settings"] in ("OK", "MIGRATED"), (cut, recovered)
        assert recovered["brightness"] == 50, (cut, recovered)
        assert recovered["calibration"] == CALIBRATION.hex(), (cut, recovered)
        assert repaired[:0x60] == original[:0x60], cut

    for cut in range(save_writes):
        interrupted, torn = boot(migrated, "save", cut)
        assert interrupted is None, cut
        recovered, _ = boot(torn)
        assert (recovered["brightness"], recovered["display"]) in ((50, 0), (70, 1)), (cut, recovered)
        assert recovered["calibration"] == CALIBRATION.hex(), (cut, recovered)
        assert torn[:0x90] == migrated[:0x90], cut
        assert torn[0xd0:] == migrated[0xd0:], cut

    if external_image:
        raw = external_image.read_bytes()
        assert len(raw) == 256, "External EEPROM image must contain exactly 256 bytes"
        smoke, converted = boot(raw)
        assert converted[:0x60] == raw[:0x60]
        assert smoke["calibration"] == raw[0x48:0x60].hex(), smoke
        assert smoke["calibration_status"] in ("OK", "MIGRATED", "RECOVERED"), smoke
        assert external_image.read_bytes() == raw, "External EEPROM image was modified"
        print("PASS: real EEPROM backup migrated in a temporary copy:", json.dumps(smoke))

    print(f"PASS: exhaustive reboot at {migration_writes} migration and {save_writes} save byte-cut "
          "boundaries; every reboot used a fresh process")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="power-pico-settings-") as temporary:
        directory = Path(temporary)
        executable = directory / "check_settings"
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections",
            "-I", str(SUPPORT), "-I", str(STORAGE), "-I", str(USER),
            "-I", str(ROOT / "src/app/BSP/GATE"), "-I", str(USER / "Tasks/Inc"),
            str(SUPPORT / "settings_cut_runner.c"),
            str(TESTS / "settings_support.c"),
            str(STORAGE / "BL24C02.c"),
            str(USER / "user_AdcDataStrategy.c"),
            str(USER / "user_CmdStrategy.c"),
            "-Wl,--gc-sections", "-lm", "-o", str(executable),
        ], check=True)
        run_checks(executable, directory, args.image)


if __name__ == "__main__":
    main()
